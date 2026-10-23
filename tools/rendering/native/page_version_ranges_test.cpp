#include "renderer/native/page-version-ranges.h"
#include "renderer/native/exact_lru_cache.h"
#include "renderer/native/texture_data.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>

namespace allocation_probe {bool fail_next=false;}
void* operator new(size_t bytes) {
    if(allocation_probe::fail_next){allocation_probe::fail_next=false;throw std::bad_alloc();}
    if(void* result=std::malloc(bytes?bytes:1))return result;
    throw std::bad_alloc();
}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete(void* value,size_t)noexcept{std::free(value);}
void* operator new[](size_t bytes){return ::operator new(bytes);}
void operator delete[](void* value)noexcept{::operator delete(value);}
void operator delete[](void* value,size_t)noexcept{::operator delete(value);}

using namespace gow3::renderer;
using Json=nlohmann::json;
using Fetch=std::array<uint32_t,6>;
namespace {
constexpr uint32_t kCapacity=512u*1024*1024,kPages=kCapacity/4096;
uint64_t checks=0,range_cases=0,max_cases=0,serial_cases=0;
void Check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
uint64_t Mix(uint64_t value) {
    value+=0x9e3779b97f4a7c15ull;value=(value^(value>>30))*0xbf58476d1ce4e5b9ull;
    value=(value^(value>>27))*0x94d049bb133111ebull;return value^(value>>31);
}
// Independent original inclusive page enumeration. It intentionally does not
// derive intervals, call the candidate builder or sort/merge its metadata.
std::set<uint32_t> OldPageSet(const NativeTextureByteRegions& bytes,uint64_t* visits=nullptr) {
    std::set<uint32_t> result;
    for(const auto& region:bytes)for(const auto& [first,last]:region) {
        Check(first<last&&last<=kCapacity,"Oracle received invalid byte interval");
        for(uint32_t page=first/4096;page<=(last-1)/4096;++page) {
            result.insert(page);if(visits)++*visits;
        }
    }
    return result;
}
uint64_t OldMax(const NativeTextureByteRegions& bytes,const std::vector<uint64_t>& versions) {
    uint64_t result=0;
    for(const auto& region:bytes)for(const auto& [first,last]:region)
        for(uint32_t page=first/4096;page<=(last-1)/4096;++page)result=std::max(result,versions.at(page));
    return result;
}
struct Memo {uint64_t serial=0,version=0;bool valid=false;};
uint64_t OldRead(Memo& memo,uint64_t serial,const NativeTextureByteRegions& bytes,
                 const std::vector<uint64_t>& versions,uint64_t& scans) {
    if(memo.valid&&memo.serial==serial)return memo.version;
    ++scans;memo.version=OldMax(bytes,versions);memo.serial=serial;memo.valid=true;return memo.version;
}
void SerialTrace(const NativeTextureByteRegions& bytes,const NativePageVersionRanges& pages,
                 const std::set<uint32_t>& sampled) {
    std::vector<uint64_t> epochs(kPages);uint64_t serial=0,old_scans=0,new_scans=0;
    Memo old,next;
    auto read=[&] {
        const auto expected=OldRead(old,serial,bytes,epochs,old_scans);
        const auto actual=ReadNativePageVersion(serial,next.serial,next.version,next.valid,[&] {
            ++new_scans;return ScanNativePageVersionRanges(pages,epochs);
        });
        ++serial_cases;Check(actual==expected&&old.serial==next.serial&&old.valid==next.valid,
            "Exact global serial gate changed texture version publication");
        Check(old_scans==new_scans,"Candidate changed original rescan/hit decisions");
        const auto before=new_scans;
        const auto hit=ReadNativePageVersion(serial,next.serial,next.version,next.valid,[]()->uint64_t {
            throw std::runtime_error("Memo hit invoked scanner");
        });
        Check(hit==actual&&new_scans==before,"Same serial memo hit performed a scan");
    };
    read();
    // Each label models the proven original page-epoch writer, including an
    // ownership-only invalidation with no queued upload. This does not execute
    // ResolveMemory or guest CPU/GPU concurrency.
    for(const char* writer:{"Define/store","InitializeBytes","Copy","RecordImportedBytes",
                            "InvalidateBytes/null","InvalidateBytes/queued","RecordSurfaceWrite"}) {
        (void)writer;++serial;
        if(!sampled.empty())epochs.at(*sampled.begin())=serial;
        read();
        uint32_t unrelated=0;
        while(unrelated<kPages&&sampled.contains(unrelated))++unrelated;
        if(unrelated<kPages){++serial;epochs.at(unrelated)=serial;read();}
    }
    const auto saved=next;bool rejected=false;
    try{(void)ReadNativePageVersion(serial+1,next.serial,next.version,next.valid,[]()->uint64_t {
        throw std::runtime_error("scanner failure");
    });}catch(const std::runtime_error& e){rejected=std::string(e.what())=="scanner failure";}
    Check(rejected&&next.serial==saved.serial&&next.version==saved.version&&next.valid==saved.valid,
        "Failed scan published an incomplete version memo");
}
std::pair<uint64_t,uint64_t> Compare(const NativeTextureByteRegions& bytes,
                                    const std::array<std::vector<uint64_t>,3>& versions) {
    ++range_cases;const auto unchanged=bytes;uint64_t old_visits=0;
    const auto expected=OldPageSet(bytes,&old_visits);
    const auto pages=BuildNativePageVersionRanges(bytes);
    std::set<uint32_t> actual;uint32_t previous_end=0;bool first=true;
    for(const auto& [start,end]:pages) {
        Check(start<end&&end<=kPages,"Candidate produced malformed page interval");
        Check(first||previous_end<start,"Candidate union left overlap or touching ranges");
        first=false;previous_end=end;
        for(uint32_t page=start;page<end;++page)Check(actual.insert(page).second,"Candidate visits a page twice");
    }
    Check(actual==expected,"Page union broadened or omitted original sampled page set");
    Check(bytes==unchanged,"Page union changed canonical byte regions");
    Check(NativeByteRangePageVisits(bytes)==old_visits,"Admission baseline visit count differs from old loop");
    Check(NativePageRangeVisits(pages)==expected.size(),"Union visit count differs from independent page set");
    Check(expected.size()<=old_visits,"Union introduced additional page visits");
    for(const auto& epochs:versions) {
        ++max_cases;Check(OldMax(bytes,epochs)==ScanNativePageVersionRanges(pages,epochs),
            "Adversarial/random page epochs changed exact max version");
    }
    SerialTrace(bytes,pages,expected);return {old_visits,expected.size()};
}
NativeTextureByteRegions Canonical(const TextureLayout& texture,uint32_t capacity=kCapacity) {
    NativeTextureByteRegions result;
    for(unsigned region=0;region<2;++region) {
        result[region]=TextureMemoryRanges(texture,region!=0);
        for(auto& [first,count]:result[region]) {
            if(uint64_t(first)+count>capacity)throw std::runtime_error("CPU texture exceeds physical storage");
            count+=first;
        }
    }
    return result;
}
Fetch MakeFetch(uint32_t format,uint32_t endian,bool tiled,bool packed,uint32_t dimension=1) {
    constexpr uint32_t width=33,height=17;
    Fetch f{2u|(((width+31)/32)<<22)|(tiled?1u<<31:0),0x100000u|format|(endian<<6),
        (width-1)|((height-1)<<13),0,5u<<6,0x1000000u|(dimension<<9)|(packed?2048u:0)};
    if(dimension==0){f[0]&=~(1u<<31);f[2]=width-1;f[4]=0;f[5]&=~2048u;}
    if(dimension==2)f[2]=(width-1)|((height-1)<<11)|(4u<<22);
    if(dimension==3)f[2]|=5u<<26;
    return f;
}
void FailureAndOwnership(const std::array<std::vector<uint64_t>,3>& epochs) {
    NativeTextureByteRegions bytes;
    bytes[0]={{7,8},{4095,4096},{8192,8193}};bytes[1]={{7,8},{4096,4097}};
    const auto original=bytes;
    const auto pages=BuildNativePageVersionRanges(bytes);
    // Byte ownership remains exact: neither gaps within pages nor allocation
    // padding become GPU-owned because version pages share an epoch.
    std::array<bool,12288> owned{};
    for(const auto& region:bytes)for(const auto& [first,last]:region)
        for(uint32_t byte=first;byte<last;++byte)owned[byte]=true;
    Check(owned[7]&&owned[4095]&&owned[4096]&&owned[8192]&&!owned[8]&&!owned[4097]&&!owned[8193],
        "Independent byte mask broadened original ownership");
    Check(bytes==original&&pages==NativePageVersionRanges{{0,3}},"Version union changed byte provenance");
    Compare(bytes,epochs);
    ExactLruCache<uint32_t,NativePageVersionRanges,1> cache;
    const auto old=cache.GetOrCreate(1,[&]{return pages;});
    allocation_probe::fail_next=true;bool failed=false;
    try{(void)cache.GetOrCreate(2,[&]{return BuildNativePageVersionRanges(bytes);});}
    catch(const std::bad_alloc&){failed=true;}
    allocation_probe::fail_next=false;
    Check(failed&&cache.statistics().entries==1&&cache.statistics().evictions==0,
        "Page metadata allocation failure evicted existing cache entry");
    Check(cache.GetOrCreate(1,[]()->NativePageVersionRanges{throw std::runtime_error("lost value");})==old,
        "Failed page builder changed existing immutable page value");
    for(auto malformed:std::array<std::pair<uint32_t,uint32_t>,2>{{{9,9},{10,9}}}) {
        NativeTextureByteRegions invalid;invalid[0].push_back(malformed);bool rejected=false;
        try{(void)BuildNativePageVersionRanges(invalid);}catch(const std::invalid_argument&){rejected=true;}
        Check(rejected,"Malformed page-builder interval underflowed or broadened pages");
    }
    const auto texture=DescribeTexture(MakeFetch(6,0,false,false));
    std::string error;
    try{(void)Canonical(texture,4096);}catch(const std::runtime_error& e){error=e.what();}
    Check(error=="CPU texture exceeds physical storage","Original pre-union capacity refusal changed");
}
Json Capture(const std::string& path,const std::array<std::vector<uint64_t>,3>& epochs,
             size_t expected_fetches,uint64_t expected_old,uint64_t expected_union) {
    std::ifstream input(path);Check(bool(input),"Cannot open actual captured scene");Json scene;input>>scene;
    std::map<Fetch,uint64_t> uses;
    for(const auto& operation:scene.at("operations"))if(operation.contains("textures"))
        for(const auto& texture:operation.at("textures"))if(texture.at("source")=="cpu_snapshot")
            ++uses[texture.at("fetch").get<Fetch>()];
    Check(!uses.empty(),"Capture has no CPU-snapshot texture fetches");
    if(expected_fetches)Check(uses.size()==expected_fetches,"Captured CPU fetch coverage differs from required424layouts");
    uint64_t bindings=0,old_weighted=0,union_weighted=0;
    for(const auto& [fetch,count]:uses) {
        const auto [old_pages,union_pages]=Compare(Canonical(DescribeTexture(fetch)),epochs);
        bindings+=count;old_weighted+=count*old_pages;union_weighted+=count*union_pages;
    }
    if(expected_old)Check(old_weighted==expected_old,"Captured old potential page visits changed");
    if(expected_union)Check(union_weighted==expected_union,"Captured union potential page visits changed");
    return {{"path",path},{"cpu_fetch_layouts",uses.size()},{"cpu_bindings",bindings},
        {"old_visits_if_all_bindings_rescanned",old_weighted},
        {"union_visits_if_all_bindings_rescanned",union_weighted},
        {"scope","Captured potential visits only; not observed game rescans or live timing"}};
}
} // namespace
int main(int argc,char** argv)try {
    std::array<std::vector<uint64_t>,3> epochs;
    for(size_t seed=0;seed<epochs.size();++seed) {
        epochs[seed].resize(kPages);
        for(uint32_t page=0;page<kPages;++page)epochs[seed][page]=seed==0?Mix(page):
            seed==1?(page&1?std::numeric_limits<uint64_t>::max()-page:page):Mix(uint64_t(page)^0x83aba5367f09d74cull);
    }
    std::vector<NativeTextureByteRegions> explicit_cases;
    auto add=[&](NativePageVersionRanges base,NativePageVersionRanges mips={}) {
        NativeTextureByteRegions next;next[0]=std::move(base);next[1]=std::move(mips);
        explicit_cases.push_back(std::move(next));
    };
    add({});add({{0,1}});add({{4095,4096},{4096,4097}});
    add({{0,1},{4095,4096},{8192,8193}},{{0,1},{4096,4097}});
    add({{12288,12289},{0,1}},{{8192,8193},{4096,4097}});
    add({{kCapacity-1,kCapacity}});add({{0,4096},{4096,8192}});
    add({{0,1},{12288,12289}},{{0,1},{12288,12289}});
    for(const auto& bytes:explicit_cases)Compare(bytes,epochs);
    for(uint64_t case_id=0;case_id<64;++case_id) {
        NativeTextureByteRegions bytes;
        for(unsigned region=0;region<2;++region)for(uint64_t piece=0;piece<1+case_id%32;++piece) {
            const uint64_t random=Mix(case_id*97+piece*3+region);
            const uint32_t start=uint32_t(random%32768);
            const uint32_t end=start+1+uint32_t(Mix(random)%16384);
            bytes[region].emplace_back(start,end);
        }
        Compare(bytes,epochs);
    }
    FailureAndOwnership(epochs);
    for(uint32_t format:{2u,10u,6u,32u,38u,18u,19u,20u,22u,23u})
        for(uint32_t endian=0;endian<4;++endian)for(bool tiled:{false,true})for(bool packed:{false,true}) {
            Compare(Canonical(DescribeTexture(MakeFetch(format,endian,tiled,packed))),epochs);
        }
    for(uint32_t dimension:{0u,2u,3u})for(bool tiled:{false,true}) {
        auto fetch=MakeFetch(6,2,tiled,false,dimension);Compare(Canonical(DescribeTexture(fetch)),epochs);
    }
    auto stacked=MakeFetch(38,3,true,true);stacked[1]|=1024;stacked[2]|=2u<<26;
    Compare(Canonical(DescribeTexture(stacked)),epochs);
    std::string capture;size_t expected_fetches=0;uint64_t expected_old=0,expected_union=0;
    for(int i=1;i<argc;++i) {
        const std::string option=argv[i];Check(i+1<argc,"Missing fixture argument value");
        if(option=="--capture")capture=argv[++i];
        else if(option=="--expected-fetches")expected_fetches=std::stoull(argv[++i]);
        else if(option=="--expected-old-visits")expected_old=std::stoull(argv[++i]);
        else if(option=="--expected-union-visits")expected_union=std::stoull(argv[++i]);
        else throw std::runtime_error("Unknown page-version fixture option");
    }
    Json actual=nullptr;
    if(!capture.empty())actual=Capture(capture,epochs,expected_fetches,expected_old,expected_union);
    else Check(!expected_fetches&&!expected_old&&!expected_union,"Required capture omitted");
    std::cout<<Json{{"status","PASS"},{"checks",checks},{"range_cases",range_cases},
        {"max_cases",max_cases},{"serial_cases",serial_cases},{"actual_capture",actual}}.dump(2)<<'\n';
}catch(const std::exception& error){allocation_probe::fail_next=false;std::cerr<<error.what()<<'\n';return 1;}
