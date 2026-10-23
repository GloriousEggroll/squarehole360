#include "shader_recompiler.h"
#include "retained_opaque_phase_compiler.h"
#include "retained_opaque_phase_module.h"
#include "retained_opaque_phase_shader.h"
#include "native_shader_embedded.h"
#include <algorithm>
#include <unordered_map>
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
    // Resolve pointer storage classes from actual result types before checking
    // stores. This also covers access chains, phi/select and UToPtr results.
    std::unordered_map<uint32_t,uint32_t> pointer_types,pointers;
    for(size_t at=5;at<code.size();) {
        const uint32_t n=code[at]>>16,op=code[at]&65535;
        if(!n||n>code.size()-at)return false;
        if(op==32) {if(n!=4)return false;pointer_types.emplace(code[at+1],code[at+2]);}
        at+=n;
    }
    for(size_t at=5;at<code.size();) {
        const uint32_t n=code[at]>>16,op=code[at]&65535;
        // Only actual typed-result pointer producers. A pointer type's own
        // result id or an annotation operand is not a value's result type.
        const bool typed=op==1||op==46||op==55||op==57||op==59||op==60||op==61||
            op==65||op==66||op==67||op==70||op==83||op==120||op==124||op==169||op==245;
        if(typed&&n>=3)if(const auto type=pointer_types.find(code[at+1]);type!=pointer_types.end())
            pointers.emplace(code[at+2],type->second);
        at+=n;
    }
    unsigned entries=0;bool depth=false;
    for(size_t at=5;at<code.size();) {
        const uint32_t n=code[at]>>16,op=code[at]&65535;
        if(!n||n>code.size()-at)return false;
        if(op==15){if(n<4||code[at+1]!=4)return false;++entries;}
        if(op==16&&n>=3){if(code[at+2]==9)return false;depth|=code[at+2]==12;}
        if(op==17&&n==2&&code[at+1]==5378)return false;
        if(op==71&&n==4&&code[at+2]==34&&code[at+3]==4)return false;
        if(op==59&&n>=4&&code[at+3]==12)return false;
        if(op==5364||op==5365||op==224||op==225||op==63||op==64||op==99||
           (op>=227&&op<=242)||op==318||op==319||op==331||op==5614||op==5615||op==6035)return false;
        if(op==62) {
            if(n<3)return false;
            const auto pointer=pointers.find(code[at+1]);
            // Only shader-local/private or the authenticated raster outputs
            // may be written; physical, storage, uniform and image stores fail.
            if(pointer==pointers.end()||(pointer->second!=3&&pointer->second!=6&&pointer->second!=7))return false;
        }
        at+=n;
    }
    return entries==1&&depth;
}
bool ConservativeFragmentSpirv(std::span<const uint32_t> code) {
    // Keep the V3 effect/pointer checks, then independently authenticate the
    // execution modes on the actual single fragment entry. This is a closed
    // mode shape, not permission to add early side effects or a new depth ABI.
    if(!FragmentSpirv(code))return false;
    uint32_t entry=0;
    for(size_t at=5;at<code.size();at+=code[at]>>16)
        if((code[at]&65535)==15)entry=code[at+2];
    if(!entry||entry>=code[3])return false;
    uint32_t modes=0;
    for(size_t at=5;at<code.size();at+=code[at]>>16) {
        const uint32_t n=code[at]>>16,op=code[at]&65535;
        if(op!=16)continue;
        if(n!=3||code[at+1]!=entry)return false;
        uint32_t bit=0;
        switch(code[at+2]) {
        case 7:bit=1;break;   // OriginUpperLeft.
        case 12:bit=2;break;  // DepthReplacing.
        case 15:bit=4;break;  // DepthLess (inclusive <= bound).
        default:return false; // Includes EarlyFragmentTests/other depth modes.
        }
        if(modes&bit)return false;
        modes|=bit;
    }
    return modes==7;
}
std::vector<uint32_t> Compile(std::string_view source,bool conservative=false) {
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
    Require(conservative?ConservativeFragmentSpirv(words):FragmentSpirv(words),
        "Packed attachment shader ABI/depth/interlock shape invalid");return words;
}
}
std::string NativeRetainedOpaquePhaseCompilerVersion() {
    return std::string(NativeRetainedOpaquePhaseBytecodeVersion());
}
std::shared_ptr<const NativeRetainedOpaquePhaseProgram> TryPrepareNativeRetainedOpaquePhase(
    NativeShaderCompiler& compiler,std::span<const uint8_t> container) {
    if(container.size()<36||container.size()>16u*1024*1024||
       compiler.CacheVersion()!="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b")return nullptr;
    const auto digest=Hash(container);
    if(!NativeRetainedOpaquePhasePixelSupported(digest))return nullptr;
    const auto canonical=compiler.Get(container,NativeShaderMode::FragmentInterlock);
    const auto canonical_bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(canonical->spirv.data()),canonical->spirv.size()*4);
    if(canonical->stage!=NativeShaderStage::Pixel||canonical->mode!=NativeShaderMode::FragmentInterlock||
       canonical->source_sha256!=digest||canonical->specialization_mask!=1794||
       !NativeRetainedOpaquePhaseCanonicalPixelCertified(digest,canonical_bytes))return nullptr;
    std::vector<uint32_t> aligned((container.size()+3)/4);std::memcpy(aligned.data(),container.data(),container.size());
    ShaderRecompiler recompiler;recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),kNativeShaderCommon);
    Require(recompiler.specConstantsMask==canonical->specialization_mask,"Opaque phase compiler changed guest specialization ABI");
    const auto adapted=TryNativeRetainedOpaquePhaseSource(recompiler.out,kNativeShaderCommon);
    Require(bool(adapted),"Opaque phase reversible terminal/packing source proof failed");
    auto spirv=Compile(*adapted);
    const auto bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(spirv.data()),spirv.size()*4);
    Require(NativeRetainedOpaquePhaseModuleCertified(digest,true,NativeShaderVariant::RetainedOpaquePhaseV3,bytes),
        "Opaque phase compiled module differs from audited byte cohort");
    const auto hash=Hash(bytes);
    return std::make_shared<const NativeRetainedOpaquePhaseProgram>(NativeRetainedOpaquePhaseProgram{
        digest,compiler.CacheVersion(),NativeRetainedOpaquePhaseCompilerVersion(),hash,
        canonical->specialization_mask,std::move(spirv)});
}
std::string NativeRetainedOpaquePhaseConservativeCompilerVersion() {
    return std::string(NativeRetainedOpaquePhaseConservativeBytecodeVersion());
}
std::shared_ptr<const NativeRetainedOpaquePhaseProgram> TryPrepareNativeRetainedOpaquePhaseConservative(
    NativeShaderCompiler& compiler,std::span<const uint8_t> container) {
    if(container.size()<36||container.size()>16u*1024*1024||
       compiler.CacheVersion()!="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b")return nullptr;
    const auto digest=Hash(container);
    if(!NativeRetainedOpaquePhasePixelSupported(digest))return nullptr;
    const auto canonical=compiler.Get(container,NativeShaderMode::FragmentInterlock);
    const auto canonical_bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(canonical->spirv.data()),canonical->spirv.size()*4);
    if(canonical->stage!=NativeShaderStage::Pixel||canonical->mode!=NativeShaderMode::FragmentInterlock||
       canonical->source_sha256!=digest||canonical->specialization_mask!=1794||
       !NativeRetainedOpaquePhaseCanonicalPixelCertified(digest,canonical_bytes))return nullptr;
    std::vector<uint32_t> aligned((container.size()+3)/4);std::memcpy(aligned.data(),container.data(),container.size());
    ShaderRecompiler recompiler;recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),kNativeShaderCommon);
    Require(recompiler.specConstantsMask==canonical->specialization_mask,"Opaque phase compiler changed guest specialization ABI");
    const auto adapted=TryNativeRetainedOpaquePhaseConservativeSource(recompiler.out,kNativeShaderCommon);
    Require(bool(adapted),"Opaque phase reversible terminal/packing source proof failed");
    auto spirv=Compile(*adapted,true);
    const auto bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(spirv.data()),spirv.size()*4);
    Require(NativeRetainedOpaquePhaseModuleCertified(digest,true,NativeShaderVariant::RetainedOpaquePhaseConservativeV4,bytes),
        "Opaque phase compiled module differs from audited byte cohort");
    const auto hash=Hash(bytes);
    return std::make_shared<const NativeRetainedOpaquePhaseProgram>(NativeRetainedOpaquePhaseProgram{
        digest,compiler.CacheVersion(),NativeRetainedOpaquePhaseConservativeCompilerVersion(),hash,
        canonical->specialization_mask,std::move(spirv)});
}
} // namespace gow3::renderer
