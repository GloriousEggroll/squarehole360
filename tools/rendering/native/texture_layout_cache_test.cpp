#include "renderer/native/texture_layout_cache.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <new>
#include <string>
#include <tuple>

namespace {size_t allocations=0;}
void* operator new(size_t bytes) {++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
using namespace gow3::renderer;
using Fetch=std::array<uint32_t,6>;
using Json=nlohmann::json;
namespace {
void Require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
bool Same(const TextureLevel& a,const TextureLevel& b) {
    return std::tie(a.mip,a.layer,a.width,a.height,a.depth,a.pitch_blocks,a.slice_rows,a.x,a.y,a.z,a.offset,a.mips)==
           std::tie(b.mip,b.layer,b.width,b.height,b.depth,b.pitch_blocks,b.slice_rows,b.x,b.y,b.z,b.offset,b.mips);
}
bool Same(const TextureLayout& a,const TextureLayout& b) {
    return std::tie(a.fetch,a.width,a.height,a.depth,a.layers,a.dimension,a.format,a.endian,a.min_mip,a.max_mip,
                   a.base_address,a.mip_address,a.tiled,a.block.block_width,a.block.block_height,a.block.bytes,a.block.components)==
           std::tie(b.fetch,b.width,b.height,b.depth,b.layers,b.dimension,b.format,b.endian,b.min_mip,b.max_mip,
                   b.base_address,b.mip_address,b.tiled,b.block.block_width,b.block.block_height,b.block.bytes,b.block.components)&&
           a.levels.size()==b.levels.size()&&std::equal(a.levels.begin(),a.levels.end(),b.levels.begin(),
               [](const auto& x,const auto& y){return Same(x,y);});
}
Fetch Make(uint32_t format,uint32_t dimension,uint32_t endian,bool tiled,bool packed,bool stacked=false) {
    Fetch f{2u|(4u<<22)|(tiled?1u<<31:0),0x1000u|format|(endian<<6)|(stacked?1024u:0),0,0,13u<<6,0x2000u|(dimension<<9)|(packed?2048u:0)};
    f[2]=dimension==0?127u:dimension==2?127u|(63u<<11)|(3u<<22):127u|(63u<<13);
    if(dimension==3)f[2]|=5u<<26;
    if(stacked)f[2]|=3u<<26;
    return f;
}
size_t cases=0,errors=0;
void Check(TextureLayoutCache& cache,const Fetch& fetch) {
    ++cases;TextureLayout expected,fallback;std::string direct_error;
    try {expected=DescribeTexture(fetch);}catch(const std::runtime_error& e){direct_error=e.what();}
    if(!direct_error.empty()) {
        ++errors;bool rejected=false;
        try {(void)cache.Get(fetch,fallback);}catch(const std::runtime_error& e){rejected=direct_error==e.what();}
        Require(rejected,"Cached description changed a validation error");return;
    }
    Require(Same(expected,cache.Get(fetch,fallback)),"Cached layout differs from exact DescribeTexture fields");
    const auto before=allocations;
    const auto& hit=cache.Get(fetch,fallback);
    Require(Same(expected,hit),"Repeated layout lookup changed fields");
    // All ordinary layouts in this oracle fit the retained metadata budget.
    if(&hit!=&fallback)Require(allocations==before,"Metadata cache hit allocated memory");
    Require(cache.statistics().level_bytes<=TextureLayoutCache::kMaximumLevelBytes,"Metadata bytes exceeded their bound");
    Require(cache.statistics().entries<=TextureLayoutCache::kEntries,"Metadata entry bound changed");
}
Fetch Collide(Fetch candidate,const Fetch& target) {
    for(uint32_t i=0;;++i) {
        candidate[3]=i;
        if(candidate!=target&&TextureLayoutCache::Bucket(candidate)==TextureLayoutCache::Bucket(target))return candidate;
        if(i==1000000)throw std::runtime_error("Cannot construct collision fixture");
    }
}
void ReadFetches(const Json& value,std::vector<Fetch>& output) {
    if(value.is_object()) {
        if(value.contains("fetch")&&value.at("fetch").is_array()&&value.at("fetch").size()==6)
            output.push_back(value.at("fetch").get<Fetch>());
        for(const auto& item:value.items())if(item.key()!="fetch")ReadFetches(item.value(),output);
    } else if(value.is_array())for(const auto& item:value)ReadFetches(item,output);
}
uint64_t Digest(const TextureLayout& value) {
    uint64_t result=value.width+value.height+value.depth+value.layers+value.levels.size();
    for(const auto& level:value.levels)result+=level.offset+level.pitch_blocks+level.width+level.mip;
    return result;
}
}
int main(int argc,char** argv) try {
    TextureLayoutCache cache;
    for(uint32_t format=0;format<64;++format)for(uint32_t dimension=0;dimension<4;++dimension)
        for(uint32_t endian=0;endian<4;++endian)for(bool tiled:{false,true})for(bool packed:{false,true})
            Check(cache,Make(format,dimension,endian,tiled,packed));
    for(uint32_t dimension=0;dimension<4;++dimension)Check(cache,Make(6,dimension,0,false,false,true));
    const auto original=Make(20,1,1,true,true);
    for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit) {
        auto changed=original;changed[word]^=1u<<bit;Check(cache,changed);
    }
    // Addresses, reserved sampler words and mip controls all remain in the key.
    TextureLayoutCache collisions;TextureLayout fallback;
    const auto other=Collide(original,original);
    Check(collisions,original);const auto* old=&collisions.Get(original,fallback);
    const auto old_copy=*old;
    auto invalid=original;invalid[0]=(invalid[0]&~3u)|3u;invalid=Collide(invalid,original);
    const auto prior=collisions.statistics();Check(collisions,invalid);
    Require(collisions.statistics().evictions==prior.evictions,"Failed description evicted a good entry");
    Require(&collisions.Get(original,fallback)==old&&Same(*old,old_copy),"Failed description damaged previous borrowed metadata");
    Check(collisions,other);Require(collisions.statistics().evictions==prior.evictions+1,"Exact colliding key failed to replace old layout");
    Check(collisions,original);
    // Exercise the maximum valid level count and pressure with distinct keys.
    // Oversized admissions return the caller's temporary; they never skip data.
    const Fetch large{2u|(256u<<22),0x1000u|2u|1024u,8191u|(255u<<13)|(63u<<26),0,13u<<6,0x2000u|(1u<<9)};
    const auto largest=DescribeTexture(large);Require(largest.levels.size()==TextureLayoutCache::kMaximumLevels,"Maximum mip/layer fixture changed");
    TextureLayoutCache pressure;
    for(uint32_t i=0;i<2048;++i) {auto f=large;f[3]=i;Check(pressure,f);}
    Require(pressure.statistics().bypasses>0&&pressure.statistics().level_bytes<=TextureLayoutCache::kMaximumLevelBytes,
        "Metadata pressure did not preserve the exact bounded fallback");
    Json captures=Json::array();
    for(int i=1;i<argc;++i) {
        std::ifstream input(argv[i]);Require(bool(input),"Capture fixture is missing");Json capture;input>>capture;
        std::vector<Fetch> fetches;ReadFetches(capture,fetches);Require(!fetches.empty(),"Capture fixture contains no texture fetches");
        TextureLayoutCache captured;for(const auto& f:fetches)Check(captured,f);
        uint64_t baseline_digest=0,cached_digest=0;
        const auto base_start=std::clock();
        for(const auto& f:fetches){std::atomic_signal_fence(std::memory_order_seq_cst);baseline_digest+=Digest(DescribeTexture(f));}
        const double direct_ms=1000.0*double(std::clock()-base_start)/CLOCKS_PER_SEC;
        const auto cache_start=std::clock();
        for(const auto& f:fetches){std::atomic_signal_fence(std::memory_order_seq_cst);TextureLayout local;cached_digest+=Digest(captured.Get(f,local));}
        const double cached_ms=1000.0*double(std::clock()-cache_start)/CLOCKS_PER_SEC;
        Require(baseline_digest==cached_digest,"Captured sequence cache changed exact layout checksum");
        const auto stats=captured.statistics();
        captures.push_back({{"path",argv[i]},{"fetches",fetches.size()},{"direct_cpu_ms",direct_ms},{"cached_cpu_ms",cached_ms},
            {"hits",stats.hits},{"misses",stats.misses},{"evictions",stats.evictions},{"bypasses",stats.bypasses},
            {"entries",stats.entries},{"retained_level_bytes",stats.level_bytes}});
    }
    std::cout<<Json{{"status","PASS"},{"cases",cases},{"validation_errors",errors},
        {"cache_fixed_bytes",sizeof(TextureLayoutCache)},{"maximum_retained_level_bytes",TextureLayoutCache::kMaximumLevelBytes},
        {"maximum_levels_per_entry",TextureLayoutCache::kMaximumLevels},{"pressure_bypasses",pressure.statistics().bypasses},
        {"captures",captures}}.dump(2)<<'\n';return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
