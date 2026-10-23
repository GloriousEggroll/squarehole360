#include "fsi_state_forward_cfg_compiler.h"
#include "native_shader_embedded.h"
#include "native_fsi_state_forward_cfg_shader_version.h"
#include "shader_recompiler.h"
#include "fsi_state_forward_cfg.h"
#include <dxcapi.h>
#include <picosha2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
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
    bool entry=false,interlock=false,enable=false;
    for(size_t at=5;at<code.size();) {
        const uint32_t count=code[at]>>16,opcode=code[at]&65535;
        if(!count||count>code.size()-at)return false;
        if(opcode==15) {if(count<4||code[at+1]!=4)return false;entry=true;}
        if(opcode==17&&count==2&&code[at+1]==5378)interlock=true;
        if(opcode==71&&count==4&&code[at+2]==1) {
            const auto spec=code[at+3];
            if(spec!=0&&(spec<32||spec>47))return false;
            enable|=spec==32;
        }
        at+=count;
    }
    return entry&&interlock&&enable;
}
std::vector<uint32_t> Compile(const std::string& source) {
    IDxcCompiler3* raw_compiler=nullptr;
    const auto created=DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&raw_compiler));
    Com<IDxcCompiler3> compiler(raw_compiler);
    Require(SUCCEEDED(created)&&compiler,"Cannot initialize FSI-state-forward-CFG DXC");
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
    Require(SUCCEEDED(compiled)&&result,"FSI-state-forward-CFG DXC invocation failed");
    HRESULT status=E_FAIL;
    Require(SUCCEEDED(result->GetStatus(&status)),"FSI-state-forward-CFG DXC status unavailable");
    if(FAILED(status)) {
        IDxcBlobUtf8* raw_errors=nullptr;
        result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&raw_errors),nullptr);
        Com<IDxcBlobUtf8> errors(raw_errors);
        throw std::runtime_error(std::string("FSI-state-forward-CFG SPIR-V compile failed: ")+
            (errors?std::string(errors->GetStringPointer(),errors->GetStringLength()):"No diagnostics"));
    }
    IDxcBlob* raw_object=nullptr;
    const auto obtained=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&raw_object),nullptr);
    Com<IDxcBlob> object(raw_object);
    Require(SUCCEEDED(obtained)&&object&&object->GetBufferSize()>=20&&
        object->GetBufferSize()<=kMaxSpirvBytes&&object->GetBufferSize()%4==0,
        "FSI-state-forward-CFG DXC object is invalid");
    std::vector<uint32_t> words(object->GetBufferSize()/4);
    std::memcpy(words.data(),object->GetBufferPointer(),object->GetBufferSize());
    Require(FragmentSpirv(words),"FSI-state-forward-CFG fragment SPIR-V is malformed");
    return words;
}

constexpr size_t kHeaderBytes=400;
constexpr std::array<uint8_t,8> kMagic{'G','3','F','F','W','D','0','1'};
void Put32(std::vector<uint8_t>& bytes,uint32_t value) {
    for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(uint8_t(value>>shift));
}
uint32_t Le32(std::span<const uint8_t> bytes,size_t at) {
    return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;
}
std::string Field(std::span<const uint8_t> bytes,size_t at) {
    return {reinterpret_cast<const char*>(bytes.data()+at),64};
}
bool Digest(std::string_view value) {
    return value.size()==64&&std::ranges::all_of(value,[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
bool Load(const std::filesystem::path& path,FsiStateSpecializationProgram& program) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)return false;
    const auto size=input.tellg();
    if(size<std::streamoff(kHeaderBytes+20)||size>std::streamoff(kHeaderBytes+kMaxSpirvBytes))return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(bytes.data()),size))return false;
    if(!std::ranges::equal(kMagic,std::span(bytes).first(8))||
       size_t(Le32(bytes,12))*4!=bytes.size()-kHeaderBytes||
       Le32(bytes,8)!=program.specialization_mask||Field(bytes,16)!=program.source_sha256||
       Field(bytes,80)!=program.variant_version||Field(bytes,272)!=program.modified_common_sha256)return false;
    std::vector<uint8_t> checked(bytes.begin(),bytes.begin()+336);
    checked.insert(checked.end(),bytes.begin()+kHeaderBytes,bytes.end());
    if(Hash(checked)!=Field(bytes,336)||!Digest(Field(bytes,144))||!Digest(Field(bytes,208)))return false;
    std::vector<uint32_t> words(Le32(bytes,12));
    for(size_t i=0;i<words.size();++i)words[i]=Le32(bytes,kHeaderBytes+i*4);
    if(!FragmentSpirv(words))return false;
    program.baseline_hlsl_sha256=Field(bytes,144);program.modified_hlsl_sha256=Field(bytes,208);
    program.spirv=std::move(words);
    program.compiled_spirv_sha256=Hash({reinterpret_cast<const uint8_t*>(program.spirv.data()),program.spirv.size()*4});
    program.disk_cache_hit=true;return true;
}
void Store(const std::filesystem::path& path,const FsiStateSpecializationProgram& program) noexcept {
    // Optional cache never changes rendering authority. A concurrent valid
    // writer/Windows destination may win; a failed replacement is a cache miss.
    std::filesystem::path temporary;
    try {
        std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);if(ec)return;
        std::vector<uint8_t> bytes(kMagic.begin(),kMagic.end());
        Put32(bytes,program.specialization_mask);Put32(bytes,uint32_t(program.spirv.size()));
        for(const auto* field:{&program.source_sha256,&program.variant_version,&program.baseline_hlsl_sha256,
                              &program.modified_hlsl_sha256,&program.modified_common_sha256}) {
            Require(Digest(*field),"Invalid FSI state cache identity");
            bytes.insert(bytes.end(),field->begin(),field->end());
        }
        std::vector<uint8_t> payload;
        for(uint32_t word:program.spirv)Put32(payload,word);
        auto checked=bytes;checked.insert(checked.end(),payload.begin(),payload.end());
        const auto digest=Hash(checked);bytes.insert(bytes.end(),digest.begin(),digest.end());
        bytes.insert(bytes.end(),payload.begin(),payload.end());
        static std::atomic<uint64_t> serial{0};
        static const uint64_t nonce=[](){std::random_device random;return uint64_t(random())<<32|random();}();
        temporary=path;temporary+=".tmp-"+std::to_string(nonce)+"-"+std::to_string(serial++);
        {std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);
         if(!stream.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()))||!stream.flush())
             throw std::runtime_error("FSI state cache write failed");}
        std::filesystem::rename(temporary,path,ec);
    }catch(...){/* Optional cache failure is not a shader failure. */}
    if(!temporary.empty()){std::error_code ignored;std::filesystem::remove(temporary,ignored);}
}
std::shared_ptr<FsiStateSpecializationProgram> Candidate(const CompiledNativeShader& canonical,
    std::string_view modified_common,bool depth_only) {
    auto program=std::make_shared<FsiStateSpecializationProgram>();
    program->source_sha256=canonical.source_sha256;program->compiler_version=NativeShaderCompiler::CacheVersion();
    program->variant_version=kNativeFsiStateForwardCfgShaderVersion;
    program->modified_common_sha256=Hash({reinterpret_cast<const uint8_t*>(modified_common.data()),modified_common.size()});
    program->specialization_mask=canonical.specialization_mask;program->depth_only=depth_only;return program;
}
void SetCompiled(FsiStateSpecializationProgram& program,std::string_view baseline,std::string_view candidate) {
    program.baseline_hlsl_sha256=Hash({reinterpret_cast<const uint8_t*>(baseline.data()),baseline.size()});
    program.modified_hlsl_sha256=Hash({reinterpret_cast<const uint8_t*>(candidate.data()),candidate.size()});
    program.spirv=Compile(std::string(candidate));
    program.compiled_spirv_sha256=Hash({reinterpret_cast<const uint8_t*>(program.spirv.data()),program.spirv.size()*4});
}
const std::optional<std::string>& ModifiedCommon() {
    static const auto common=TryFsiStateSpecializationSource(kNativeShaderCommon);return common;
}
std::filesystem::path CachePath(const std::filesystem::path& directory,const FsiStateSpecializationProgram& p) {
    return directory.empty()?std::filesystem::path{}:directory/p.variant_version/(p.source_sha256+".g3fsifwd");
}

}
std::string FsiStateForwardCfgCompilerVersion(){return kNativeFsiStateForwardCfgShaderVersion;}
std::shared_ptr<const FsiStateSpecializationProgram> TryPrepareFsiStateForwardCfg(
    NativeShaderCompiler& compiler,std::span<const uint8_t> container,const std::filesystem::path& directory) {
    if(container.size()<36||container.size()>16u*1024*1024||Hash(container)!=kFsiStateForwardCfgPixel||
       compiler.CacheVersion()!=kFsiStateForwardCfgBaselineCompiler)return nullptr;
    const auto canonical=compiler.Get(container,NativeShaderMode::FragmentInterlock);
    if(canonical->stage!=NativeShaderStage::Pixel||canonical->source_sha256!=kFsiStateForwardCfgPixel||
       canonical->mode!=NativeShaderMode::FragmentInterlock||!ModifiedCommon())return nullptr;
    const auto analyzed=AnalyzeForwardPixelCatalog(container);
    if(!analyzed)return nullptr;
    const auto& common=*ModifiedCommon();auto program=Candidate(*canonical,common,false);
    const auto path=CachePath(directory,*program);
    if(!path.empty()&&Load(path,*program))return program;
    std::vector<uint32_t> aligned((container.size()+3)/4);std::memcpy(aligned.data(),container.data(),container.size());
    ShaderRecompiler baseline,state;
    baseline.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),kNativeShaderCommon);
    state.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),common);
    Require(baseline.specConstantsMask==canonical->specialization_mask&&
        state.specConstantsMask==canonical->specialization_mask,"FSI forward composition changed guest specialization ABI");
    const auto composed=TryComposeFsiStateForwardCfgSource(baseline.out,state.out,kNativeShaderCommon,common,*analyzed);
    Require(bool(composed),"FSI forward composition failed common restoration or exact guest wrapper proof");
    SetCompiled(*program,baseline.out,*composed);
    Require(program->spirv!=canonical->spirv,"FSI forward composition equals canonical output");
    if(!path.empty())Store(path,*program);return program;
}
}
