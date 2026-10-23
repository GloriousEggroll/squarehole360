#include "shader_recompiler.h"
#include "retained_opaque_compiler.h"
#include "retained_opaque_writer.h"
#include "native_packed_opaque_shader.h"
#include "native_shader_embedded.h"
#include "native_retained_opaque_shader_version.h"
#include <dxcapi.h>
#include <picosha2.h>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
namespace gow3::renderer {
namespace {
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class T>struct ComRelease {void operator()(T* pointer)const{if(pointer)pointer->Release();}};
template<class T>using Com=std::unique_ptr<T,ComRelease<T>>;
std::string Hash(std::span<const uint8_t> bytes) {return picosha2::hash256_hex_string(bytes.begin(),bytes.end());}
bool FragmentSpirv(std::span<const uint32_t> code) {
    if(code.size()<5||code.size_bytes()>16u*1024*1024||code[0]!=0x07230203||
       code[1]<0x10000||code[1]>0x10600||!code[3]||code[4])return false;
    unsigned entries=0;bool depth=false;
    for(size_t at=5;at<code.size();) {
        const uint32_t n=code[at]>>16,op=code[at]&65535;
        if(!n||n>code.size()-at)return false;
        if(op==15){if(n<4||code[at+1]!=4)return false;++entries;}
        if(op==16&&n>=3){if(code[at+2]==9)return false;depth|=code[at+2]==12;}
        if(op==17&&n==2&&code[at+1]==5378)return false;
        if(op==71&&n==4&&code[at+2]==34&&code[at+3]==4)return false;
        if(op==59&&n>=4&&code[at+3]==12)return false;
        if(op==5364||op==5365||op==224||op==225)return false;
        at+=n;
    }
    return entries==1&&depth;
}
std::vector<uint32_t> Compile(std::string_view source) {
    IDxcCompiler3* raw=nullptr;
    const auto created=DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&raw));
    Com<IDxcCompiler3> compiler(raw);Require(SUCCEEDED(created)&&compiler,"Cannot initialize packed attachment DXC");
    const DxcBuffer buffer{source.data(),source.size(),DXC_CP_UTF8};
    // Keep the canonical material compilation flags. The FSI macro retains
    // the original entry derivatives; the interlock extension is unnecessary.
    std::array<const wchar_t*,13> arguments{L"-T",L"ps_6_0",L"-E",L"main",L"-HV",L"2021",
        L"-spirv",L"-fvk-use-dx-layout",L"-D",L"GOW3_NATIVE_FSI=1",L"-fspv-target-env=vulkan1.2",
        L"-fspv-extension=SPV_EXT_descriptor_indexing",L"-fspv-extension=SPV_KHR_physical_storage_buffer"};
    IDxcResult* raw_result=nullptr;
    const auto compiled=compiler->Compile(&buffer,arguments.data(),uint32_t(arguments.size()),nullptr,IID_PPV_ARGS(&raw_result));
    Com<IDxcResult> result(raw_result);Require(SUCCEEDED(compiled)&&result,"Packed attachment DXC invocation failed");
    HRESULT status=E_FAIL;Require(SUCCEEDED(result->GetStatus(&status)),"Packed attachment DXC status unavailable");
    if(FAILED(status)) {
        IDxcBlobUtf8* raw_errors=nullptr;result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&raw_errors),nullptr);
        Com<IDxcBlobUtf8> errors(raw_errors);
        throw std::runtime_error(std::string("Packed attachment shader compilation failed: ")+
            (errors?std::string(errors->GetStringPointer(),errors->GetStringLength()):"No diagnostics"));
    }
    IDxcBlob* raw_object=nullptr;
    const auto obtained=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&raw_object),nullptr);
    Com<IDxcBlob> object(raw_object);
    Require(SUCCEEDED(obtained)&&object&&object->GetBufferSize()>=20&&object->GetBufferSize()<=16u*1024*1024&&
        object->GetBufferSize()%4==0,"Packed attachment DXC object invalid");
    std::vector<uint32_t> words(object->GetBufferSize()/4);
    std::memcpy(words.data(),object->GetBufferPointer(),object->GetBufferSize());
    Require(FragmentSpirv(words),"Packed attachment shader ABI/depth/interlock shape invalid");return words;
}
}
std::string NativeRetainedOpaqueCompilerVersion(){return kNativeRetainedOpaqueShaderVersion;}
std::shared_ptr<const NativeRetainedOpaqueProgram> TryPrepareNativeRetainedOpaque(
    NativeShaderCompiler& compiler,std::span<const uint8_t> container) {
    if(container.size()<36||container.size()>16u*1024*1024||Hash(container)!=kNativePackedOpaquePixel||
       compiler.CacheVersion()!="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b")return nullptr;
    const auto canonical=compiler.Get(container,NativeShaderMode::FragmentInterlock);
    if(canonical->stage!=NativeShaderStage::Pixel||canonical->mode!=NativeShaderMode::FragmentInterlock||
       canonical->source_sha256!=kNativePackedOpaquePixel)return nullptr;
    std::vector<uint32_t> aligned((container.size()+3)/4);std::memcpy(aligned.data(),container.data(),container.size());
    ShaderRecompiler recompiler;recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),kNativeShaderCommon);
    Require(recompiler.specConstantsMask==canonical->specialization_mask,"Packed attachment compiler changed guest specialization ABI");
    const auto adapted=TryNativePackedOpaqueSource(recompiler.out,kNativeShaderCommon);
    Require(bool(adapted),"Packed attachment pinned terminal/packing source proof failed");
    auto program=std::make_shared<NativeRetainedOpaqueProgram>();
    program->source_sha256=canonical->source_sha256;program->vertex_sha256=kNativePackedOpaqueVertex;
    program->baseline_version=compiler.CacheVersion();program->variant_version=NativeRetainedOpaqueCompilerVersion();
    program->specialization_mask=canonical->specialization_mask;program->spirv=Compile(*adapted);
    program->spirv_sha256=Hash({reinterpret_cast<const uint8_t*>(program->spirv.data()),program->spirv.size()*4});
    Require(program->spirv!=canonical->spirv,"Packed attachment module reused canonical output");return program;
}
}
