#include "native_early_depth_fixture_pair.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <bit>
using namespace gow3::renderer;
int main(int argc,char** argv)try {
    static_assert(std::endian::native==std::endian::little);
    if(argc!=4)throw std::invalid_argument("Usage: fixture-editor baseline.spv authenticated_baseline_sha256 new_candidate.spv");
    const std::filesystem::path input=argv[1],output=argv[3];
    if(std::filesystem::exists(output))throw std::invalid_argument("Fixture candidate output already exists");
    std::ifstream stream(input,std::ios::binary|std::ios::ate);
    if(!stream||stream.tellg()<20||stream.tellg()>1048576||stream.tellg()%4)
        throw std::invalid_argument("Invalid bounded fixture baseline");
    std::vector<uint32_t> baseline(size_t(stream.tellg())/4);stream.seekg(0);
    if(!stream.read(reinterpret_cast<char*>(baseline.data()),baseline.size()*4))throw std::invalid_argument("Truncated fixture baseline");
    const auto candidate=fixture::MakeNativeEarlyDepthFixtureCandidate(baseline,argv[2]);
    if(!fixture::AuditNativeEarlyDepthFixturePair(baseline,candidate,argv[2]))throw std::runtime_error("Fixture candidate exact recovery failed");
    std::ofstream destination(output,std::ios::binary);
    if(!destination.write(reinterpret_cast<const char*>(candidate.data()),candidate.size()*4))throw std::runtime_error("Fixture candidate write failed");
    std::cout<<"Controlled fixture three-word editor PASS; authenticated original recovered; no production shader admission\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
