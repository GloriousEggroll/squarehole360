#include "stencil_material_cutoff_compiler.h"
#include "content_hash.h"
#include "native_shader_embedded.h"
#include "native_stencil_material_version.h"
#include "shader_recompiler.h"
#include <dxcapi.h>
#include <array>
#include <cstring>
#include <stdexcept>
namespace gow3::renderer {
namespace {
constexpr size_t kMaximumSpirvBytes=256u*1024;
void Require(bool v,const char* text){if(!v)throw std::runtime_error(text);}
std::string Hash(std::string_view t){return ContentHash({reinterpret_cast<const uint8_t*>(t.data()),t.size()});}
template<class T>struct Release {void operator()(T* pointer) const {if(pointer)pointer->Release();}};
template<class T>using Com=std::unique_ptr<T,Release<T>>;
std::vector<uint32_t> Compile(const std::string& source,NativeStencilMaterialMode mode) {
    IDxcCompiler3* raw=nullptr;
    const auto initialized=DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&raw));Com<IDxcCompiler3> compiler(raw);
    Require(SUCCEEDED(initialized)&&compiler,"Cannot initialize Stencil-material DXC");
    const DxcBuffer buffer{source.data(),source.size(),DXC_CP_UTF8};
    // Byte-identical canonical FSI pixel compilation arguments.
    std::array<const wchar_t*,14> arguments{L"-T",L"ps_6_0",L"-E",L"main",L"-HV",L"2021",L"-spirv",L"-fvk-use-dx-layout",
        L"-D",L"GOW3_NATIVE_FSI=1",L"-fspv-target-env=vulkan1.2",L"-fspv-extension=SPV_EXT_fragment_shader_interlock",
        L"-fspv-extension=SPV_EXT_descriptor_indexing",L"-fspv-extension=SPV_KHR_physical_storage_buffer"};
    IDxcResult* raw_result=nullptr;
    const auto compiled=compiler->Compile(&buffer,arguments.data(),uint32_t(arguments.size()),nullptr,IID_PPV_ARGS(&raw_result));
    Com<IDxcResult> result(raw_result);Require(SUCCEEDED(compiled)&&result,"Stencil-material DXC invocation failed");
    HRESULT status=E_FAIL;Require(SUCCEEDED(result->GetStatus(&status)),"Stencil-material DXC status unavailable");
    if(FAILED(status)) {
        IDxcBlobUtf8* raw_errors=nullptr;result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&raw_errors),nullptr);Com<IDxcBlobUtf8> errors(raw_errors);
        throw std::runtime_error(std::string("Stencil-material SPIR-V compile failed: ")+
            (errors?std::string(errors->GetStringPointer(),errors->GetStringLength()):"No diagnostics"));
    }
    IDxcBlob* raw_object=nullptr;const auto obtained=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&raw_object),nullptr);Com<IDxcBlob> object(raw_object);
    Require(SUCCEEDED(obtained)&&object&&object->GetBufferSize()>=20&&object->GetBufferSize()<=kMaximumSpirvBytes&&
        object->GetBufferSize()%4==0,"Stencil-material DXC object invalid");
    std::vector<uint32_t> words(object->GetBufferSize()/4);std::memcpy(words.data(),object->GetBufferPointer(),object->GetBufferSize());
    Require(NativeStencilMaterialDerivedCertified(words,mode),"Stencil-material fragment SPIR-V malformed");return words;
}

}
std::string NativeStencilMaterialCompilerVersion(){return kNativeStencilMaterialVersion;}
std::shared_ptr<const NativeStencilMaterialProgram> CompileNativeStencilMaterial(
    NativeShaderCompiler& compiler,std::span<const uint8_t> source,NativeStencilMaterialMode mode) {
    Require(mode==NativeStencilMaterialMode::Observe||mode==NativeStencilMaterialMode::Cutoff||mode==NativeStencilMaterialMode::Uninstrumented,"Stencil material mode refused");
    // Own source before authentication, and decline foreign containers before
    // asking the canonical compiler to process them.
    Require(source.size()==764,"Stencil material exact source size refused");
    std::vector<uint32_t> owned((source.size()+3)/4);
    std::memcpy(owned.data(),source.data(),source.size());
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(owned.data()),source.size());
    Require(ContentHash(bytes)==kStencilMaterialPixel,"Stencil material source refused (fb9aabe excluded)");
    Require(NativeShaderCompiler::CacheVersion()==kStencilMaterialCanonicalVersion&&
        Hash(kNativeShaderCommon)==kStencilMaterialCommonSha,"Stencil material canonical compiler/common identity refused");
    const auto original=compiler.Get(bytes,NativeShaderMode::FragmentInterlock);
    Require(original&&original->stage==NativeShaderStage::Pixel&&original->mode==NativeShaderMode::FragmentInterlock&&
        original->source_sha256==kStencilMaterialPixel&&NativeStencilMaterialCanonicalCertified(original->spirv,false),
        "Stencil material original binary/derivative certificate refused");
    ShaderRecompiler canonical;canonical.recompile(bytes.data(),kNativeShaderCommon);
    Require(canonical.specConstantsMask==original->specialization_mask,"Stencil material specialization mask changed");
    const auto candidate=TryNativeStencilMaterialSource(canonical.out,mode);
    Require(candidate.has_value(),"Stencil material source shape refused");
    const auto restored=RestoreNativeStencilMaterialSource(*candidate,mode);
    Require(restored&&*restored==canonical.out,"Stencil material full-source restoration refused");
    auto result=std::make_shared<NativeStencilMaterialProgram>();result->mode=mode;
    result->source=original->source_sha256;result->canonical_version=NativeShaderCompiler::CacheVersion();
    result->derived_version=NativeStencilMaterialCompilerVersion();result->specialization_mask=original->specialization_mask;
    result->spirv=Compile(*candidate,mode);result->Authenticate();return result;
}
} // namespace gow3::renderer
