#include "renderer/native/opaque_color_overwrite.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace gow3::renderer;
namespace {
void Check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    Check(bool(stream),"Cannot read source fixture");
    return std::string(std::istreambuf_iterator<char>(stream),{});
}
void Write(const std::filesystem::path& path,const std::string& source) {
    std::ofstream stream(path,std::ios::binary);
    Check(bool(stream),"Cannot open paired HLSL output");
    stream.write(source.data(),source.size());Check(bool(stream),"Cannot write paired HLSL output");
}
}
int main(int argc,char** argv) try {
    Check(argc==5,"Usage: source-test canonical-helper.hlsl fixture.hlsl baseline.hlsl candidate.hlsl");
    const auto helper=Read(argv[1]);
    const auto candidate=TryOpaqueColorOverwriteSource(helper);
    Check(candidate.has_value(),"Canonical FSI helper has an unsupported source shape");
    Check(!TryOpaqueColorOverwriteSource("").has_value(),"Empty source was accepted");
    Check(!TryOpaqueColorOverwriteSource(*candidate).has_value(),"Candidate was transformed again");
    Check(!TryOpaqueColorOverwriteSource(helper+helper).has_value(),"Ambiguous duplicate helper was accepted");
    auto mutated=helper;
    const auto offset=mutated.find(kOpaqueColorOverwriteOriginal);
    mutated[offset+5]='X';
    Check(!TryOpaqueColorOverwriteSource(mutated).has_value(),"Unknown helper shape was accepted");
    const auto begin=helper.find(kOpaqueColorOverwriteOriginal);
    Check(helper.substr(0,begin)==candidate->substr(0,begin),"Prefix outside color epilogue changed");
    Check(helper.substr(begin+kOpaqueColorOverwriteOriginal.size())==
        candidate->substr(begin+kOpaqueColorOverwriteReplacement.size()),"Suffix outside color epilogue changed");
    const auto fixture=Read(argv[2]);
    constexpr std::string_view marker="// GOW3_INSERT_FSI_HELPER";
    const auto position=fixture.find(marker);
    Check(position!=std::string::npos&&fixture.find(marker,position+1)==std::string::npos,
        "Missing or duplicate fixture include marker");
    auto baseline_source=fixture,candidate_source=fixture;
    baseline_source.replace(position,marker.size(),helper);
    candidate_source.replace(position,marker.size(),*candidate);
    Write(argv[3],baseline_source);Write(argv[4],candidate_source);
    std::cout<<"PASS: closed single-function source replacement and paired production-ABI fixture preparation\n";
} catch(const std::exception& exception) {std::cerr<<exception.what()<<'\n';return 1;}
