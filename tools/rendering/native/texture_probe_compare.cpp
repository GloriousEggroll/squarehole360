// CPU-only comparison of bounded, user-owned native texture probe records.
#include "renderer/native/texture_data.h"
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace fs=std::filesystem;
using Json=nlohmann::json;
using namespace gow3::renderer;
constexpr uint64_t kLimit=256ull*1024*1024;

std::string Digest(const Json& field) {
    const auto hash=field.get<std::string>();
    if(hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::runtime_error("Invalid probe SHA-256");
    return hash;
}
std::vector<uint8_t> Read(const fs::path& path,uint64_t expected) {
    if(expected>kLimit||fs::is_symlink(path)||!fs::is_regular_file(path)||fs::file_size(path)!=expected)
        throw std::runtime_error("Invalid probe file extent/type: "+path.string());
    std::ifstream file(path,std::ios::binary);
    std::vector<uint8_t> bytes(expected);
    if(expected)file.read(reinterpret_cast<char*>(bytes.data()),expected);
    if(!file)throw std::runtime_error("Cannot read probe file: "+path.string());
    return bytes;
}
struct Record {
    fs::path path;
    Json metadata,levels=Json::array();
    std::string group,signature;
    std::vector<uint8_t> base,mips;
    TextureData decoded;
};
std::vector<Record> Load(const fs::path& directory) {
    if(!fs::is_directory(directory))throw std::runtime_error("Invalid probe directory: "+directory.string());
    std::vector<fs::path> paths;
    for(const auto& entry:fs::directory_iterator(directory)) {
        const auto name=entry.path().filename().string();
        if(!name.starts_with("texture-")||!name.ends_with(".json"))continue;
        const auto number=name.substr(8,name.size()-13);
        if(number.empty()||number.find_first_not_of("0123456789")!=std::string::npos)
            throw std::runtime_error("Invalid probe metadata filename");
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(),paths.end(),[](const auto& a,const auto& b) {
        return std::stoull(a.stem().string().substr(8))<std::stoull(b.stem().string().substr(8));
    });
    if(paths.empty()||paths.size()>128)throw std::runtime_error("Probe directory requires 1..128 committed records");
    uint64_t total=0,decoded_total=0;
    std::vector<Record> records;
    for(const auto& path:paths) {
        Record record;record.path=path;
        const auto text=Read(path,fs::file_size(path));
        if(text.size()>1024*1024)throw std::runtime_error("Oversized probe metadata");
        record.metadata=Json::parse(text);
        const auto& metadata=record.metadata;
        if(metadata.at("version")!=1||metadata.at("provenance")!="native_cpu_texture_after_gpu_overlay_before_decode")
            throw std::runtime_error("Unrecognized texture probe provenance");
        const auto source_base=Digest(metadata.at("source_base_payload"));
        if(!metadata.at("source_mip_payload").is_null())Digest(metadata.at("source_mip_payload"));
        auto payload=[&](const char* key,const char* suffix) {
            const auto& field=metadata.at(key);
            const auto filename=field.at("file").get<std::string>();
            if(filename!=path.stem().string()+suffix)throw std::runtime_error("Unexpected probe payload filename");
            const auto size=field.at("bytes").get<uint64_t>();
            if(size>kLimit-total)throw std::runtime_error("Probe directory exceeds byte limit");
            total+=size;
            auto bytes=Read(directory/filename,size);
            if(picosha2::hash256_hex_string(bytes)!=Digest(field.at("sha256")))
                throw std::runtime_error("Probe payload SHA-256 mismatch: "+filename);
            return bytes;
        };
        record.base=payload("base","-base.bin");record.mips=payload("mips","-mips.bin");
        const auto fetch=metadata.at("fetch").get<std::array<uint32_t,6>>();
        auto normalized=fetch;normalized[1]&=0xfff;normalized[5]&=0xfff;
        record.group=Json::array({source_base,normalized}).dump();
        record.decoded=DecodeTexture(fetch,record.base,record.mips);
        Json signatures=Json::array();
        for(const auto& level:record.decoded.levels) {
            if(level.bytes.size()>kLimit*2-decoded_total)throw std::runtime_error("Decoded probes exceed memory limit");
            decoded_total+=level.bytes.size();
            const auto hash=picosha2::hash256_hex_string(level.bytes);
            const auto& layout=level.layout;
            record.levels.push_back({{"mip",layout.mip},{"layer",layout.layer},
                {"width",layout.width},{"height",layout.height},{"depth",layout.depth},
                {"bytes",level.bytes.size()},{"sha256",hash}});
            signatures.push_back(Json::array({layout.mip,layout.layer,layout.width,layout.height,layout.depth,hash}));
        }
        record.signature=signatures.dump();
        records.push_back(std::move(record));
    }
    return records;
}
struct Difference {uint64_t bytes=0,blocks=0;Json first=nullptr;};
Difference Diff(std::span<const uint8_t> a,std::span<const uint8_t> b,size_t block_size=1) {
    Difference out;
    const auto size=std::max(a.size(),b.size());
    for(size_t block=0;block<size;block+=block_size) {
        bool changed=false;
        for(size_t i=block;i<std::min(size,block+block_size);++i) {
            if(i<a.size()&&i<b.size()&&a[i]==b[i])continue;
            if(out.first.is_null())out.first=i;
            ++out.bytes;changed=true;
        }
        out.blocks+=changed;
    }
    return out;
}
Json Compare(const Record& a,const Record& b) {
    Json levels=Json::array();uint64_t consumed=0,blocks=0;
    if(a.decoded.levels.size()!=b.decoded.levels.size())
        return {{"classification","different_resident_mip_layouts"}};
    for(size_t i=0;i<a.decoded.levels.size();++i) {
        const auto& x=a.decoded.levels[i];const auto& y=b.decoded.levels[i];
        const auto& xl=x.layout;const auto& yl=y.layout;
        if(std::tie(xl.mip,xl.layer,xl.width,xl.height,xl.depth)!=std::tie(yl.mip,yl.layer,yl.width,yl.height,yl.depth))
            return {{"classification","different_resident_mip_layouts"}};
        const auto diff=Diff(x.bytes,y.bytes,a.decoded.layout.block.bytes);
        consumed+=diff.bytes;blocks+=diff.blocks;
        levels.push_back({{"mip",xl.mip},{"layer",xl.layer},{"changed_bytes",diff.bytes},
            {"changed_blocks",diff.blocks},{"first_changed_byte",diff.first}});
    }
    const auto base=Diff(a.base,b.base),mips=Diff(a.mips,b.mips);
    return {{"classification",consumed?"consumed_blocks_differ":base.bytes||mips.bytes?"padding_only":"identical"},
        {"base_allocation_changed_bytes",base.bytes},{"mip_allocation_changed_bytes",mips.bytes},
        {"consumed_level_changed_bytes",consumed},{"consumed_changed_blocks",blocks},{"levels",levels}};
}
Json CompareCommonLevels(const Record& a,const Record& b) {
    Json levels=Json::array();uint64_t consumed=0,blocks=0;
    for(const auto& x:a.decoded.levels)for(const auto& y:b.decoded.levels) {
        const auto& xl=x.layout;const auto& yl=y.layout;
        if(a.decoded.layout.format!=b.decoded.layout.format||
            std::tie(xl.mip,xl.layer,xl.width,xl.height,xl.depth)!=std::tie(yl.mip,yl.layer,yl.width,yl.height,yl.depth))continue;
        const auto diff=Diff(x.bytes,y.bytes,a.decoded.layout.block.bytes);
        consumed+=diff.bytes;blocks+=diff.blocks;
        levels.push_back({{"mip",xl.mip},{"layer",xl.layer},{"width",xl.width},{"height",xl.height},{"depth",xl.depth},
            {"bytes",x.bytes.size()},{"left_sha256",picosha2::hash256_hex_string(x.bytes)},
            {"right_sha256",picosha2::hash256_hex_string(y.bytes)},{"changed_bytes",diff.bytes},
            {"changed_blocks",diff.blocks},{"first_changed_byte",diff.first}});
    }
    const bool all_matched=levels.size()==a.decoded.levels.size()&&levels.size()==b.decoded.levels.size();
    const auto base=Diff(a.base,b.base),mips=Diff(a.mips,b.mips);
    return {{"classification",levels.empty()?"no_comparable_levels":consumed?"consumed_blocks_differ":
        all_matched?base.bytes||mips.bytes?"padding_only":"identical":"matched_levels_identical_other_levels_unmatched"},
        {"all_levels_matched",all_matched},{"base_allocation_changed_bytes",base.bytes},
        {"mip_allocation_changed_bytes",mips.bytes},{"consumed_level_changed_bytes",consumed},
        {"consumed_changed_blocks",blocks},{"levels",levels}};
}
void WriteExclusive(const fs::path& path,const std::string& text) {
#ifdef _WIN32
    FILE* file=_wfopen(path.c_str(),L"wbx");
#else
    FILE* file=std::fopen(path.c_str(),"wbx");
#endif
    if(!file)throw std::runtime_error("Comparison output must be a new writable file");
    const bool written=std::fwrite(text.data(),1,text.size(),file)==text.size();
    const bool closed=std::fclose(file)==0;
    if(!written||!closed)throw std::runtime_error("Cannot finish comparison output");
}
int main(int argc,char** argv) try {
    if(argc<3||argc>10)throw std::runtime_error("Usage: gow3-native-texture-probe-compare NEW_OUTPUT.json PROBE_DIRECTORY [PROBE_DIRECTORY ...] (maximum 8 directories)");
    if(fs::exists(argv[1]))throw std::runtime_error("Comparison output already exists");
    std::vector<std::vector<Record>> directories;
    Json result={{"version",1},{"kind","native_texture_probe_comparison"},
        {"comparison_scope","Grouped by source base payload and fetch state with words 1 and 5 masked to 0xfff. Decoded bytes are untiled/endian-fixed native-format blocks or texels; BC compression is preserved. Different simulation frames are not an equivalence verdict."},
        {"directories",Json::array()},{"groups",Json::array()}};
    // Group -> directory -> consumed-byte signature -> records. Collapsing
    // padding-only variants keeps cross-directory comparisons bounded and clear.
    using Variants=std::map<std::string,std::vector<const Record*>>;
    std::map<std::string,std::map<size_t,Variants>> groups;
    size_t record_count=0;
    for(int i=2;i<argc;++i) {
        directories.push_back(Load(argv[i]));
        result["directories"].push_back({{"id",i-2},{"path",argv[i]},{"records",directories.back().size()}});
        record_count+=directories.back().size();
    }
    for(size_t i=0;i<directories.size();++i)for(const auto& record:directories[i])
        groups[record.group][i][record.signature].push_back(&record);
    size_t variants_count=0,comparisons_count=0;
    for(const auto& [identity,sources]:groups) {
        const auto key=Json::parse(identity);
        Json group={{"source_base_payload",key[0]},{"fetch_without_addresses",key[1]},
            {"variants",Json::array()},{"comparisons",Json::array()}};
        std::vector<std::pair<size_t,const Record*>> representatives;
        for(const auto& [directory,variants]:sources)for(const auto& [signature,records]:variants) {
            const auto& first=*records.front();Json instances=Json::array();
            for(const auto* record:records) {
                const auto& metadata=record->metadata;
                instances.push_back({{"file",record->path.filename().string()},
                    {"frame",metadata.at("frame")},{"event",metadata.at("event")},
                    {"memory_version",metadata.at("memory_version")},{"overlaid_bytes",metadata.at("overlaid_bytes")},
                    {"base_sha256",metadata.at("base").at("sha256")},{"mips_sha256",metadata.at("mips").at("sha256")}});
            }
            group["variants"].push_back({{"id",representatives.size()},{"directory",directory},
                {"format",first.decoded.layout.format},{"block_bytes",first.decoded.layout.block.bytes},
                {"levels",first.levels},{"records",instances}});
            representatives.emplace_back(directory,&first);++variants_count;
        }
        for(size_t a=0;a<representatives.size();++a)for(size_t b=a+1;b<representatives.size();++b) {
            // Within-directory variants already have different consumed hashes;
            // include them only when one input directory was supplied.
            if(directories.size()>1&&representatives[a].first==representatives[b].first)continue;
            auto comparison=Compare(*representatives[a].second,*representatives[b].second);
            comparison["left_variant"]=a;comparison["right_variant"]=b;
            comparison["raw_diff_scope"]="first record of each consumed-byte variant";
            group["comparisons"].push_back(std::move(comparison));++comparisons_count;
        }
        result["groups"].push_back(std::move(group));
    }
    // Streaming may update only sampler LOD bias between captures. This second
    // view compares the latest record for each asset, not sampling equivalence.
    std::map<std::string,std::map<size_t,const Record*>> latest;
    for(size_t i=0;i<directories.size();++i)for(const auto& record:directories[i]) {
        auto*& chosen=latest[record.metadata.at("source_base_payload").get<std::string>()][i];
        const auto time=[](const Record& r) {return std::pair(r.metadata.at("frame").get<uint64_t>(),r.metadata.at("event").get<uint64_t>());};
        if(!chosen||time(record)>time(*chosen))chosen=&record;
    }
    result["relaxed_latest_record_comparisons"]=Json::array();
    for(const auto& [base,sources]:latest)for(auto a=sources.begin();a!=sources.end();++a)
        for(auto b=std::next(a);b!=sources.end();++b) {
            auto comparison=CompareCommonLevels(*a->second,*b->second);
            comparison["source_base_payload"]=base;
            comparison["scope"]="Latest record per asset; compares consumed blocks at matching format/mip/layer/dimensions while ignoring other fetch differences. This does not validate sampling state or simulation-frame equivalence.";
            const auto describe=[](size_t directory,const Record& r) {
                return Json{{"directory",directory},{"file",r.path.filename().string()},{"fetch",r.metadata.at("fetch")},
                    {"frame",r.metadata.at("frame")},{"event",r.metadata.at("event")},{"memory_version",r.metadata.at("memory_version")}};
            };
            comparison["left"]=describe(a->first,*a->second);
            comparison["right"]=describe(b->first,*b->second);
            result["relaxed_latest_record_comparisons"].push_back(std::move(comparison));
        }
    Json summary={{"records",record_count},{"groups",groups.size()},{"consumed_byte_variants",variants_count},
        {"comparisons",comparisons_count},{"output",argv[1]}};
    result["summary"]=summary;
    WriteExclusive(argv[1],result.dump(2)+"\n");
    std::cout<<summary.dump()<<'\n';return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
