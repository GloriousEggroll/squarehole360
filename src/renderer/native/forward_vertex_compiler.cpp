#include "renderer/native/forward_vertex_compiler.h"
#include "native_shader_embedded.h"
#include "native_forward_vertex_shader_version.h"
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
bool VertexSpirv(std::span<const uint32_t> code) {
    if(code.size()<5||code.size_bytes()>kMaxSpirvBytes||code[0]!=0x07230203||
       code[1]<0x00010000||code[1]>0x00010600||!code[3]||code[4])return false;
    bool entry=false;
    for(size_t at=5;at<code.size();) {
        const uint32_t count=code[at]>>16,opcode=code[at]&65535;
        if(!count||count>code.size()-at)return false;
        if(opcode==15) {if(count<4||code[at+1]!=0)return false;entry=true;}
        if(opcode==17&&count==2&&code[at+1]==5378)return false;
        at+=count;
    }
    return entry;
}
std::vector<uint32_t> Compile(const std::string& source) {
    IDxcCompiler3* raw_compiler=nullptr;
    const auto created=DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&raw_compiler));
    Com<IDxcCompiler3> compiler(raw_compiler);
    Require(SUCCEEDED(created)&&compiler,"Cannot initialize forward-vs DXC");
    const DxcBuffer buffer{source.data(),source.size(),DXC_CP_UTF8};
    // Identical to canonical Raster-stage VS flags, including Vulkan Y inversion.
    std::array<const wchar_t*,9> arguments{
        L"-T",L"vs_6_0",L"-E",L"main",L"-HV",L"2021",L"-spirv",L"-fvk-use-dx-layout",L"-fvk-invert-y"};
    IDxcResult* raw_result=nullptr;
    const auto compiled=compiler->Compile(&buffer,arguments.data(),uint32_t(arguments.size()),nullptr,IID_PPV_ARGS(&raw_result));
    Com<IDxcResult> result(raw_result);
    Require(SUCCEEDED(compiled)&&result,"Forward-VS DXC invocation failed");
    HRESULT status=E_FAIL;
    Require(SUCCEEDED(result->GetStatus(&status)),"Forward-VS DXC status unavailable");
    if(FAILED(status)) {
        IDxcBlobUtf8* raw_errors=nullptr;
        result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&raw_errors),nullptr);
        Com<IDxcBlobUtf8> errors(raw_errors);
        throw std::runtime_error(std::string("Forward-VS SPIR-V compile failed: ")+
            (errors?std::string(errors->GetStringPointer(),errors->GetStringLength()):"No diagnostics"));
    }
    IDxcBlob* raw_object=nullptr;
    const auto obtained=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&raw_object),nullptr);
    Com<IDxcBlob> object(raw_object);
    Require(SUCCEEDED(obtained)&&object&&object->GetBufferSize()>=20&&
        object->GetBufferSize()<=kMaxSpirvBytes&&object->GetBufferSize()%4==0,
        "Forward-VS DXC object is invalid");
    std::vector<uint32_t> words(object->GetBufferSize()/4);
    std::memcpy(words.data(),object->GetBufferPointer(),object->GetBufferSize());
    Require(VertexSpirv(words),"Forward-VS vertex SPIR-V is malformed");
    return words;
}
}
std::string ForwardVertexCompilerVersion() {return kNativeForwardVertexShaderVersion;}
std::shared_ptr<const ForwardVertexProgram> TryPrepareForwardVertex(
    NativeShaderCompiler& compiler,std::span<const uint8_t> source) {
    if(source.size()<36||source.size()>16u*1024*1024||Hash(source)!=kForwardVertexSource)
        return nullptr;
    // This validates the pinned vertex container before a canonical cache hit.
    // All declarations, vertex fetch metadata and outputs retain the Raster ABI.
    const auto canonical=compiler.Get(source,NativeShaderMode::Raster);
    if(canonical->stage!=NativeShaderStage::Vertex||
       canonical->source_sha256!=kForwardVertexSource||
       compiler.CacheVersion()!=kForwardVertexBaselineCompiler)return nullptr;
    const auto analyzed=AnalyzeForwardVertexControlFlow(source);
    if(!analyzed)return nullptr;
    std::vector<uint32_t> aligned((source.size()+3)/4);
    std::memcpy(aligned.data(),source.data(),source.size());
    ShaderRecompiler recompiler;
    recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),kNativeShaderCommon);
    Require(recompiler.specConstantsMask==canonical->specialization_mask,
        "Forward-VS changed shader specialization ABI");
    const auto modified=TryForwardVertexControlFlowSource(recompiler.out,*analyzed);
    if(!modified)return nullptr;
    auto program=std::make_shared<ForwardVertexProgram>();
    program->source_sha256=canonical->source_sha256;
    program->compiler_version=compiler.CacheVersion();
    program->variant_version=ForwardVertexCompilerVersion();
    program->modified_hlsl_sha256=Hash({reinterpret_cast<const uint8_t*>(modified->data()),modified->size()});
    program->baseline_hlsl_sha256=Hash({reinterpret_cast<const uint8_t*>(recompiler.out.data()),recompiler.out.size()});
    program->control_instructions=uint32_t(analyzed->clauses.size());
    program->terminal=analyzed->terminal;program->padding_clauses=uint32_t(analyzed->clauses.size()-1-analyzed->terminal);
    program->specialization_mask=recompiler.specConstantsMask;
    program->spirv=Compile(*modified);
    program->compiled_spirv_sha256=Hash({reinterpret_cast<const uint8_t*>(program->spirv.data()),program->spirv.size()*4});
    Require(program->spirv!=canonical->spirv,"Forward-VS output equals canonical shader");
    return program;
}
} // namespace gow3::renderer
