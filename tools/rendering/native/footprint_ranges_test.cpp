#include "renderer/native/footprint_ranges.h"
#include "renderer/native/texture_data.h"
#include <nlohmann/json.hpp>
#include <array>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <string>

// Fault only the next ordinary allocation; the production helper's replacement
// vector must fail before it swaps or alters any retained range list.
namespace {bool fail_next_allocation=false;}
void* operator new(std::size_t bytes) {
    if(fail_next_allocation){fail_next_allocation=false;throw std::bad_alloc{};}
    if(auto p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
uint64_t checks=0,cases=0,visited=0,captured=0,surface_cases=0;
uint64_t old_retained=0,new_retained=0,old_temporary=0,new_temporary=0;
uint64_t maximum_old=0,maximum_new=0;
void Require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Rect {int32_t left=0,top=0,right=0,bottom=0;bool Empty() const{return right<=left||bottom<=top;}};
struct Stats {uint64_t validations=0;size_t temporary=0,retained=0;};
FootprintRanges SortedUnion(FootprintRanges ranges) {
    std::sort(ranges.begin(),ranges.end());size_t count=0;
    for(const auto& range:ranges) {
        if(count&&range.first<=ranges[count-1].second)
            ranges[count-1].second=std::max(ranges[count-1].second,range.second);
        else ranges[count++]=range;
    }
    ranges.resize(count);return ranges;
}
// Original GPU Footprint enumeration/reserve and candidate actual helper. All
// addressing comes from the real DescribeTexture/TextureBlockOffset functions,
// with the unchanged source validation order and exact failure strings.
FootprintRanges Build(const TextureLayout& t,Rect r,uint32_t address,uint32_t capacity,bool compact,Stats& stats) {
    if(address%4||t.levels.size()!=t.layers||!t.layers||t.min_mip||t.max_mip||t.block.block_width!=1||t.dimension!=1||
       r.Empty()||r.left<0||r.top<0||r.right>int32_t(t.width)||r.bottom>int32_t(t.height))
        throw std::runtime_error("Unsupported storage footprint");
    FootprintRanges ranges;
    if(!compact)ranges.reserve(size_t(r.right-r.left)*(r.bottom-r.top)*t.layers);
    for(const auto& level:t.levels)for(int32_t y=r.top;y<r.bottom;++y)for(int32_t x=r.left;x<r.right;++x) {
        ++stats.validations;
        uint64_t first=TextureBlockOffset(t,level,uint32_t(x),uint32_t(y),0)+address;
        constexpr uint32_t swaps[]{0,1,3,2};
        if(t.block.bytes<4)first^=swaps[t.endian]&~(t.block.bytes-1u);
        if(first+t.block.bytes>capacity)throw std::runtime_error("Resolved texture exceeds backing allocation");
        if(compact)AppendFootprintRange(ranges,uint32_t(first),uint32_t(first+t.block.bytes));
        else ranges.emplace_back(uint32_t(first),uint32_t(first+t.block.bytes));
    }
    stats.temporary=ranges.size();ranges=SortedUnion(std::move(ranges));
    if(compact)ReleaseFootprintRangeCapacity(ranges);
    stats.retained=ranges.capacity()*sizeof(FootprintRanges::value_type);return ranges;
}
struct Outcome {FootprintRanges ranges;Stats stats;std::string error;};
Outcome Call(const TextureLayout& t,Rect r,uint32_t address,uint32_t capacity,bool compact) {
    Outcome result;try{result.ranges=Build(t,r,address,capacity,compact,result.stats);}
    catch(const std::exception& e){result.error=e.what();}return result;
}
uint64_t Version(const FootprintRanges& ranges) {
    uint64_t result=0;
    for(const auto [first,last]:ranges)for(uint32_t page=first/4096;page<=(last-1)/4096;++page)
        result=std::max(result,uint64_t((page*0x9e3779b9u)^(page>>7)));
    return result;
}
std::set<uint32_t> Pages(const FootprintRanges& ranges) {
    std::set<uint32_t> result;
    for(const auto [first,last]:ranges)for(uint32_t page=first/4096;page<=(last-1)/4096;++page)result.insert(page);
    return result;
}
bool Overlap(const FootprintRanges& ranges,uint32_t first,uint32_t last) {
    for(const auto& range:ranges)if(first<range.second&&last>range.first)return true;
    return false;
}
void IndependentBytes(const TextureLayout& t,Rect r,uint32_t address,const FootprintRanges& ranges) {
    const uint32_t first=ranges.front().first,last=ranges.back().second;
    // Exhaust every byte for small layouts. Captured large cases use exact
    // vectors/page sets instead of allocating another full-size byte oracle.
    if(uint64_t(last)-first>256u*1024)return;
    std::vector<uint8_t> direct(size_t(last-first)),from_ranges(direct.size());
    std::set<uint32_t> direct_pages;
    constexpr uint32_t swaps[]{0,1,3,2};
    for(const auto& level:t.levels)for(int32_t y=r.top;y<r.bottom;++y)for(int32_t x=r.left;x<r.right;++x) {
        uint64_t at=TextureBlockOffset(t,level,uint32_t(x),uint32_t(y),0)+address;
        if(t.block.bytes<4)at^=swaps[t.endian]&~(t.block.bytes-1u);
        for(uint32_t byte=0;byte<t.block.bytes;++byte){direct.at(size_t(at+byte-first))=1;direct_pages.insert(uint32_t(at+byte)/4096);}
    }
    for(const auto& range:ranges)for(uint32_t at=range.first;at<range.second;++at)from_ranges.at(at-first)=1;
    Require(direct==from_ranges,"Compaction invented or omitted byte ownership inside sparse storage");
    Require(direct_pages==Pages(ranges),"Sampled page-version coverage differs from independent point bytes");
}
void Compare(const TextureLayout& t,Rect r,uint32_t address,uint32_t capacity=512u*1024*1024) {
    const auto old=Call(t,r,address,capacity,false),next=Call(t,r,address,capacity,true);++cases;
    Require(old.error==next.error&&old.stats.validations==next.stats.validations,
        "Compaction changed point validation count/order or malformed failure");
    if(!old.error.empty())return;
    Require(old.ranges==next.ranges,"Exact sorted storage footprint changed");
    Require(next.stats.temporary<=old.stats.temporary,"Consecutive compaction increased temporary intervals");
    Require(next.stats.retained==next.ranges.size()*sizeof(FootprintRanges::value_type),
        "Final cached footprint retained temporary spare capacity");
    Require(Pages(old.ranges)==Pages(next.ranges)&&Version(old.ranges)==Version(next.ranges),
        "Full sampled page/version coverage changed");
    Require(!next.ranges.empty(),"Admitted nonempty rectangle lost its ownership footprint");
    IndependentBytes(t,r,address,next.ranges);
    for(size_t i=0;i<old.ranges.size();++i) {
        const auto [first,last]=old.ranges[i];
        for(const auto probe:std::array<std::pair<uint32_t,uint32_t>,3>{{{first,first+1},{last-1,last},{last,last+1}}})
            Require(Overlap(old.ranges,probe.first,probe.second)==Overlap(next.ranges,probe.first,probe.second),
                "CPU write/alias overlap at a sampled endpoint changed");
        if(i+1<old.ranges.size()&&last<old.ranges[i+1].first)
            Require(!Overlap(next.ranges,last,last+1),"Compaction falsely owned an unsampled gap");
    }
    visited+=next.stats.validations;old_retained+=old.stats.retained;new_retained+=next.stats.retained;
    old_temporary+=old.stats.temporary;new_temporary+=next.stats.temporary;
    maximum_old=std::max(maximum_old,uint64_t(old.stats.temporary));
    maximum_new=std::max(maximum_new,uint64_t(next.stats.temporary));
}
std::array<uint32_t,6> Fetch(uint32_t width,uint32_t height,uint32_t pitch,uint32_t format,uint32_t endian,
                            bool tiled,bool packed,uint32_t layers) {
    return {2u|((pitch/32)<<22)|(tiled?1u<<31:0),0x1000u|format|(endian<<6)|(layers>1?1024u:0),
        (width-1)|((height-1)<<13)|((layers>1?layers-1:0)<<26),0,0,(1u<<9)|(packed?2048u:0)};
}
void SyntheticLayouts() {
    for(uint32_t format:{2u,10u,6u,32u,38u})for(uint32_t endian=0;endian<4;++endian)
    for(bool tiled:{false,true})for(bool packed:{false,true})for(uint32_t layers:{1u,3u})
    for(uint32_t width:{1u,2u,7u,16u,31u,33u,65u})for(uint32_t height:{1u,2u,7u,17u}) {
        const uint32_t pitch=(width+31)&~31u;
        const auto t=DescribeTexture(Fetch(width,height,pitch,format,endian,tiled,packed,layers));
        const Rect full{0,0,int32_t(width),int32_t(height)};
        Compare(t,full,0x2000);
        if(width>2&&height>2)Compare(t,{1,1,int32_t(width-1),int32_t(height-1)},0x4004);
    }
    // Legal overlapping rows and alternating endian pairs still require every
    // original point even when a previous temporary run already owns its bytes.
    for(uint32_t endian=0;endian<4;++endian)for(bool tiled:{false,true}) {
        const auto t=DescribeTexture(Fetch(65,17,32,2,endian,tiled,false,1));Compare(t,{0,0,65,17},0);
    }
    const auto compressed=DescribeTexture(Fetch(16,16,32,18,0,true,false,1));
    Compare(compressed,{0,0,16,16},0x1000); // Actual BC layout remains refused.
    auto mip_fetch=Fetch(33,17,64,6,2,true,false,1);mip_fetch[5]|=0x10000;mip_fetch[4]|=1u<<6;
    const auto mip_layout=DescribeTexture(mip_fetch);Compare(mip_layout,{0,0,33,17},0x1000);
    const auto t=DescribeTexture(Fetch(33,17,64,10,3,true,true,3));const Rect r{0,0,33,17};
    Stats stats;const auto ranges=Build(t,r,0x4000,512u*1024*1024,false,stats);
    Compare(t,r,0x4000,ranges.back().second);Compare(t,r,0x4000,ranges.back().second-1);
    Compare(t,r,0x4000,0x4000);Compare(t,r,0xfffffffc,512u*1024*1024);
    for(const Rect bad:std::array{Rect{0,0,0,1},Rect{-1,0,1,1},Rect{0,-1,1,1},Rect{0,0,34,17},Rect{0,0,33,18}})
        Compare(t,bad,0x4000);
    Compare(t,r,0x4001);
    for(uint32_t bad=0;bad<6;++bad) {
        auto altered=t;
        if(bad==0)altered.dimension=2;
        if(bad==1)altered.layers=0;
        if(bad==2)altered.levels.pop_back();
        if(bad==3)altered.block.block_width=4;
        if(bad==4)altered.min_mip=1;
        if(bad==5)altered.max_mip=1;
        Compare(altered,r,0x4000);
    }
}
void CapacityAndFailure() {
    FootprintRanges intervals;intervals.reserve(10000);
    for(const auto range:std::array<std::pair<uint32_t,uint32_t>,6>{{{100,104},{96,100},{104,108},{200,204},{201,203},{108,112}}})
        AppendFootprintRange(intervals,range.first,range.second);
    Require(intervals==FootprintRanges{{96,108},{200,204},{108,112}},
        "Consecutive descending/overlap compaction crossed an untouching gap");
    intervals=SortedUnion(std::move(intervals));Require(intervals==FootprintRanges{{96,112},{200,204}},
        "Unchanged final union did not merge nonconsecutive touching runs");
    const auto before=intervals;const auto* data=intervals.data();const auto capacity=intervals.capacity();
    fail_next_allocation=true;bool rejected=false;
    try{ReleaseFootprintRangeCapacity(intervals);}catch(const std::bad_alloc&){rejected=true;}
    Require(rejected&&!fail_next_allocation&&intervals==before&&intervals.data()==data&&intervals.capacity()==capacity,
        "Failed metadata shrink changed a retained accepted range list");
    ReleaseFootprintRangeCapacity(intervals);
    Require(intervals==before&&intervals.capacity()==intervals.size(),"Metadata shrink changed values or retained spare capacity");
    fail_next_allocation=true;rejected=false;
    try{AppendFootprintRange(intervals,400,404);}catch(const std::bad_alloc&){rejected=true;}
    Require(rejected&&!fail_next_allocation&&intervals==before,"Failed disjoint admission corrupted its existing prefix");
    FootprintRanges empty;empty.reserve(1000);ReleaseFootprintRangeCapacity(empty);
    Require(empty.empty()&&!empty.capacity(),"Empty exact range list retained temporary storage");
    // Surface path changes only this same capacity helper; its already-built
    // tile-row vectors and their final sorted union remain byte-identical.
    for(uint32_t pieces:{1u,2u,17u,360u,720u}) {
        FootprintRanges surface;surface.reserve(size_t(pieces)*2);
        for(uint32_t i=0;i<pieces;++i)surface.emplace_back(i*320,i*320+320);
        surface=SortedUnion(std::move(surface));const auto original=surface;
        ReleaseFootprintRangeCapacity(surface);++surface_cases;
        Require(surface==original&&surface.capacity()==surface.size(),"Surface-only capacity release changed its original final union");
    }
}
void Capture(const std::string& path) {
    std::ifstream input(path);Require(bool(input),"Cannot open supplied capture scene");Json scene;input>>scene;
    std::set<std::array<uint32_t,11>> keys;
    auto add=[&](const std::array<uint32_t,6>& fetch,Rect rect,uint32_t address) {
        const auto t=DescribeTexture(fetch);
        if(t.dimension!=1||t.block.block_width!=1||t.min_mip||t.max_mip||t.levels.size()!=t.layers)return;
        std::array<uint32_t,11> key;std::copy(fetch.begin(),fetch.end(),key.begin());
        key[6]=address;key[7]=uint32_t(rect.left);key[8]=uint32_t(rect.top);key[9]=uint32_t(rect.right);key[10]=uint32_t(rect.bottom);
        if(keys.insert(key).second){Compare(t,rect,address);++captured;}
    };
    for(const auto& op:scene.at("operations")) {
        const auto type=op.at("type").get<std::string>();
        if(type=="draw")for(const auto& texture:op.at("textures")) {
            const auto source=texture.at("source").get<std::string>();
            if(source!="gpu_resolve"&&source!="gpu_alias_or_previous_frame")continue;
            const auto fetch=texture.at("fetch").get<std::array<uint32_t,6>>();const auto t=DescribeTexture(fetch);
            add(fetch,{0,0,int32_t(t.width),int32_t(t.height)},uint32_t(std::stoul(texture.at("identity").at("base").get<std::string>(),nullptr,16)));
        } else if(type=="resolve") {
            const auto rect=op.at("destination_rectangle").get<std::array<int32_t,4>>();
            add(op.at("destination_fetch").get<std::array<uint32_t,6>>(),{rect[0],rect[1],rect[2],rect[3]},
                uint32_t(std::stoul(op.at("destination").at("base").get<std::string>(),nullptr,16)));
        } else if(type=="present"&&op.contains("frontbuffer_fetch")) {
            const auto fetch=op.at("frontbuffer_fetch").get<std::array<uint32_t,6>>();const auto t=DescribeTexture(fetch);
            add(fetch,{0,0,int32_t(t.width),int32_t(t.height)},t.base_address);
        }
    }
    Require(captured>0,"Supplied capture had no eligible GPU storage footprint keys");
}
}
int main(int argc,char** argv) try {
    Require(argc==1||(argc==3&&std::string(argv[1])=="--capture"),"usage: footprint-ranges-test [--capture scene.json]");
    CapacityAndFailure();SyntheticLayouts();if(argc==3)Capture(argv[2]);
    std::cout<<Json{{"passed",true},{"checks",checks},{"layout_cases",cases},{"captured_keys",captured},
        {"surface_capacity_cases",surface_cases},{"point_validations",visited},
        {"old_temporary_intervals",old_temporary},{"new_temporary_intervals",new_temporary},
        {"maximum_old_temporary_intervals",maximum_old},{"maximum_new_temporary_intervals",maximum_new},
        {"old_retained_interval_bytes",old_retained},{"new_retained_interval_bytes",new_retained}}.dump()<<'\n';
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
