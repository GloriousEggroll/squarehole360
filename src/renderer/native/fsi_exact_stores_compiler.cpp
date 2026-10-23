#include "renderer/native/fsi_exact_stores_compiler.h"
#include "renderer/native/fsi_exact_stores.h"
#include "native_shader_embedded.h"
#include "native_fsi_exact_stores_version.h"
#include "shader_recompiler.h"
#include <dxcapi.h>
#include <picosha2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace gow3::renderer {
namespace {
constexpr size_t kMaximumSpirvBytes=16u*1024*1024;
constexpr size_t kHeaderBytes=144;
constexpr std::array<uint8_t,8> kMagic{'G','3','E','S','T','V','0','1'};
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
std::string Hash(std::span<const uint8_t> bytes) {return picosha2::hash256_hex_string(bytes.begin(),bytes.end());}
std::string Hash(std::string_view text) {return Hash({reinterpret_cast<const uint8_t*>(text.data()),text.size()});}
template<class T>struct Release {void operator()(T* pointer) const {if(pointer)pointer->Release();}};
template<class T>using Com=std::unique_ptr<T,Release<T>>;
bool FragmentSpirv(std::span<const uint32_t> words) {
    if(words.size()<5||words.size_bytes()>kMaximumSpirvBytes||words[0]!=0x07230203||
       words[1]<0x00010000||words[1]>0x00010600||!words[3]||words[4])return false;
    bool entry=false,interlock=false;
    for(size_t at=5;at<words.size();) {
        const auto count=words[at]>>16,opcode=words[at]&65535u;
        if(!count||count>words.size()-at)return false;
        if(opcode==15) {if(count<4||words[at+1]!=4)return false;entry=true;}
        if(opcode==17&&count==2&&words[at+1]==5378)interlock=true;
        at+=count;
    }
    return entry&&interlock;
}
std::vector<uint32_t> Compile(const std::string& source) {
    IDxcCompiler3* raw=nullptr;
    const auto initialized=DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&raw));Com<IDxcCompiler3> compiler(raw);
    Require(SUCCEEDED(initialized)&&compiler,"Cannot initialize exact-store DXC");
    const DxcBuffer buffer{source.data(),source.size(),DXC_CP_UTF8};
    // Byte-identical canonical FSI pixel compilation arguments.
    std::array<const wchar_t*,14> arguments{L"-T",L"ps_6_0",L"-E",L"main",L"-HV",L"2021",L"-spirv",L"-fvk-use-dx-layout",
        L"-D",L"GOW3_NATIVE_FSI=1",L"-fspv-target-env=vulkan1.2",L"-fspv-extension=SPV_EXT_fragment_shader_interlock",
        L"-fspv-extension=SPV_EXT_descriptor_indexing",L"-fspv-extension=SPV_KHR_physical_storage_buffer"};
    IDxcResult* raw_result=nullptr;
    const auto compiled=compiler->Compile(&buffer,arguments.data(),uint32_t(arguments.size()),nullptr,IID_PPV_ARGS(&raw_result));
    Com<IDxcResult> result(raw_result);Require(SUCCEEDED(compiled)&&result,"Exact-store DXC invocation failed");
    HRESULT status=E_FAIL;Require(SUCCEEDED(result->GetStatus(&status)),"Exact-store DXC status unavailable");
    if(FAILED(status)) {
        IDxcBlobUtf8* raw_errors=nullptr;result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&raw_errors),nullptr);Com<IDxcBlobUtf8> errors(raw_errors);
        throw std::runtime_error(std::string("Exact-store SPIR-V compile failed: ")+
            (errors?std::string(errors->GetStringPointer(),errors->GetStringLength()):"No diagnostics"));
    }
    IDxcBlob* raw_object=nullptr;const auto obtained=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&raw_object),nullptr);Com<IDxcBlob> object(raw_object);
    Require(SUCCEEDED(obtained)&&object&&object->GetBufferSize()>=20&&object->GetBufferSize()<=kMaximumSpirvBytes&&
        object->GetBufferSize()%4==0,"Exact-store DXC object invalid");
    std::vector<uint32_t> words(object->GetBufferSize()/4);std::memcpy(words.data(),object->GetBufferPointer(),object->GetBufferSize());
    Require(FragmentSpirv(words),"Exact-store fragment SPIR-V malformed");return words;
}
uint32_t Le32(std::span<const uint8_t> data,size_t at) {
    return uint32_t(data[at])|uint32_t(data[at+1])<<8|uint32_t(data[at+2])<<16|uint32_t(data[at+3])<<24;
}
void Put32(std::vector<uint8_t>& data,uint32_t value) {for(unsigned shift=0;shift<32;shift+=8)data.push_back(uint8_t(value>>shift));}
bool Load(const std::filesystem::path& path,CompiledNativeShader& shader) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return false;
    const auto length=input.tellg();if(length<std::streamoff(kHeaderBytes)||length>std::streamoff(kHeaderBytes+kMaximumSpirvBytes))return false;
    std::vector<uint8_t> data(static_cast<size_t>(length));input.seekg(0);if(!input.read(reinterpret_cast<char*>(data.data()),length))return false;
    if(!std::equal(kMagic.begin(),kMagic.end(),data.begin())||Le32(data,8)!=shader.specialization_mask||
       size_t(Le32(data,12))*4!=data.size()-kHeaderBytes||
       !std::equal(shader.source_sha256.begin(),shader.source_sha256.end(),data.begin()+16))return false;
    std::vector<uint8_t> checked(data.begin()+8,data.begin()+80);checked.insert(checked.end(),data.begin()+kHeaderBytes,data.end());
    const auto digest=Hash(checked);if(!std::equal(digest.begin(),digest.end(),data.begin()+80))return false;
    shader.spirv.resize(Le32(data,12));for(size_t i=0;i<shader.spirv.size();++i)shader.spirv[i]=Le32(data,kHeaderBytes+i*4);
    if(!FragmentSpirv(shader.spirv)){shader.spirv.clear();return false;}shader.disk_cache_hit=true;return true;
}
void Store(const std::filesystem::path& path,const CompiledNativeShader& shader) {
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error)return;
    std::vector<uint8_t> checked;Put32(checked,shader.specialization_mask);Put32(checked,uint32_t(shader.spirv.size()));
    checked.insert(checked.end(),shader.source_sha256.begin(),shader.source_sha256.end());for(auto word:shader.spirv)Put32(checked,word);
    const auto digest=Hash(checked);std::vector<uint8_t> data(kMagic.begin(),kMagic.end());
    data.insert(data.end(),checked.begin(),checked.begin()+72);data.insert(data.end(),digest.begin(),digest.end());data.insert(data.end(),checked.begin()+72,checked.end());
    static std::atomic<uint64_t> sequence{0};static const uint64_t nonce=[] {std::random_device random;return uint64_t(random())<<32|random();}();
    auto temporary=path;temporary+=".tmp."+std::to_string(nonce)+"."+std::to_string(sequence++);
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);if(!output)return;
    output.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));output.close();
    if(output) {
        std::filesystem::rename(temporary,path,error);
        if(error&&std::filesystem::exists(path)) {
            CompiledNativeShader existing;existing.source_sha256=shader.source_sha256;existing.specialization_mask=shader.specialization_mask;
            if(!Load(path,existing)){std::filesystem::remove(path,error);if(!error)std::filesystem::rename(temporary,path,error);}
        }
    }
    std::filesystem::remove(temporary,error);
}
std::string ModifiedCommon() {
    Require(NativeShaderCompiler::CacheVersion()==kNativeFsiExactStoresBaseline,"Exact-store canonical compiler identity mismatch");
    auto modified=TryNativeFsiExactStoresSource(kNativeShaderCommon);Require(modified.has_value(),"Exact-store canonical common shape refused");
    const auto restored=RestoreNativeFsiExactStoresSource(*modified);Require(restored&&*restored==kNativeShaderCommon,"Exact-store common restoration failed");
    return std::move(*modified);
}
constexpr std::string_view kDepthBody=R"HLSL(
void main(float4 p : SV_Position, bool face : SV_IsFrontFace) {
  float2 slope = float2(ddx_coarse(p.z), ddy_coarse(p.z));
  const float4 zero = float4(0, 0, 0, 0);
  gow3NativeRasterOutput(p.xy, p.z, face, zero, zero, zero, zero, 0u, false, slope);
}
)HLSL";
} // namespace
struct NativeFsiExactStoresCompiler::Impl {
    NativeShaderCompiler& canonical;
    std::filesystem::path cache;
    const std::string modified_common;
    std::mutex mutex;
    std::unordered_map<std::string,std::shared_future<std::shared_ptr<const CompiledNativeShader>>> compiled;
    Impl(NativeShaderCompiler& original,std::filesystem::path path):canonical(original),modified_common(ModifiedCommon()) {
        if(!path.empty())cache=std::move(path)/NativeFsiExactStoresCompiler::CacheVersion();
    }
    std::shared_ptr<const CompiledNativeShader> Get(std::shared_ptr<const CompiledNativeShader> original,
        const std::function<std::string()>& generate) {
        Require(original&&original->stage==NativeShaderStage::Pixel&&original->mode==NativeShaderMode::FragmentInterlock&&
            original->source_sha256.size()==64&&FragmentSpirv(original->spirv),"Exact-store input is not canonical FSI fragment");
        const auto key=original->source_sha256;std::shared_future<std::shared_ptr<const CompiledNativeShader>> future;
        std::shared_ptr<std::promise<std::shared_ptr<const CompiledNativeShader>>> producer;
        {std::lock_guard lock(mutex);auto found=compiled.find(key);if(found!=compiled.end())future=found->second;
         else{producer=std::make_shared<std::promise<std::shared_ptr<const CompiledNativeShader>>>();future=producer->get_future().share();compiled.emplace(key,future);}}
        if(producer)try {
            auto shader=std::make_shared<CompiledNativeShader>();shader->stage=NativeShaderStage::Pixel;shader->mode=NativeShaderMode::FragmentInterlock;
            shader->source_sha256=original->source_sha256;shader->specialization_mask=original->specialization_mask;
            const auto path=cache/(key+".g3exactfsi");
            if(cache.empty()||!Load(path,*shader)) {
                shader->spirv=Compile(generate());if(!cache.empty())try{Store(path,*shader);}catch(const std::exception&){}
            }
            producer->set_value(std::move(shader));
        }catch(...){producer->set_exception(std::current_exception());}
        return future.get();
    }
};
NativeFsiExactStoresCompiler::NativeFsiExactStoresCompiler(NativeShaderCompiler& canonical,std::filesystem::path cache)
    :impl_(std::make_unique<Impl>(canonical,std::move(cache))){}
NativeFsiExactStoresCompiler::~NativeFsiExactStoresCompiler()=default;
std::string NativeFsiExactStoresCompiler::CacheVersion(){return kNativeFsiExactStoresShaderVersion;}
std::shared_ptr<const CompiledNativeShader> NativeFsiExactStoresCompiler::Get(std::span<const uint8_t> source) {
    // Canonical validation executes even for a derived cache hit.
    auto original=impl_->canonical.Get(source,NativeShaderMode::FragmentInterlock);
    return impl_->Get(original,[&,mask=original->specialization_mask] {
        std::vector<uint32_t> aligned((source.size()+3)/4);std::memcpy(aligned.data(),source.data(),source.size());
        ShaderRecompiler canonical,candidate;canonical.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),kNativeShaderCommon);
        candidate.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),impl_->modified_common.c_str());
        const auto restored=RestoreNativeFsiExactStoresSource(candidate.out);
        Require(restored&&*restored==canonical.out&&candidate.specConstantsMask==canonical.specConstantsMask&&canonical.specConstantsMask==mask,
            "Exact-store material/alpha/scale/ABI source restoration failed");
        return std::move(candidate.out);
    });
}
std::shared_ptr<const CompiledNativeShader> NativeFsiExactStoresCompiler::GetDepthOnly() {
    auto original=impl_->canonical.GetDepthOnlyFragmentInterlock();
    return impl_->Get(std::move(original),[&] {
        const auto canonical=std::string(kNativeShaderCommon)+std::string(kDepthBody);
        auto candidate=impl_->modified_common+std::string(kDepthBody);
        const auto restored=RestoreNativeFsiExactStoresSource(candidate);
        Require(restored&&*restored==canonical,"Exact-store null fragment restoration failed");
        return candidate;
    });
}
} // namespace gow3::renderer
