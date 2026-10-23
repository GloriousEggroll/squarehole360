#include "renderer/native/texture_probe.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using gow3::renderer::NativeTextureProbe;
namespace fs=std::filesystem;
using Json=nlohmann::json;
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class F>void Reject(F&& action) {
    bool rejected=false;try {action();}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"Invalid configuration was accepted");
}
std::vector<uint8_t> Read(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);
    Require(bool(f),"Missing probe output");
    return {std::istreambuf_iterator<char>(f),{}};
}
int main() try {
    const std::string hash(64,'a'),other(64,'b');
    Require(NativeTextureProbe::Parse({},{}).empty(),"Default probe must be off");
    Require(NativeTextureProbe::Parse("probe",hash).size()==1,"Valid hash rejected");
    Require(NativeTextureProbe::Parse("probe",hash+","+hash).size()==1,"Duplicate hash not deduplicated");
    Reject([&]{NativeTextureProbe::Parse({},hash);});
    Reject([&]{NativeTextureProbe::Parse("probe",{});});
    for(const auto& malformed:{hash+",",","+hash,hash+",,"+other,std::string(64,'A'),std::string(63,'a'),"../"+hash})
        Reject([&]{NativeTextureProbe::Parse("probe",malformed);});
    std::string maximum=hash;
    for(size_t i=1;i<NativeTextureProbe::kMaxHashes;++i)maximum+=","+hash;
    Require(NativeTextureProbe::Parse("probe",maximum).size()==1,"32 hashes rejected");
    Reject([&]{NativeTextureProbe::Parse("probe",maximum+","+hash);});
    const auto root=fs::temp_directory_path()/("gow3-texture-probe-test-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(fs::create_directory(root),"Test directory collision");
    struct Cleanup {fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);}} cleanup{root};
    std::array<uint32_t,6> fetch{1,2,3,4,5,6};
    std::vector<uint8_t> base{0,1,2,255},mips{19,18,17};
    NativeTextureProbe probe(root/"valid",hash);
    Require(probe.Selected(hash)&&!probe.Selected(other),"Selection mismatch");
    probe.Record(fetch,7,8,9,10,hash,other,3,base,mips);
    Require(Read(root/"valid/texture-0-base.bin")==base,"Base bytes changed");
    Require(Read(root/"valid/texture-0-mips.bin")==mips,"Mip bytes changed");
    std::ifstream metadata_file(root/"valid/texture-0.json");Json metadata;metadata_file>>metadata;
    Require(metadata.at("fetch")==fetch&&metadata.at("memory_version")==7&&metadata.at("frame")==8&&
        metadata.at("event")==9&&metadata.at("slot")==10&&metadata.at("overlaid_bytes")==3,"Metadata mismatch");
    Require(metadata.at("source_base_payload")==hash&&metadata.at("source_mip_payload")==other&&
        metadata.at("base").at("sha256")==picosha2::hash256_hex_string(base)&&
        metadata.at("mips").at("sha256")==picosha2::hash256_hex_string(mips),"Payload hashes mismatch");
    probe.Record(fetch,70,80,90,10,hash,hash,0,base,mips);
    Require(probe.Status().at("records")==1,"Unchanged bytes duplicated after version/source churn");
    fetch[0]=99;
    probe.Record(fetch,71,81,91,10,hash,{},0,base,{});
    Require(probe.Status().at("records")==2&&Read(root/"valid/texture-1-mips.bin").empty(),"Empty mip record failed");
    const auto sentinel=std::vector<uint8_t>{44,55};
    {std::ofstream f(root/"valid/texture-2-base.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(sentinel.data()),sentinel.size());}
    base.push_back(1);probe.Record(fetch,72,82,92,10,hash,{},0,base,{});
    Require(!probe.Selected(hash)&&Read(root/"valid/texture-2-base.bin")==sentinel,
        "Collision overwrote a file or failed to disable probe");
    Require(!fs::exists(root/"valid/texture-2.json"),"Incomplete record was committed");
    NativeTextureProbe existing(root/"valid",hash);
    Require(!existing.Selected(hash),"Existing output directory accepted");
    NativeTextureProbe quota(root/"quota",hash);
    for(size_t i=0;i<=NativeTextureProbe::kMaxRecords;++i) {
        fetch[0]=uint32_t(i);quota.Record(fetch,i,8,9,10,hash,{},0,base,{});
    }
    Require(quota.Status().at("records")==NativeTextureProbe::kMaxRecords&&!quota.Selected(hash),"Record limit failed");
    NativeTextureProbe size_quota(root/"size-quota",hash);
    const std::vector<uint8_t> excessive(NativeTextureProbe::kMaxBytes+1,0);
    size_quota.Record(fetch,7,8,9,10,hash,{},0,excessive,{});
    Require(size_quota.Status().at("records")==0&&!size_quota.Selected(hash)&&fs::is_empty(root/"size-quota"),"Byte limit failed");
    std::cout<<"Texture probe parsing, exact bytes/metadata, deduplication, exclusive output, I/O disable, and quotas passed\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
