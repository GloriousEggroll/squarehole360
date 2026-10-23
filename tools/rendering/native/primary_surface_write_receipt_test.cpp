#include "renderer/native/primary_surface_write_receipt.h"
#include "renderer/native/page_version_table.h"
#include "renderer/native/gpu_alias_state.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
using Range=std::pair<uint32_t,uint32_t>;
using Layout=std::array<uint32_t,6>;
constexpr Layout color_layout{1280,720,1280,720,3,0},depth_layout{1280,720,1280,0,1,1};
const std::vector<Range> color{{3686400,7372800}},depth{{0,3686400}};
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
// Independent tiled-address enumeration: every actual32-bit pixel contributes
// its complete byte range, then the union is formed; no guessed page span.
std::vector<Range> Enumerate(const Layout& layout) {
    std::vector<Range> out;out.reserve(1280*720);
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x) {
        const uint32_t tile=layout[3]+(y/16)*16+x/80;
        const uint32_t column=layout[5]?(x%80+40)%80:x%80;
        const uint32_t first=(tile*1280+(y%16)*80+column)*4;
        out.emplace_back(first,first+4);
    }
    std::sort(out.begin(),out.end());size_t count=0;
    for(auto r:out) {
        if(count&&r.first<=out[count-1].second)out[count-1].second=std::max(out[count-1].second,r.second);
        else out[count++]=r;
    }
    out.resize(count);return out;
}
void FootprintProof() {
    Check(Enumerate(color_layout)==color,"Color tiled union differs");
    Check(Enumerate(depth_layout)==depth,"Depth swapped-half-tile union differs");
    Check(NativePrimarySurfaceWritePages(color_layout,color,2560)==std::optional(Range{900,1800}),"Color footprint rejected");
    Check(NativePrimarySurfaceWritePages(depth_layout,depth,2560)==std::optional(Range{0,900}),"Depth footprint rejected");
    Check(!NativePrimarySurfaceWritePages(color_layout,depth,2560),"Wrong surface footprint admitted");
    Check(!NativePrimarySurfaceWritePages(color_layout,{},2560),"Empty footprint admitted");
    Check(!NativePrimarySurfaceWritePages(color_layout,color,2559),"Short backing admitted");
    for(unsigned i=0;i<6;++i) {
        auto wrong=color_layout;++wrong[i];Check(!NativePrimarySurfaceWritePages(wrong,color,2560),"Wrong color layout admitted");
        wrong=depth_layout;++wrong[i];Check(!NativePrimarySurfaceWritePages(wrong,depth,2560),"Wrong depth layout admitted");
    }
    for(auto wrong:std::vector<std::vector<Range>>{{{3686404,7372800}},{{3686400,7372796}},{{3686396,7372800}},{{3686400,7372804}},{{3686400,3686400}},{{7372800,3686400}},{{3686400,4000000},{4000000,7372800}},{{3686400,4000000},{4000004,7372800}},{{3686400,UINT32_MAX}}})
        Check(!NativePrimarySurfaceWritePages(color_layout,wrong,2560),"Partial/fragmented/invalid footprint admitted");
}
struct Model {
    NativePageVersionTable pages;uint64_t serial=0,epoch=0;
    std::map<int,NativeGpuAliasFootprint> footprints;
    explicit Model(bool blocks,bool tracked){pages.Reset(2560,blocks,tracked);}
    void Advance(){if(AdvanceNativeGpuAliasStateSerial(serial,footprints))++epoch;}
    // Mirrors ResolveMemory's unaltered assignment ordering, using the REAL
    // table and serial implementation. Scope is metadata math, not Scene/GPU.
    void Write(const Layout& layout,const std::vector<Range>& ranges,NativePrimarySurfaceWriteStamp& receipt) {
        receipt={};Advance();
        for(auto [first,last]:ranges)pages.Assign(first/4096,(last-1)/4096+1,serial);
        if(auto span=NativePrimarySurfaceWritePages(layout,ranges,pages.size()))
            receipt={this,serial,epoch,span->first,span->second};
    }
    auto Pair(const NativePrimarySurfaceWriteStamp& c,const NativePrimarySurfaceWriteStamp& d)const {
        return NativePrimarySurfaceWriteVersions(c,d,this,serial,epoch);
    }
};
void AssignmentAndFreshness(bool blocks,bool tracked) {
    Model m(blocks,tracked);NativePrimarySurfaceWriteStamp c,d;
    // All prior pages may hold a numerically newer value. Assignment, not max
    // assignment, must still make both original scans equal actual receipts.
    m.pages.Assign(0,2560,UINT64_MAX);
    for(unsigned round=0;round<24;++round) {
        m.Write(color_layout,color,c);m.Write(depth_layout,depth,d);
        auto versions=m.Pair(c,d);Check(bool(versions),"Exact adjacent writes refused");
        Check((*versions)[0]==m.pages.Max(900,1800)&&(*versions)[1]==m.pages.Max(0,900),"Receipt differs from real table Max");
        Check(m.pages.Max(1800,2560)==UINT64_MAX,"Untouched pages changed");
        auto copy=c;copy.owner=&c;Check(!m.Pair(copy,d),"Foreign color owner admitted");
        copy=c;++copy.first_page;Check(!m.Pair(copy,d),"Partial color page span admitted");
        copy=c;copy.first_page=0;copy.end_page=900;Check(!m.Pair(copy,d),"Aliased color pages admitted");
        copy=c;++copy.epoch;Check(!m.Pair(copy,d),"Wrong receipt epoch admitted");
        copy=c;copy.serial=0;Check(!m.Pair(copy,d),"Zero receipt admitted");
        copy=d;--copy.end_page;Check(!m.Pair(c,copy),"Partial depth pages admitted");
        copy=d;copy.owner=&d;Check(!m.Pair(c,copy),"Foreign depth owner admitted");
        Check(!NativePrimarySurfaceWriteVersions(c,d,nullptr,m.serial,m.epoch),"Missing current owner admitted");
        Check(!NativePrimarySurfaceWriteVersions(c,d,&m,m.serial,m.epoch+1),"Wrong current epoch admitted");
        m.Advance();Check(!m.Pair(c,d),"Later write serial retained receipt");
    }
    // Overlapping and disjoint intervening writes are both conservative misses.
    m.Write(color_layout,color,c);m.Advance();m.pages.Assign(1000,1001,m.serial);m.Write(depth_layout,depth,d);
    Check(!m.Pair(c,d),"Intervening overlapping write admitted");Check(m.pages.Max(900,1800)>c.serial,"Overlap did not alter oracle");
    m.Write(color_layout,color,c);m.Advance();m.pages.Assign(2200,2201,m.serial);m.Write(depth_layout,depth,d);
    Check(!m.Pair(c,d),"Intervening disjoint write admitted");
    // Wrong cached footprint still performs the original assignment, but mints
    // no exact primary receipt. Failed/partial writes publish no receipt either.
    m.Write(color_layout,{{3686400,7372796}},c);Check(!c.owner,"Partial range minted receipt");
    c={&m,42,0,900,1800};bool threw=false;
    try{m.Write(color_layout,{{3686400,7372800},{0,UINT32_MAX}},c);}catch(const std::out_of_range&){threw=true;}
    Check(threw&&!c.owner,"Failed assignment retained a receipt");
    m.serial=UINT64_MAX-1;m.Write(color_layout,color,c);m.Write(depth_layout,depth,d);
    Check(c.serial==UINT64_MAX&&d.serial==0&&!m.Pair(c,d),"Across-wrap pair admitted");
    Check(m.pages.Max(900,1800)==UINT64_MAX&&m.pages.Max(0,900)==0,"Wrap oracle differs");
    m.serial=UINT64_MAX;m.Write(color_layout,color,c);m.Write(depth_layout,depth,d);
    Check(c.serial==0&&d.serial==1&&!m.Pair(c,d),"Zero-color wrapped pair admitted");
    m.Write(color_layout,color,c);m.Write(depth_layout,depth,d);Check(bool(m.Pair(c,d)),"Fresh post-wrap pair refused");
    auto stale=c;--stale.epoch;Check(!m.Pair(stale,d),"Old epoch reused after wrap");
    Check((*m.Pair(c,d))[0]==m.pages.Max(900,1800)&&(*m.Pair(c,d))[1]==m.pages.Max(0,900),"Post-wrap receipt differs from Max");
}
}
int main(){try{FootprintProof();for(bool blocks:{false,true})for(bool tracked:{false,true})AssignmentAndFreshness(blocks,tracked);
    std::cout<<"PASS "<<checks<<" primary write receipt metadata checks; no GPU or FPS claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
