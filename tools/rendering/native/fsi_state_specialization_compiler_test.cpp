#include "renderer/native/fsi_state_specialization_compiler.h"
#include "renderer/native/content_hash.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
using namespace gow3::renderer;
namespace fs=std::filesystem;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> Read(const fs::path& path){std::ifstream file(path,std::ios::binary|std::ios::ate);auto size=file.tellg();if(!file||size<0||size>16u*1024*1024)throw std::runtime_error("Invalid bounded input");std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);if(!file.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("Truncated input");return bytes;}
void Check(const FsiStateSpecializationProgram& program,const CompiledNativeShader& canonical) {
    Require(program.source_sha256==canonical.source_sha256&&program.compiler_version==NativeShaderCompiler::CacheVersion()&&
        program.variant_version==FsiStateSpecializationCompilerVersion()&&program.compiler_version!=program.variant_version&&
        program.specialization_mask==canonical.specialization_mask,"Canonical/derived compiler namespace or guest ABI collision");
    Require(program.baseline_hlsl_sha256.size()==64&&program.modified_hlsl_sha256.size()==64&&
        program.baseline_hlsl_sha256!=program.modified_hlsl_sha256&&program.modified_common_sha256.size()==64,"Generated common/body proof was not retained");
    Require(program.spirv.size()>=5&&program.spirv[0]==0x07230203&&program.compiled_spirv_sha256==
        ContentHash({reinterpret_cast<const uint8_t*>(program.spirv.data()),program.spirv.size()*4}),"Derived SPIR-V digest differs");
    std::map<uint32_t,uint32_t> ids;std::map<uint32_t,uint32_t> defaults;bool interlock=false;
    for(size_t at=5;at<program.spirv.size();) {
        const auto count=program.spirv[at]>>16,op=program.spirv[at]&65535;
        Require(count&&count<=program.spirv.size()-at,"Invalid SPIR-V instruction extent");
        if(op==17&&count==2&&program.spirv[at+1]==5378)interlock=true;
        if(op==71&&count==4&&program.spirv[at+2]==1) {
            auto id=program.spirv[at+3];Require(id==0||(id>=32&&id<=47),"Unapproved specialization ID introduced");
            Require(ids.emplace(id,program.spirv[at+1]).second,"Duplicate specialization ID");
        }
        if(op==50&&count==4)defaults.emplace(program.spirv[at+2],program.spirv[at+3]); // OpSpecConstant uint
        at+=count;
    }
    Require(interlock&&ids.contains(32)&&defaults.contains(ids.at(32))&&defaults.at(ids.at(32))==0,
        "Default-disabled module lost interlock or generic enable=0 semantics");
}
}
int main(int argc,char** argv)try {
    if(argc!=3)throw std::runtime_error("Usage: gow3-native-fsi-state-specialization-compiler-test manifest.json NEW-cache-directory");
    const fs::path manifest=argv[1],directory=argv[2];Require(!fs::exists(directory),"Fixture cache must be new");
    fs::create_directories(directory);NativeShaderCompiler compiler(directory/"canonical");
    const auto canonical_version=NativeShaderCompiler::CacheVersion();
    std::ifstream input(manifest);const auto sources=nlohmann::json::parse(input);
    std::vector<std::vector<uint8_t>> pixels;std::shared_ptr<const FsiStateSpecializationProgram> builtin;
    size_t count=0;
    const auto exercise=[&](const std::shared_ptr<const CompiledNativeShader>& canonical,const std::vector<uint8_t>* bytes) {
        auto candidate=bytes?TryPrepareFsiStateSpecialization(compiler,*bytes,directory/"derived"):
            TryPrepareDepthOnlyFsiStateSpecialization(compiler,directory/"derived");
        Require(bool(candidate)&&!candidate->disk_cache_hit,"Cold compiler proof failed");Check(*candidate,*canonical);
        const auto output=directory/(candidate->source_sha256+".specialized.fsi.spv");
        std::ofstream file(output,std::ios::binary);file.write(reinterpret_cast<const char*>(candidate->spirv.data()),candidate->spirv.size()*4);
        Require(bool(file),"Cannot retain later ISA artifact");
        auto warm=bytes?TryPrepareFsiStateSpecialization(compiler,*bytes,directory/"derived"):
            TryPrepareDepthOnlyFsiStateSpecialization(compiler,directory/"derived");
        Require(warm&&warm->disk_cache_hit&&warm->spirv==candidate->spirv&&warm->baseline_hlsl_sha256==candidate->baseline_hlsl_sha256&&
            warm->modified_hlsl_sha256==candidate->modified_hlsl_sha256,"Derived warm cache lost compile proof/identity");
        const auto cache=directory/"derived"/candidate->variant_version/(candidate->source_sha256+".g3fsi");
        {std::fstream corrupt(cache,std::ios::binary|std::ios::in|std::ios::out);corrupt.seekp(336);corrupt.put('!');Require(bool(corrupt),"Cannot mutate isolated corruption fixture");}
        auto repaired=bytes?TryPrepareFsiStateSpecialization(compiler,*bytes,directory/"derived"):
            TryPrepareDepthOnlyFsiStateSpecialization(compiler,directory/"derived");
        Require(repaired&&!repaired->disk_cache_hit&&repaired->spirv==candidate->spirv,"Corrupt derived artifact was trusted or changed output");
        Require(NativeShaderCompiler::CacheVersion()==canonical_version,"Optional compiler mutated canonical cache identity");
        ++count;return candidate;
    };
    for(const auto& source:sources.at("shaders"))if(source.at("stage")=="pixel") {
        pixels.push_back(Read(manifest.parent_path()/source.at("path").get<std::string>()));
        auto canonical=compiler.Get(pixels.back(),NativeShaderMode::FragmentInterlock);
        Require(canonical->source_sha256==source.at("sha256").get<std::string>(),"Manifest source was not authenticated");
        exercise(canonical,&pixels.back());
    }
    Require(!pixels.empty(),"Manifest must exercise actual guest pixel shaders");
    builtin=exercise(compiler.GetDepthOnlyFragmentInterlock(),nullptr);Require(builtin->depth_only,"Builtin identity lost");
    // The compiler path has no pinned source allowlist. The caller's full PS
    // manifest is exercised; source restoration and exact generated body are
    // required inside TryPrepare before compiling each cold artifact.
    std::cout<<nlohmann::json({{"actual_guest_sources",pixels.size()},{"cold_warm_corruption_proofs",count},
        {"canonical_version",canonical_version},{"variant_version",FsiStateSpecializationCompilerVersion()},
        {"gpu_executed",false},{"artifacts",directory.string()}}).dump(2)<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
