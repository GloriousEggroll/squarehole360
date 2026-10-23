#include "renderer/native/fsi_state_forward_cfg_compiler.h"
#include "renderer/native/fsi_state_forward_cfg.h"
#include "renderer/native/content_hash.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <map>
using namespace gow3::renderer;
namespace fs=std::filesystem;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> Read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);const auto size=file.tellg();
    Require(bool(file)&&size>=0&&size<=16u*1024*1024,"Invalid bounded input");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);
    Require(bool(file.read(reinterpret_cast<char*>(bytes.data()),size)),"Truncated input");return bytes;
}
void Write(const fs::path& path,std::span<const uint32_t> words) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(words.data()),words.size_bytes());
    Require(bool(file),"Cannot retain actual compiled ISA input");
}
void Check(const FsiStateSpecializationProgram& program,const CompiledNativeShader& canonical) {
    Require(program.source_sha256==kFsiStateForwardCfgPixel&&program.source_sha256==canonical.source_sha256&&
        program.compiler_version==NativeShaderCompiler::CacheVersion()&&
        program.variant_version==FsiStateForwardCfgCompilerVersion()&&
        program.variant_version!=FsiStateSpecializationCompilerVersion()&&program.variant_version!=program.compiler_version&&
        program.specialization_mask==canonical.specialization_mask&&!program.depth_only,"Derived namespace/source/guest ABI collision");
    Require(program.baseline_hlsl_sha256.size()==64&&program.modified_hlsl_sha256.size()==64&&
        program.modified_common_sha256.size()==64&&program.baseline_hlsl_sha256!=program.modified_hlsl_sha256,
        "Actual cold source restoration/closed wrapper proof was not retained");
    Require(program.compiled_spirv_sha256==ContentHash({reinterpret_cast<const uint8_t*>(program.spirv.data()),
        program.spirv.size()*4}),"Compiled payload digest differs");
    std::map<uint32_t,uint32_t> ids,defaults;bool interlock=false;
    for(size_t at=5;at<program.spirv.size();) {
        const auto count=program.spirv[at]>>16,op=program.spirv[at]&65535;
        Require(count&&count<=program.spirv.size()-at,"Invalid SPIR-V extent");
        if(op==17&&count==2&&program.spirv[at+1]==5378)interlock=true;
        if(op==71&&count==4&&program.spirv[at+2]==1) {
            const auto id=program.spirv[at+3];Require(id==0||(id>=32&&id<=47),"New bool/float/other specialization ID introduced");
            Require(ids.emplace(id,program.spirv[at+1]).second,"Duplicate specialization ID");
        }
        if(op==50&&count==4)defaults.emplace(program.spirv[at+2],program.spirv[at+3]);
        at+=count;
    }
    Require(interlock&&ids.contains(32)&&defaults.contains(ids.at(32))&&defaults.at(ids.at(32))==0,
        "Derived module changed interlock or default-disabled FSI state");
}
}
int main(int argc,char** argv)try {
    Require(argc==3,"Usage: gow3-native-fsi-state-forward-cfg-compiler-test manifest.json NEW-cache-directory");
    const fs::path manifest=argv[1],directory=argv[2];Require(!fs::exists(directory),"Fixture cache must be new");
    fs::create_directories(directory);std::ifstream input(manifest);const auto sources=nlohmann::json::parse(input);
    std::vector<uint8_t> pixel,vertex,other;
    for(const auto& source:sources.at("shaders")) {
        const auto digest=source.at("sha256").get<std::string>();
        if(digest==kFsiStateForwardCfgPixel)pixel=Read(manifest.parent_path()/source.at("path").get<std::string>());
        else if(digest==kFsiStateForwardCfgVertex)vertex=Read(manifest.parent_path()/source.at("path").get<std::string>());
        else if(other.empty()&&source.at("stage")=="pixel")other=Read(manifest.parent_path()/source.at("path").get<std::string>());
    }
    Require(!pixel.empty()&&!vertex.empty()&&!other.empty(),"Manifest must contain actual pinned pair and refused companion PS");
    NativeShaderCompiler compiler(directory/"canonical");const auto version=NativeShaderCompiler::CacheVersion();
    const auto canonical=compiler.Get(pixel,NativeShaderMode::FragmentInterlock);
    const auto canonical_vs=compiler.Get(vertex,NativeShaderMode::Raster);
    Require(canonical->source_sha256==kFsiStateForwardCfgPixel&&canonical_vs->source_sha256==kFsiStateForwardCfgVertex,
        "Pinned source bytes differ from manifest");
    const auto canonical_path=directory/"canonical"/version/(canonical->source_sha256+".fsi.g3spv");
    const auto vs_path=directory/"canonical"/version/(canonical_vs->source_sha256+".g3spv");
    const auto original_cache=Read(canonical_path),original_vs_cache=Read(vs_path);
    auto candidate=TryPrepareFsiStateForwardCfg(compiler,pixel,directory/"combined");
    Require(candidate&&!candidate->disk_cache_hit,"Actual cold composition refused");Check(*candidate,*canonical);
    auto pure=TryPrepareFsiStateSpecialization(compiler,pixel,directory/"pure-state");
    Require(pure&&candidate->spirv!=pure->spirv&&candidate->spirv!=canonical->spirv,
        "Combined output reused pure-state or canonical bytes");
    Write(directory/"original-ps.spv",canonical->spirv);Write(directory/"original-vs.spv",canonical_vs->spirv);
    Write(directory/"pure-state-ps.spv",pure->spirv);Write(directory/"combined-state-forward-ps.spv",candidate->spirv);
    auto warm=TryPrepareFsiStateForwardCfg(compiler,pixel,directory/"combined");
    Require(warm&&warm->disk_cache_hit&&warm->spirv==candidate->spirv&&
        warm->baseline_hlsl_sha256==candidate->baseline_hlsl_sha256&&warm->modified_hlsl_sha256==candidate->modified_hlsl_sha256,
        "Warm derived cache lost actual compile proof");Check(*warm,*canonical);
    const auto cache=directory/"combined"/candidate->variant_version/(candidate->source_sha256+".g3fsifwd");
    {std::fstream corrupt(cache,std::ios::binary|std::ios::in|std::ios::out);corrupt.seekp(336);corrupt.put('!');Require(bool(corrupt),"Cannot mutate isolated corruption fixture");}
    auto repaired=TryPrepareFsiStateForwardCfg(compiler,pixel,directory/"combined");
    Require(repaired&&!repaired->disk_cache_hit&&repaired->spirv==candidate->spirv,"Corrupt combined artifact trusted");
    Require(!TryPrepareFsiStateForwardCfg(compiler,other,directory/"combined")&&
        !TryPrepareFsiStateForwardCfg(compiler,vertex,directory/"combined"),"Unpinned source/stage admitted");
    auto malformed=pixel;malformed.back()^=1;
    Require(!TryPrepareFsiStateForwardCfg(compiler,malformed,directory/"combined"),"Mutated source identity admitted");
    Require(NativeShaderCompiler::CacheVersion()==version&&Read(canonical_path)==original_cache&&Read(vs_path)==original_vs_cache&&
        compiler.Get(pixel,NativeShaderMode::FragmentInterlock)->spirv==canonical->spirv&&
        compiler.Get(vertex,NativeShaderMode::Raster)->spirv==canonical_vs->spirv,"Optional compiler changed canonical namespace/bytes/ABI");
    std::cout<<nlohmann::json({{"canonical_version",version},{"variant_version",candidate->variant_version},
        {"pixel_source",candidate->source_sha256},{"spec_mask",candidate->specialization_mask},
        {"cold_warm_corrupt_restoration_proof",true},{"canonical_cache_bytes_exact",true},
        {"gpu_executed",false},{"artifacts",directory.string()}}).dump(2)<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
