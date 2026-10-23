#include "renderer/native/fsi_exact_stores.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
std::string Read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot read exact-store source input");return {std::istreambuf_iterator<char>(file),{}};}
void Write(const std::filesystem::path& path,const std::string& data){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(data.data(),data.size());if(!file)throw std::runtime_error("Cannot write exact-store paired source");}
}
int main(int argc,char** argv) try {
    if(argc!=5)throw std::runtime_error("Usage: source-test canonical-helper.hlsl fixture.hlsl baseline.hlsl candidate.hlsl");
    size_t checks=0;const auto check=[&](bool condition){++checks;if(!condition)throw std::runtime_error("Exact-store source restoration/gate mismatch");};
    const auto original=Read(argv[1]);const auto candidate=TryNativeFsiExactStoresSource(original);check(candidate.has_value());
    const auto restored=RestoreNativeFsiExactStoresSource(*candidate);check(restored&&*restored==original);
    check(!TryNativeFsiExactStoresSource("")&&!TryNativeFsiExactStoresSource(*candidate)&&!TryNativeFsiExactStoresSource(original+original));
    for(const auto block:{kNativeFsiExactStoresDepthOriginal,kNativeFsiExactStoresFlagOriginal,kNativeFsiExactStoresColorOriginal}) {
        auto mutated=original;const auto at=mutated.find(block);check(at!=std::string::npos);mutated[at+3]='X';check(!TryNativeFsiExactStoresSource(mutated));
    }
    for(const auto block:{kNativeFsiExactStoresDepthReplacement,kNativeFsiExactStoresFlagReplacement,kNativeFsiExactStoresColorReplacement}) {
        auto mutated=*candidate;const auto at=mutated.find(block);check(at!=std::string::npos);mutated[at+3]='X';check(!RestoreNativeFsiExactStoresSource(mutated));
    }
    const auto fixture=Read(argv[2]);constexpr std::string_view marker="// GOW3_INSERT_FSI_HELPER";const auto at=fixture.find(marker);
    check(at!=std::string::npos&&fixture.find(marker,at+1)==std::string::npos);
    auto baseline=fixture,modified=fixture;baseline.replace(at,marker.size(),original);modified.replace(at,marker.size(),*candidate);
    Write(argv[3],baseline);Write(argv[4],modified);std::cout<<"PASS: "<<checks<<" closed source adapter/restoration checks, full production ABI paired sources\n";
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
