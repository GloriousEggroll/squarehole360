#include "renderer/native/exact_lru_cache.h"
#include "renderer/native/gpu_alias_state.h"
#include "renderer/native/texture_data.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace allocation_probe {
long fail_after=-1;
size_t allocations=0;
}
void* operator new(size_t bytes) {
    using namespace allocation_probe;
    if(fail_after==0)throw std::bad_alloc();
    if(fail_after>0)--fail_after;
    ++allocations;
    if(void* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p)noexcept {std::free(p);}
void operator delete(void* p,size_t)noexcept {std::free(p);}
void* operator new[](size_t bytes){return ::operator new(bytes);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete[](void* p,size_t)noexcept{::operator delete(p);}

using namespace gow3::renderer;
using Key=std::array<uint32_t,6>;
using Ranges=std::vector<std::pair<uint32_t,uint32_t>>;
using Json=nlohmann::json;
namespace {
size_t checks=0,range_cases=0,version_cases=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
Key MakeKey(uint32_t value){Key k{};k[0]=value;return k;}
void PolicyAndLifetime() {
    struct Value {uint64_t memo=0;std::shared_ptr<const uint32_t> owner;};
    ExactLruCache<Key,Value,3> cache;
    size_t built=0;
    auto get=[&](unsigned key)->Value& {
        return cache.GetOrCreate(MakeKey(key),[&]{++built;return Value{0,std::make_shared<const uint32_t>(key)};});
    };
    std::weak_ptr<const uint32_t> first=get(1).owner,second=get(2).owner,third=get(3).owner;
    get(1).memo=91;
    get(4);
    Check(!first.expired()&&second.expired()&&!third.expired(),"eviction did not select exact least-recent key");
    Check(get(1).memo==91&&built==4,"retained version memo or identity changed on a hit");
    const auto before=allocation_probe::allocations;
    for(unsigned n=0;n<100;++n)get(1);
    Check(allocation_probe::allocations==before,"cache hits allocate storage");
    const auto stats=cache.statistics();
    Check(stats.entries==3&&stats.peak_entries==3&&stats.misses==4&&stats.admissions==4&&stats.evictions==1&&stats.hits==102,
          "hit/miss/admission/eviction counters differ from independent sequence");
    bool failed=false;
    try{cache.GetOrCreate(MakeKey(8),[]()->Value{throw std::runtime_error("original range validation");});}
    catch(const std::runtime_error& e){failed=std::string(e.what())=="original range validation";}
    Check(failed&&cache.statistics().entries==3&&cache.statistics().evictions==1&&!first.expired()&&!third.expired(),
          "builder validation failure mutated existing cache values");
    // Inject failure into the list node and map node separately, after a fully
    // built immutable value exists. Pending external owners remain valid.
    for(long allocation:{0L,1L}) {
        ExactLruCache<Key,Value,2> small;
        auto retained=std::make_shared<const uint32_t>(7);std::weak_ptr<const uint32_t> old=retained;
        small.GetOrCreate(MakeKey(1),[&]{return Value{0,retained};});
        small.GetOrCreate(MakeKey(2),[]{return Value{0,std::make_shared<const uint32_t>(8)};});
        std::shared_ptr<const uint32_t> candidate=std::make_shared<const uint32_t>(9);
        std::weak_ptr<const uint32_t> candidate_weak=candidate;
        allocation_probe::fail_after=allocation;bool bad_alloc=false;
        try{small.GetOrCreate(MakeKey(3),[&]{return Value{0,std::move(candidate)};});}
        catch(const std::bad_alloc&){bad_alloc=true;}
        allocation_probe::fail_after=-1;
        Check(bad_alloc&&candidate_weak.expired()&&!old.expired(),"failed admission leaked candidate or released external owner");
        Check(small.statistics().entries==1&&small.statistics().evictions==1,"failed admission violated entry bound");
        for(unsigned n=3;n<15;++n) {
            auto& value=small.GetOrCreate(MakeKey(n),[&]{return Value{n,std::make_shared<const uint32_t>(n)};});
            Check(value.memo==n&&small.statistics().entries<=2,"failed insertion stranded age state");
        }
    }
    // Exact keys include every fetch word; no hash or partially decoded key is used.
    ExactLruCache<Key,uint32_t,16> exact;Key zero{};
    exact.GetOrCreate(zero,[]{return 0u;});
    for(unsigned word=0;word<6;++word)for(uint32_t bit:{1u,0x80000000u}) {
        auto key=zero;key[word]=bit;
        Check(exact.GetOrCreate(key,[&]{return word+1;})==word+1,"distinct fetch word aliases another entry");
    }
    Check(exact.statistics().entries==13,"full six-word keys were not retained");
    // The production hard entry cap is exercised independently of small caches.
    ExactLruCache<Key,uint32_t,2048> full;
    for(uint32_t n=0;n<4099;++n)full.GetOrCreate(MakeKey(n),[&]{return n;});
    Check(full.statistics().entries==2048&&full.statistics().peak_entries==2048&&full.statistics().evictions==2051,
          "production hard entry cap exceeded");
}

// Independent original full-clear cache and range union. It intentionally
// computes each block before sorting, without using the new cache/helper.
Ranges OldRanges(const TextureLayout& t,bool mips,uint32_t capacity) {
    Ranges ranges;constexpr uint32_t swaps[]{0,1,3,2};
    const uint32_t address=mips?t.mip_address:t.base_address;
    for(const auto& level:t.levels) {
        if(level.mips!=mips)continue;
        const uint32_t columns=(level.width+t.block.block_width-1)/t.block.block_width;
        const uint32_t rows=(level.height+t.block.block_height-1)/t.block.block_height;
        for(uint32_t z=0;z<level.depth;++z)for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<columns;++x) {
            uint64_t offset=TextureBlockOffset(t,level,x,y,z);
            if(t.block.bytes<4)offset^=swaps[t.endian]&~(t.block.bytes-1);
            const uint64_t first=uint64_t(address)+offset,last=first+t.block.bytes;
            if(last>512u*1024*1024)throw std::runtime_error("Texture block exceeds physical memory");
            ranges.emplace_back(uint32_t(first),uint32_t(last));
        }
    }
    std::sort(ranges.begin(),ranges.end());size_t count=0;
    for(const auto& range:ranges) {
        if(count&&range.first<=ranges[count-1].second)ranges[count-1].second=std::max(ranges[count-1].second,range.second);
        else ranges[count++]=range;
    }
    ranges.resize(count);
    for(const auto& [first,last]:ranges)if(uint64_t(first)+(last-first)>capacity)
        throw std::runtime_error("CPU texture exceeds physical storage");
    return ranges;
}
struct Footprint {
    std::array<Ranges,2> ranges;
    uint64_t version_serial=0,version=0;bool version_valid=false;
};
Footprint NewRanges(const TextureLayout& t,uint32_t capacity) {
    Footprint result;
    for(unsigned region=0;region<2;++region) {
        result.ranges[region]=TextureMemoryRanges(t,region!=0);
        for(auto& [first,count]:result.ranges[region]) {
            if(uint64_t(first)+count>capacity)throw std::runtime_error("CPU texture exceeds physical storage");
            count+=first;
        }
    }
    return result;
}
struct Legacy {
    std::map<Key,Footprint> values;
    size_t misses=0,clears=0;
    Footprint& Get(const TextureLayout& t,uint32_t capacity) {
        if(auto found=values.find(t.fetch);found!=values.end())return found->second;
        ++misses;Footprint next;
        for(unsigned n=0;n<2;++n)next.ranges[n]=OldRanges(t,n!=0,capacity);
        if(values.size()>=16){values.clear();++clears;}
        return values.emplace(t.fetch,std::move(next)).first->second;
    }
};
uint64_t Version(Footprint& footprint,uint64_t serial,const std::vector<uint64_t>& pages) {
    if(footprint.version_valid&&footprint.version_serial==serial)return footprint.version;
    uint64_t version=0;
    for(const auto& region:footprint.ranges)for(const auto& [first,last]:region)
        for(uint32_t page=first/4096;page<=(last-1)/4096;++page)version=std::max(version,pages.at(page));
    footprint.version=version;footprint.version_serial=serial;footprint.version_valid=true;return version;
}
Key MakeFetch(uint32_t format,uint32_t endian,bool tiled,bool packed,uint32_t width=33,uint32_t height=17) {
    Key f{2u|(((width+31)/32)<<22)|(tiled?1u<<31:0),0x100000u|format|(endian<<6),
        (width-1)|((height-1)<<13),0,5u<<6,0x1000000u|(1u<<9)|(packed?2048u:0)};
    return f;
}
void Fetches(const Json& value,std::set<Key>& out) {
    if(value.is_object()) {
        if(value.contains("fetch")&&value.at("fetch").is_array()&&value.at("fetch").size()==6)
            out.insert(value.at("fetch").get<Key>());
        for(const auto& [key,child]:value.items())if(key!="fetch")Fetches(child,out);
    } else if(value.is_array())for(const auto& child:value)Fetches(child,out);
}
Json CompareLayouts(const std::vector<TextureLayout>& layouts) {
    constexpr uint32_t capacity=512u*1024*1024;
    std::vector<uint64_t> pages(capacity/4096);uint64_t serial=0;
    ExactLruCache<Key,Footprint,16> cache;Legacy legacy;
    auto visit=[&](const TextureLayout& layout) {
        ++range_cases;
        auto& actual=cache.GetOrCreate(layout.fetch,[&]{return NewRanges(layout,capacity);});
        auto& expected=legacy.Get(layout,capacity);
        Check(actual.ranges==expected.ranges,"LRU changed exact old byte ranges");
        // Independently define a deterministic page-version function; then
        // compare cache hits, unrelated writes and overlapping writes.
        for(unsigned action=0;action<3;++action) {
            if(action) {
                ++serial;
                uint32_t page=uint32_t((range_cases*17)%pages.size());
                if(action==2)for(const auto& r:actual.ranges)if(!r.empty()){page=r.front().first/4096;break;}
                pages.at(page)=serial;
            }
            const uint64_t a=Version(actual,serial,pages),b=Version(expected,serial,pages);
            ++version_cases;Check(a==b,"LRU changed serial-gated physical texture version");
        }
    };
    for(size_t i=0;i<layouts.size();++i) {
        visit(layouts[i]);
        if(i%8==0)for(size_t hot=0;hot<std::min(size_t(3),layouts.size());++hot)visit(layouts[hot]);
    }
    const auto stats=cache.statistics();
    Check(stats.entries<=16&&stats.peak_entries<=16,"captured range cache exceeded bound");
    return {{"layouts",layouts.size()},{"lru_hits",stats.hits},{"lru_misses",stats.misses},
        {"lru_evictions",stats.evictions},{"legacy_misses",legacy.misses},{"legacy_clears",legacy.clears}};
}
void InvalidLayout() {
    ExactLruCache<Key,Footprint,1> cache;
    auto valid=DescribeTexture(MakeFetch(6,0,false,false,4,4));
    cache.GetOrCreate(valid.fetch,[&]{return NewRanges(valid,512u*1024*1024);});
    auto invalid=valid;invalid.fetch[0]^=0x100;invalid.base_address=512u*1024*1024-1;
    std::string old_error,new_error;
    try{(void)OldRanges(invalid,false,512u*1024*1024);}catch(const std::exception& e){old_error=e.what();}
    try{cache.GetOrCreate(invalid.fetch,[&]{return NewRanges(invalid,512u*1024*1024);});}catch(const std::exception& e){new_error=e.what();}
    Check(!old_error.empty()&&old_error==new_error&&cache.statistics().evictions==0,
          "invalid layout changed exception or evicted existing value");
    auto& retained=cache.GetOrCreate(valid.fetch,[]()->Footprint{throw std::runtime_error("lost valid entry");});
    Check(!retained.ranges[0].empty(),"validation failure lost previous entry");
    old_error.clear();new_error.clear();
    try{(void)OldRanges(valid,false,1024);}catch(const std::exception& e){old_error=e.what();}
    try{(void)NewRanges(valid,1024);}catch(const std::exception& e){new_error=e.what();}
    Check(!old_error.empty()&&old_error==new_error,"physical capacity validation differs");
}

Json CheckGpuAliasState() {
    using FullKey=std::array<uint32_t,11>;
    std::map<FullKey,NativeGpuAliasFootprint> entries;
    NativeGpuAliasStateStatistics stats;uint64_t serial=0;
    std::map<uint32_t,uint32_t> defined;std::vector<uint64_t> pages(4);
    FullKey key{};entries.emplace(key,NativeGpuAliasFootprint{{{3,9},{15,17},{25,28}}, {}});
    // Independent byte oracle does not use predecessor searches or the memo.
    auto oracle=[&](const NativeGpuAliasFootprint& entry) {
        NativeGpuAliasState expected;
        for(auto [first,last]:entry.ranges)for(uint32_t byte=first;byte<last;++byte) {
            bool known=false;for(auto [begin,end]:defined)known|=byte>=begin&&byte<end;
            expected.any_defined|=known;expected.fully_defined&=known;
            expected.version=std::max(expected.version,pages.at(byte/4096));
        }
        return expected;
    };
    auto read=[&](const FullKey& k) {
        auto& entry=entries.at(k);const auto expected=oracle(entry);
        const auto got=ReadNativeGpuAliasState(entry,serial,defined,pages,stats);
        Check(got==expected,"GPU alias state differs from independent byte oracle");
        const auto hits=stats.hits;allocation_probe::fail_after=0;
        const auto repeated=ReadNativeGpuAliasState(entry,serial,defined,pages,stats);
        allocation_probe::fail_after=-1;
        Check(repeated==got&&stats.hits==hits+1,"Same-serial GPU alias hit allocates or changes state");
        return got;
    };
    Check(!read(key).any_defined,"Initial serial zero falsely marks memory defined");
    // All256 definitions of eight4-byte blocks: disjoint islands, one-byte
    // holes in sampled ranges, exact endpoint touches, and full sparse unions.
    for(uint32_t mask=0;mask<256;++mask) {
        defined.clear();
        for(uint32_t block=0;block<8;) {
            if(!(mask&(1u<<block))){++block;continue;}
            const auto first=block*4;do{++block;}while(block<8&&(mask&(1u<<block)));
            defined.emplace(first,block*4);
        }
        AdvanceNativeGpuAliasStateSerial(serial,entries);pages[0]=serial;read(key);
    }
    entries.at(key).ranges={{4092,4100},{8192,8200}};
    entries.at(key).memo.valid=false;defined={{4092,4096}};pages={1,2,77,0};
    AdvanceNativeGpuAliasStateSerial(serial,entries);
    auto got=read(key);Check(got.any_defined&&!got.fully_defined&&got.version==77,
        "Partial first range truncated later maximum-page version");
    defined={{4092,4100},{8192,8200}};AdvanceNativeGpuAliasStateSerial(serial,entries);
    Check(read(key).fully_defined,"Unsampled bounding gap was treated as missing data");
    defined={{4092,4096},{4097,4100},{8192,8200}};
    AdvanceNativeGpuAliasStateSerial(serial,entries);pages[1]=serial;
    Check(!read(key).fully_defined,"Single-byte invalidation retained a full-state memo");
    defined={{4092,4100},{8192,8200}};AdvanceNativeGpuAliasStateSerial(serial,entries);pages[1]=serial;
    const auto repaired=read(key);Check(repaired.fully_defined,"Same-byte redefine did not replace partial memo");
    AdvanceNativeGpuAliasStateSerial(serial,entries);pages[3]=serial;
    Check(read(key)==repaired,"Unrelated-page write changed exact sampled state");
    AdvanceNativeGpuAliasStateSerial(serial,entries);pages[2]=serial;
    got=read(key);Check(got.fully_defined&&got.version==serial,
        "RecordSurfaceWrite-style page update reused mutation-only memo");
    // Each fetch/address/rectangle word belongs to the existing exact map key.
    // New entries never inherit another key's memo, even at the same serial.
    for(size_t word=0;word<key.size();++word)for(uint32_t bit:{1u,0x80000000u}) {
        auto other=key;other[word]=bit;
        entries.emplace(other,NativeGpuAliasFootprint{{{0,1}}, {}});
        const auto scans=stats.scans;Check(!read(other).any_defined&&stats.scans==scans+1,
            "GPU alias state dropped a full-key word or reused another key's memo");
    }
    const auto retained=got;entries.clear();entries.emplace(key,NativeGpuAliasFootprint{{{0,1}}, {}});
    Check(!read(key).any_defined&&retained.fully_defined,
        "Footprint clear/re-admission reused stale state or changed a returned copy");
    // A complete failed rescan must leave the previous memo intact.
    auto& entry=entries.at(key);const auto old_memo=entry.memo;
    entry.ranges.push_back({16384,16388});AdvanceNativeGpuAliasStateSerial(serial,entries);
    bool threw=false;try{(void)ReadNativeGpuAliasState(entry,serial,defined,pages,stats);}
    catch(const std::invalid_argument&){threw=true;}
    Check(threw&&entry.memo.serial==old_memo.serial&&entry.memo.value==old_memo.value&&entry.memo.valid,
        "Failed full scan published a partial memo");entry.ranges.pop_back();
    // The active Define caller may retain this ranges reference across wrap.
    const auto* ranges_data=entry.ranges.data();const auto ranges_before=entry.ranges;
    serial=UINT64_MAX;entry.memo.serial=0;entry.memo.valid=true;
    Check(AdvanceNativeGpuAliasStateSerial(serial,entries)&&serial==0&&!entry.memo.valid&&
          entry.ranges.data()==ranges_data&&entry.ranges==ranges_before,
          "Serial wrap reused an old memo or invalidated active footprint ranges");
    read(key);
    Check(stats.requests==stats.hits+stats.scans&&stats.hits>0&&stats.pages_scanned>0,
        "GPU alias state mechanism accounting is inconsistent");
    return {{"passed",true},{"definition_masks",256},{"full_key_word_witnesses",22},
        {"requests",stats.requests},{"hits",stats.hits},{"scans",stats.scans},
        {"ranges_scanned",stats.ranges_scanned},{"pages_scanned",stats.pages_scanned},
        {"serial_zero",true},{"partial_and_redefine",true},{"unrelated_write",true},
        {"page_only_write",true},{"cache_clear",true},{"failed_scan",true},{"serial_wrap",true}};
}

}
int main(int argc,char** argv)try {
    PolicyAndLifetime();InvalidLayout();const auto gpu_alias=CheckGpuAliasState();std::vector<TextureLayout> synthetic;
    for(uint32_t format:{2u,6u,10u,18u,20u,22u,23u,31u,36u,37u,38u})
        for(uint32_t endian=0;endian<4;++endian)for(bool tiled:{false,true})for(bool packed:{false,true})
            synthetic.push_back(DescribeTexture(MakeFetch(format,endian,tiled,packed)));
    Json reports=Json::array();reports.push_back(CompareLayouts(synthetic));
    for(int i=1;i<argc;++i) {
        std::ifstream file(argv[i]);Check(bool(file),"missing captured plan");Json value;file>>value;
        std::set<Key> fetches;Fetches(value,fetches);Check(!fetches.empty(),"captured plan has no fetches");
        std::vector<TextureLayout> layouts;for(const auto& fetch:fetches)layouts.push_back(DescribeTexture(fetch));
        auto report=CompareLayouts(layouts);report["path"]=argv[i];reports.push_back(std::move(report));
    }
    std::cout<<Json{{"status","PASS"},{"checks",checks},{"range_cases",range_cases},{"version_cases",version_cases},{"gpu_alias_state",gpu_alias},{"workloads",reports}}.dump(2)<<'\n';
} catch(const std::exception& e){allocation_probe::fail_after=-1;std::cerr<<e.what()<<'\n';return 1;}
