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

namespace {size_t allocations=0;bool fail_next_allocation=false;}
void* operator new(size_t bytes) {++allocations;if(fail_next_allocation){fail_next_allocation=false;throw std::bad_alloc();}if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
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

namespace {
std::array<Fetch,6> SetCollisions(const Fetch& seed) {
    std::array<Fetch,6> result{};size_t count=0;
    for(uint32_t i=0;i<1000000&&count<result.size();++i) {
        auto f=seed;f[3]=i;
        if((TextureLayoutCache::Bucket(f)&255)==(TextureLayoutCache::Bucket(seed)&255))result[count++]=f;
    }
    Require(count==result.size(),"Cannot construct same-set keys");return result;
}
void CollisionAndFailure() {
    const auto keys=SetCollisions(Make(20,1,1,true,true));
    TextureLayoutCache cache(true);TextureLayout fallback;
    for(size_t i=0;i<4;++i)Check(cache,keys[i]);
    Require(cache.statistics().entries==4&&cache.statistics().evictions==0,"Four same-set keys did not coexist");
    const auto retained=*(&cache.Get(keys[0],fallback)); // Key0 is now MRU, key1 LRU.
    const auto original=cache.statistics();
    auto invalid=keys[4];invalid[0]=(invalid[0]&~3u)|3u;
    // Find a failing key in the same set so it would replace the same victim.
    bool invalid_collision=false;
    for(uint32_t i=0;i<1000000;++i) {
        invalid[3]=i; // Reserved word does not alter descriptor validation.
        if((TextureLayoutCache::Bucket(invalid)&255)==(TextureLayoutCache::Bucket(keys[0])&255)) {
            invalid_collision=true;break;
        }
    }
    Require(invalid_collision,"Cannot construct same-set validation failure");
    Check(cache,invalid);
    Require(cache.statistics().evictions==original.evictions,"Validation failure evicted a resident layout");
    fail_next_allocation=true;bool failed=false;
    try{(void)cache.Get(keys[4],fallback);}catch(const std::bad_alloc&){failed=true;}
    fail_next_allocation=false;
    Require(failed,"Allocation failure injection was not observed");
    Require(cache.statistics().evictions==original.evictions,"Allocation failure replaced old metadata");
    Require(Same(retained,cache.Get(keys[0],fallback)),"Failure changed prior metadata");
    // Key0 remains MRU after its confirming hit; neither failure touched LRU.
    Check(cache,keys[4]);
    Require(cache.statistics().evictions==original.evictions+1,"Fifth key did not evict exactly one layout");
    auto before=cache.statistics();
    for(size_t i:{size_t(0),size_t(2),size_t(3),size_t(4)}) {
        const auto expected=DescribeTexture(keys[i]);const auto allocation_after_oracle=allocations;
        Require(Same(expected,cache.Get(keys[i],fallback)),"Resident metadata changed after LRU replacement");
        Require(allocations==allocation_after_oracle,"Four-way hit allocated");
    }
    Require(cache.statistics().hits==before.hits+4&&cache.statistics().misses==before.misses,"Wrong way was evicted");
    before=cache.statistics();Check(cache,keys[1]);
    Require(cache.statistics().misses==before.misses+1,"True LRU key survived replacement");
}
void BudgetRefusalPreservesVictim() {
    const auto keys=SetCollisions(Make(20,1,1,true,true));
    const size_t target_set=TextureLayoutCache::Bucket(keys[0])&255;
    TextureLayoutCache cache(true);TextureLayout fallback;
    std::array<const TextureLayout*,4> owners{};
    std::array<TextureLayout,4> originals{};
    for(size_t i=0;i<4;++i){owners[i]=&cache.Get(keys[i],fallback);originals[i]=*owners[i];}
    const Fetch large{2u|(256u<<22),0x1000u|2u|1024u,8191u|(255u<<13)|(63u<<26),0,13u<<6,0x2000u|(1u<<9)};
    const auto expected=DescribeTexture(large);
    const size_t incoming_bytes=expected.levels.capacity()*sizeof(TextureLevel);
    const size_t victim_bytes=owners[1]->levels.capacity()*sizeof(TextureLevel);
    bool pressure_reached=false;
    for(uint32_t i=0;i<16384;++i) {
        auto f=large;f[3]=i;
        if((TextureLayoutCache::Bucket(f)&255)==target_set)continue;
        const auto& result=cache.Get(f,fallback);
        Require(Same(DescribeTexture(f),result),"Pressure description differs from oracle");
        if(TextureLayoutCache::kMaximumLevelBytes-cache.statistics().level_bytes+victim_bytes<incoming_bytes) {
            pressure_reached=true;break;
        }
    }
    Require(pressure_reached,"Could not reach admission refusal pressure");
    // Touch key0: key1 must remain the next LRU victim after the refused large key.
    Require(&cache.Get(keys[0],fallback)==owners[0],"Pressure evicted excluded-set metadata");
    auto candidate=large;bool collision=false;
    for(uint32_t i=0;i<1000000;++i) {
        candidate[3]=i;
        if((TextureLayoutCache::Bucket(candidate)&255)==target_set){collision=true;break;}
    }
    Require(collision,"Cannot construct same-set budget refusal");
    const auto before=cache.statistics();
    const auto& rejected=cache.Get(candidate,fallback);
    Require(&rejected==&fallback&&Same(DescribeTexture(candidate),rejected),"Budget refusal changed exact fallback");
    const auto after=cache.statistics();
    Require(after.admissions==before.admissions&&after.evictions==before.evictions&&after.entries==before.entries&&
        after.level_bytes==before.level_bytes&&after.peak_level_bytes==before.peak_level_bytes&&
        after.misses==before.misses+1&&after.bypasses==before.bypasses+1,"Budget refusal changed resident state");
    for(size_t i=0;i<4;++i)Require(Same(*owners[i],originals[i]),"Budget refusal replaced an existing borrowed layout");
    // Equal-sized ordinary replacement fits; it must still evict key1.
    Check(cache,keys[4]);
    const auto admitted=cache.statistics();
    for(size_t i:{size_t(0),size_t(2),size_t(3),size_t(4)})Check(cache,keys[i]);
    Require(cache.statistics().misses==admitted.misses,"Budget refusal changed LRU order");
    Check(cache,keys[1]);
    Require(cache.statistics().misses==admitted.misses+1,"Budget refusal retained wrong victim");
}
}
int main(int argc,char** argv) try {
    for(bool associative:{false,true}) {
        TextureLayoutCache cache(associative);
        Require(cache.ways()==(associative?4u:1u),"Policy identity changed");
        for(uint32_t format=0;format<64;++format)for(uint32_t dimension=0;dimension<4;++dimension)
            for(uint32_t endian=0;endian<4;++endian)for(bool tiled:{false,true})for(bool packed:{false,true})
                Check(cache,Make(format,dimension,endian,tiled,packed));
        for(uint32_t dimension=0;dimension<4;++dimension)Check(cache,Make(6,dimension,0,false,false,true));
        const auto original=Make(20,1,1,true,true);
        for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit) {
            auto changed=original;changed[word]^=1u<<bit;Check(cache,changed);
        }
        const Fetch large{2u|(256u<<22),0x1000u|2u|1024u,8191u|(255u<<13)|(63u<<26),0,13u<<6,0x2000u|(1u<<9)};
        Require(DescribeTexture(large).levels.size()==TextureLayoutCache::kMaximumLevels,"Maximum layout fixture changed");
        TextureLayoutCache pressure(associative);
        for(uint32_t i=0;i<2048;++i){auto f=large;f[3]=i;Check(pressure,f);}
        Require(pressure.statistics().bypasses>0&&pressure.statistics().entries<=TextureLayoutCache::kEntries&&
            pressure.statistics().level_bytes<=TextureLayoutCache::kMaximumLevelBytes,"Pressure bypass/bounds changed");
    }
    CollisionAndFailure();
    BudgetRefusalPreservesVictim();
    Json captures=Json::array();
    for(int i=1;i<argc;++i) {
        std::ifstream input(argv[i]);Require(bool(input),"Capture fixture is missing");Json capture;input>>capture;
        std::vector<Fetch> fetches;ReadFetches(capture,fetches);Require(!fetches.empty(),"Capture contains no fetches");
        Json policies=Json::array();
        for(bool associative:{false,true}) {
            TextureLayoutCache cache(associative);
            for(const auto& f:fetches)Check(cache,f);
            const auto prior=cache.statistics();uint64_t baseline_digest=0,cached_digest=0;
            for(const auto& f:fetches) {
                baseline_digest+=Digest(DescribeTexture(f));TextureLayout local;cached_digest+=Digest(cache.Get(f,local));
            }
            Require(baseline_digest==cached_digest,"Captured layout checksum differs from exact oracle");
            const auto stats=cache.statistics();
            policies.push_back({{"ways",cache.ways()},{"warm_hits",stats.hits-prior.hits},{"warm_misses",stats.misses-prior.misses},
                {"warm_evictions",stats.evictions-prior.evictions},{"warm_bypasses",stats.bypasses-prior.bypasses},
                {"entries",stats.entries},{"retained_level_bytes",stats.level_bytes},{"digest",cached_digest}});
        }
        captures.push_back({{"path",argv[i]},{"fetches",fetches.size()},{"policies",policies}});
    }
    std::cout<<Json{{"status","PASS"},{"cases",cases},{"validation_errors",errors},{"cache_fixed_bytes",sizeof(TextureLayoutCache)},
        {"maximum_retained_level_bytes",TextureLayoutCache::kMaximumLevelBytes},{"captures",captures}}.dump(2)<<'\n';return 0;
} catch(const std::exception& error){fail_next_allocation=false;std::cerr<<error.what()<<'\n';return 1;}
