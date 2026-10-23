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
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace allocation_probe {
struct alignas(std::max_align_t) Header {size_t bytes;uint64_t epoch;};
uint64_t epoch=0;
bool active=false;
size_t live=0,peak=0,allocations=0;
struct Measurement {size_t peak_bytes,live_bytes,allocations;};
void Begin(){++epoch;active=true;live=peak=allocations=0;}
Measurement End(){active=false;return {peak,live,allocations};}
}
void* operator new(size_t bytes) {
    using namespace allocation_probe;
    if(bytes>SIZE_MAX-sizeof(Header))throw std::bad_alloc();
    auto* h=static_cast<Header*>(std::malloc(sizeof(Header)+(bytes?bytes:1)));
    if(!h)throw std::bad_alloc();
    h->bytes=bytes;h->epoch=active?epoch:0;
    if(active){live+=bytes;peak=std::max(peak,live);++allocations;}
    return h+1;
}
void operator delete(void* p)noexcept {
    if(!p)return;
    using namespace allocation_probe;
    auto* h=static_cast<Header*>(p)-1;
    if(active&&h->epoch==epoch)live-=h->bytes;
    std::free(h);
}
void operator delete(void* p,size_t)noexcept{::operator delete(p);}
void* operator new[](size_t bytes){return ::operator new(bytes);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete[](void* p,size_t)noexcept{::operator delete(p);}

using namespace gow3::renderer;
using Fetch=std::array<uint32_t,6>;
using Ranges=std::vector<std::pair<uint32_t,uint32_t>>;
using Json=nlohmann::json;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
// Unmodified pre-change algorithm. It intentionally retains original arithmetic,
// exception text and malformed-layout behavior rather than an idealized union.
Ranges OldRanges(const TextureLayout& t,bool mips) {
    Ranges ranges;
    constexpr uint32_t swaps[]{0,1,3,2};
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
    std::sort(ranges.begin(),ranges.end());
    size_t count=0;
    for(const auto& r:ranges) {
        if(count&&r.first<=ranges[count-1].second)ranges[count-1].second=std::max(ranges[count-1].second,r.second);
        else ranges[count++]=r;
    }
    ranges.resize(count);
    for(auto& [first,last]:ranges)last-=first;
    return ranges;
}
size_t cases=0,errors=0,empty=0;
void Check(const TextureLayout& layout) {
    for(bool mips:{false,true})for(bool coalesce:{false,true}) {
        ++cases;Ranges expected;std::string error;
        try{expected=OldRanges(layout,mips);}catch(const std::runtime_error& e){error=e.what();}
        if(!error.empty()) {
            ++errors;bool exact=false;
            try{(void)TextureMemoryRanges(layout,mips,coalesce);}catch(const std::runtime_error& e){exact=e.what()==error;}
            Require(exact,"TextureMemoryRanges changed original failure");continue;
        }
        const auto actual=TextureMemoryRanges(layout,mips,coalesce);
        Require(actual==expected,"Exact sorted physical byte ranges differ from old oracle");
        if(actual.empty())++empty;
        // libstdc++ and libc++ allocate the requested range-constructor size.
        // Bound retained payload independently of the much larger temp vector.
        Require(actual.capacity()<=std::max(size_t(1),actual.size()*2),"Temporary block capacity survived into cached ranges");
    }
}
Fetch Make(uint32_t format,uint32_t dimension,uint32_t endian,bool tiled,bool packed,
           uint32_t width=33,uint32_t height=17,uint32_t depth=5,uint32_t layers=1) {
    const uint32_t pitch=(width+31)/32;
    Fetch f{2u|(pitch<<22)|(tiled?1u<<31:0),0x100000u|format|(endian<<6),0,0,13u<<6,
        0x1000000u|(dimension<<9)|(packed?2048u:0)};
    if(dimension==0)f[2]=width-1;
    else if(dimension==2)f[2]=(width-1)|((height-1)<<11)|((depth-1)<<22);
    else {
        f[2]=(width-1)|((height-1)<<13);
        if(dimension==3)f[2]|=5u<<26;
        if(layers>1){f[1]|=1024u;f[2]|=(layers-1)<<26;}
    }
    return f;
}
void FetchCase(const Fetch& fetch) {
    TextureLayout layout;
    try{layout=DescribeTexture(fetch);}catch(const std::runtime_error&){return;}
    Check(layout);
}
void Fetches(const Json& value,std::set<Fetch>& out) {
    if(value.is_object()) {
        if(value.contains("fetch")&&value.at("fetch").is_array()&&value.at("fetch").size()==6)
            out.insert(value.at("fetch").get<Fetch>());
        for(const auto& [key,item]:value.items())if(key!="fetch")Fetches(item,out);
    }else if(value.is_array())for(const auto& item:value)Fetches(item,out);
}
Json MemoryCase(bool tiled) {
    auto fetch=Make(6,1,0,tiled,false,512,512);fetch[4]=0;fetch[5]=1u<<9;
    const auto layout=DescribeTexture(fetch);
    allocation_probe::Begin();const auto old=OldRanges(layout,false);const auto prior=allocation_probe::End();
    allocation_probe::Begin();const auto now=TextureMemoryRanges(layout,false,true);const auto next=allocation_probe::End();
    Require(now==old&&now.size()==1,"Full 512 square coverage changed");
    Require(next.peak_bytes<prior.peak_bytes/2,"Streaming compaction failed to reduce actual peak allocations");
    Require(next.live_bytes<=16,"Compacted retained footprint is not bounded by final ranges");
    return {{"tiled",tiled},{"old_peak_bytes",prior.peak_bytes},{"new_peak_bytes",next.peak_bytes},
        {"old_retained_bytes",prior.live_bytes},{"new_retained_bytes",next.live_bytes},
        {"old_allocations",prior.allocations},{"new_allocations",next.allocations}};
}
}
int main(int argc,char** argv)try {
    for(uint32_t format=0;format<64;++format)for(uint32_t dimension=0;dimension<4;++dimension)
        for(uint32_t endian=0;endian<4;++endian)for(bool tiled:{false,true})for(bool packed:{false,true})
            FetchCase(Make(format,dimension,endian,tiled,packed));
    for(uint32_t edge:{1u,3u,7u,8u,15u,16u,17u,31u,32u,33u,63u,64u,65u,127u,128u})
        for(uint32_t format:{2u,10u,6u,18u,20u,38u})for(uint32_t endian=0;endian<4;++endian)
            for(bool tiled:{false,true})for(bool packed:{false,true}) {
                FetchCase(Make(format,1,endian,tiled,packed,edge,edge,1));
                FetchCase(Make(format,2,endian,tiled,packed,edge,17,5));
            }
    for(uint32_t layers:{2u,4u,64u})for(bool tiled:{false,true})for(bool packed:{false,true})
        FetchCase(Make(18,1,3,tiled,packed,35,19,1,layers));
    // Whole tiles at nonzero macro coordinates and odd tile pitches. Compare
    // every covered byte interval with the original per-block oracle, including
    // overlaps, padding gaps, unaligned offsets and near-capacity failures.
    for(uint32_t format:{6u,18u,20u,38u})for(uint32_t endian=0;endian<4;++endian)
        for(uint32_t pitch:{32u,64u,96u,160u})for(uint32_t origin:{0u,32u,64u}) {
            auto t=DescribeTexture(Make(format,1,endian,true,false,128,128));
            t.levels.resize(1);auto& l=t.levels[0];l.width=32*t.block.block_width;
            l.height=64*t.block.block_height;l.x=origin;l.y=origin;l.pitch_blocks=pitch;
            l.offset=17;Check(t);
            t.base_address=512u*1024*1024-32768;Check(t);
            // Changing one edge forces the unchanged partial-tile calculation.
            t.base_address=0x100000;++l.width;Check(t);
        }
    // Linear volume rows, pitch overlap, depth offsets and sparse row padding.
    for(uint32_t format:{6u,18u,20u,38u})for(uint32_t endian=0;endian<4;++endian)
        for(uint32_t pitch:{1u,32u,96u}) {
            auto t=DescribeTexture(Make(format,2,endian,false,false,65,37,5));
            t.levels.resize(1);auto& l=t.levels[0];l.pitch_blocks=pitch;l.x=7;l.y=5;l.z=2;
            l.offset=9;Check(t);t.base_address=512u*1024*1024-16;Check(t);
        }
    // Minimum mip, mip-only residency, separate base/mip addresses and aliases.
    for(uint32_t minimum:{0u,1u,3u,7u}) {
        auto f=Make(20,1,1,true,true,128,64);f[4]=(minimum<<2)|(7u<<6);FetchCase(f);
        f[1]&=0xfffu;FetchCase(f);
        f[1]=0x100000u|20u|(1u<<6);f[5]=(f[5]&0xfffu)|0x100000u;FetchCase(f);
    }
    // Exercise conservative bounds/errors plus unsigned wrap and descending
    // aliases using the public layout input, with no new normalization.
    auto base=DescribeTexture(Make(2,1,3,false,false,17,3));
    for(uint64_t offset:{uint64_t(512u*1024*1024-32),uint64_t(512u*1024*1024),
                         UINT64_MAX,uint64_t(UINT64_MAX-0x100000u),uint64_t(UINT64_MAX-0x100010u)}) {
        auto changed=base;changed.levels.resize(1);changed.levels[0].offset=offset;Check(changed);
    }
    auto order=base;order.levels.resize(1);order.levels[0].width=3;order.levels[0].height=1;
    for(uint64_t offset:{64u,8u,32u,0u,9u}){auto l=order.levels.front();l.offset=offset;order.levels.push_back(l);}
    Check(order);
    std::reverse(order.levels.begin(),order.levels.end());Check(order);
    auto boundary=base;boundary.levels.resize(1);boundary.levels[0].width=4;boundary.levels[0].height=1;
    boundary.base_address=512u*1024*1024-4;boundary.endian=0;Check(boundary);
    ++boundary.levels[0].width;Check(boundary);
    Json captures=Json::array();
    for(int i=1;i<argc;++i) {
        std::ifstream input(argv[i]);Require(bool(input),"Missing capture JSON");Json value;input>>value;
        std::set<Fetch> fetches;Fetches(value,fetches);Require(!fetches.empty(),"Capture contains no fetch words");
        for(const auto& fetch:fetches)FetchCase(fetch);
        captures.push_back({{"path",argv[i]},{"unique_fetches",fetches.size()}});
    }
    const auto linear=MemoryCase(false),tiled=MemoryCase(true);
    std::cout<<Json{{"status","PASS"},{"range_cases",cases},{"matching_errors",errors},{"empty_regions",empty},
        {"allocation_cases",{linear,tiled}},{"captures",captures}}.dump(2)<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
