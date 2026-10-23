#include "renderer/native/fsi_exact_stores_compiler.h"
#include "renderer/native/fsi_exact_stores.h"
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
namespace {
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    if(!stream||stream.tellg()<36||stream.tellg()>16*1024*1024)throw std::runtime_error("Invalid actual pixel source input");
    std::vector<uint8_t> data(static_cast<size_t>(stream.tellg()));stream.seekg(0);
    if(!stream.read(reinterpret_cast<char*>(data.data()),data.size()))throw std::runtime_error("Truncated actual pixel source");
    return data;
}
void Write(const std::filesystem::path& path,const std::vector<uint32_t>& words) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);stream.write(reinterpret_cast<const char*>(words.data()),words.size()*4);
    if(!stream)throw std::runtime_error("Cannot write exact-store compiled witness");
}
}
int main(int argc,char** argv) try {
    if(argc!=3&&argc!=5)throw std::runtime_error("Usage: compiler-test actual-pixel-container fresh-cache-dir [baseline.spv candidate.spv]");
    const auto source=Read(argv[1]);const std::filesystem::path cache=argv[2];
    if(std::filesystem::exists(cache))throw std::runtime_error("Use a new owned cache directory for the cold/warm test");
    size_t checks=0;const auto check=[&](bool condition){++checks;if(!condition)throw std::runtime_error("Exact-store compiler/cache/ABI mismatch");};
    NativeShaderCompiler canonical;const auto baseline=canonical.Get(source,NativeShaderMode::FragmentInterlock);
    check(NativeShaderCompiler::CacheVersion()==kNativeFsiExactStoresBaseline);
    NativeFsiExactStoresCompiler candidate(canonical,cache);const auto first=candidate.Get(source);
    check(first&&first->stage==NativeShaderStage::Pixel&&first->mode==NativeShaderMode::FragmentInterlock&&
        first->source_sha256==baseline->source_sha256&&first->specialization_mask==baseline->specialization_mask&&!first->disk_cache_hit);
    check(first->spirv!=baseline->spirv);
    check(candidate.Get(source)==first);
    std::vector<std::future<std::shared_ptr<const CompiledNativeShader>>> workers;
    for(unsigned i=0;i<8;++i)workers.push_back(std::async(std::launch::async,[&]{return candidate.Get(source);}));
    for(auto& worker:workers)check(worker.get()==first);
    const auto builtin=candidate.GetDepthOnly();check(builtin&&builtin->stage==NativeShaderStage::Pixel&&
        builtin->source_sha256==canonical.GetDepthOnlyFragmentInterlock()->source_sha256);
    NativeFsiExactStoresCompiler warm(canonical,cache);const auto second=warm.Get(source);
    check(second&&second->disk_cache_hit&&second->spirv==first->spirv&&second->source_sha256==baseline->source_sha256&&
        second->specialization_mask==baseline->specialization_mask);
    const auto warm_builtin=warm.GetDepthOnly();check(warm_builtin->disk_cache_hit&&warm_builtin->spirv==builtin->spirv);
    // A new provider must reject a corrupted owned derived payload and
    // regenerate, never admit its SPIR-V even when the source hash is right.
    const auto payload=cache/candidate.CacheVersion()/(first->source_sha256+".g3exactfsi");
    {std::fstream file(payload,std::ios::binary|std::ios::in|std::ios::out);check(bool(file));
     file.seekg(-1,std::ios::end);char byte=0;file.read(&byte,1);check(bool(file));
     byte^=char(1);file.seekp(-1,std::ios::end);file.write(&byte,1);check(bool(file));}
    NativeFsiExactStoresCompiler repair(canonical,cache);const auto repaired=repair.Get(source);
    check(repaired&&!repaired->disk_cache_hit&&repaired->spirv==first->spirv&&
        repaired->specialization_mask==baseline->specialization_mask);
    NativeFsiExactStoresCompiler repaired_warm(canonical,cache);const auto recovered=repaired_warm.Get(source);
    check(recovered->disk_cache_hit&&recovered->spirv==first->spirv);
    bool refused=false;try{candidate.Get({});}catch(const std::exception&){refused=true;}check(refused);
    const auto before=baseline->spirv;check(canonical.Get(source,NativeShaderMode::FragmentInterlock)->spirv==before);
    if(argc==5){Write(argv[3],baseline->spirv);Write(argv[4],first->spirv);}
    std::cout<<"PASS: "<<checks<<" actual canonical/derived material restoration and cold/warm/thread cache checks; canonical="
        <<NativeShaderCompiler::CacheVersion()<<" derived="<<candidate.CacheVersion()<<" source="<<baseline->source_sha256<<"\n";
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
