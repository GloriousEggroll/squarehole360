#include "renderer/native/opaque_color_overwrite_compiler.h"
#include "native_shader_embedded.h"
#include "native_opaque_shader_version.h"
#include "shader_recompiler.h"
#include <dxcapi.h>
#include <picosha2.h>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace gow3::renderer {
namespace {
constexpr size_t kMaxSpirvBytes=16u*1024*1024;
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class T>struct ComRelease {void operator()(T* pointer) const {if(pointer)pointer->Release();}};
template<class T>using Com=std::unique_ptr<T,ComRelease<T>>;
std::string Hash(std::span<const uint8_t> bytes) {
    return picosha2::hash256_hex_string(bytes.begin(),bytes.end());
}
bool FragmentSpirv(std::span<const uint32_t> code) {
    if(code.size()<5||code.size_bytes()>kMaxSpirvBytes||code[0]!=0x07230203||
       code[1]<0x00010000||code[1]>0x00010600||!code[3]||code[4])return false;
    bool entry=false,interlock=false;
    for(size_t at=5;at<code.size();) {
        const uint32_t count=code[at]>>16,opcode=code[at]&65535;
        if(!count||count>code.size()-at)return false;
        if(opcode==15) {if(count<4||code[at+1]!=4)return false;entry=true;}
        if(opcode==17&&count==2&&code[at+1]==5378)interlock=true;
        at+=count;
    }
    return entry&&interlock;
}
std::vector<uint32_t> Compile(const std::string& source) {
    IDxcCompiler3* raw_compiler=nullptr;
    const auto created=DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&raw_compiler));
    Com<IDxcCompiler3> compiler(raw_compiler);
    Require(SUCCEEDED(created)&&compiler,"Cannot initialize opaque-overwrite DXC");
    const DxcBuffer buffer{source.data(),source.size(),DXC_CP_UTF8};
    // Identical to CompileSpirv's canonical FSI pixel arguments. The
    // fragment-interlock macro is required by the unchanged generated shader.
    std::array<const wchar_t*,14> arguments{
        L"-T",L"ps_6_0",L"-E",L"main",L"-HV",L"2021",L"-spirv",L"-fvk-use-dx-layout",
        L"-D",L"GOW3_NATIVE_FSI=1",L"-fspv-target-env=vulkan1.2",
        L"-fspv-extension=SPV_EXT_fragment_shader_interlock",
        L"-fspv-extension=SPV_EXT_descriptor_indexing",
        L"-fspv-extension=SPV_KHR_physical_storage_buffer"};
    IDxcResult* raw_result=nullptr;
    const auto compiled=compiler->Compile(&buffer,arguments.data(),uint32_t(arguments.size()),nullptr,IID_PPV_ARGS(&raw_result));
    Com<IDxcResult> result(raw_result);
    Require(SUCCEEDED(compiled)&&result,"Opaque-overwrite DXC invocation failed");
    HRESULT status=E_FAIL;
    Require(SUCCEEDED(result->GetStatus(&status)),"Opaque-overwrite DXC status unavailable");
    if(FAILED(status)) {
        IDxcBlobUtf8* raw_errors=nullptr;
        result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&raw_errors),nullptr);
        Com<IDxcBlobUtf8> errors(raw_errors);
        throw std::runtime_error(std::string("Opaque-overwrite SPIR-V compile failed: ")+
            (errors?std::string(errors->GetStringPointer(),errors->GetStringLength()):"No diagnostics"));
    }
    IDxcBlob* raw_object=nullptr;
    const auto obtained=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&raw_object),nullptr);
    Com<IDxcBlob> object(raw_object);
    Require(SUCCEEDED(obtained)&&object&&object->GetBufferSize()>=20&&
        object->GetBufferSize()<=kMaxSpirvBytes&&object->GetBufferSize()%4==0,
        "Opaque-overwrite DXC object is invalid");
    std::vector<uint32_t> words(object->GetBufferSize()/4);
    std::memcpy(words.data(),object->GetBufferPointer(),object->GetBufferSize());
    Require(FragmentSpirv(words),"Opaque-overwrite fragment SPIR-V is malformed");
    return words;
}
}
std::string OpaqueColorOverwriteCompilerVersion() {return kNativeOpaqueShaderVersion;}
std::shared_ptr<const OpaqueColorOverwriteProgram> TryPrepareOpaqueColorOverwrite(
    NativeShaderCompiler& compiler,std::span<const uint8_t> source) {
    // This validates the actual container before a canonical cache hit. The
    // same source must already be a usable ordinary Native FSI pixel program.
    const auto canonical=compiler.Get(source,NativeShaderMode::FragmentInterlock);
    if(canonical->stage!=NativeShaderStage::Pixel||
       canonical->source_sha256!=kOpaqueColorOverwritePixel||
       compiler.CacheVersion()!=kOpaqueColorOverwriteBaselineCompiler)return nullptr;
    auto modified=TryOpaqueColorOverwriteSource(kNativeShaderCommon);
    if(!modified)return nullptr;
    std::vector<uint32_t> aligned((source.size()+3)/4);
    std::memcpy(aligned.data(),source.data(),source.size());
    ShaderRecompiler recompiler;
    recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),modified->c_str());
    Require(recompiler.specConstantsMask==canonical->specialization_mask,
        "Opaque-overwrite changed shader specialization ABI");
    auto program=std::make_shared<OpaqueColorOverwriteProgram>();
    program->source_sha256=canonical->source_sha256;
    program->compiler_version=compiler.CacheVersion();
    program->variant_version=OpaqueColorOverwriteCompilerVersion();
    program->modified_common_sha256=Hash({reinterpret_cast<const uint8_t*>(modified->data()),modified->size()});
    program->specialization_mask=recompiler.specConstantsMask;
    program->spirv=Compile(recompiler.out);
    program->compiled_spirv_sha256=Hash({reinterpret_cast<const uint8_t*>(program->spirv.data()),program->spirv.size()*4});
    Require(program->spirv!=canonical->spirv,"Opaque-overwrite output equals canonical shader");
    return program;
}
} // namespace gow3::renderer
