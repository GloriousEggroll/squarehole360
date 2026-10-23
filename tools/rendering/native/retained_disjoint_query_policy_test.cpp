// Pure admission/page-authority model only. Actual GPU copies, late depth,
// query sample/identity/frontier and full-memory parity are separate root tests.
#include "retained_disjoint_query.h"
#include "retained_fp24_depth_authority.h"
#include "retained_opaque_authority.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
struct Input {
    NativeFsiConstants fsi{};
    uint32_t width=640,height=360,info=640,mask=0;
    bool pixel=false,active=true,audited=true,cpu=true,no_textures=true;
    std::string_view vertex=kReadonlyQueryVertexSource;
    std::array<uint32_t,6> raster=kNativeRetainedDisjointQueryRaster;
    float min=1,max=0;
    Input(){fsi.pitch=640;fsi.depth_base=1440;fsi.depth_format=1;fsi.depth_control=0x700762;}
    bool Allowed()const{return NativeRetainedDisjointQueryAllowed(fsi,width,height,info,mask,pixel,
        active,audited,vertex,raster,min,max,cpu,no_textures);}
};
void Admission(){
    Input a;Require(a.Allowed(),"Exact reversed query refused");
    a.min=0;a.max=1;Require(a.Allowed(),"Exact forward query refused");
    a.fsi.front_slope_bias=-0.f;a.fsi.front_constant_bias=-0.f;
    a.fsi.back_slope_bias=-0.f;a.fsi.back_constant_bias=-0.f;
    Require(a.Allowed(),"Exact signed-zero bias refused");
    auto refuse=[](auto mutation){Input x;mutation(x);Require(!x.Allowed(),"Unaudited one-field mutation admitted");};
    refuse([](Input& x){x.width=641;});refuse([](Input& x){x.height=361;});
    refuse([](Input& x){x.width=0;});refuse([](Input& x){x.height=0;});
    refuse([](Input& x){x.fsi.pitch=639;});refuse([](Input& x){x.fsi.pitch=1280;});
    refuse([](Input& x){x.fsi.depth_base=1439;});refuse([](Input& x){x.fsi.depth_base=0;});
    refuse([](Input& x){x.fsi.depth_base=2047;});refuse([](Input& x){x.fsi.depth_format=0;});
    refuse([](Input& x){x.fsi.depth_control=0x700766;});
    refuse([](Input& x){x.info|=0x10000;});refuse([](Input& x){x.mask=1;});
    refuse([](Input& x){x.pixel=true;});refuse([](Input& x){x.active=false;});
    refuse([](Input& x){x.audited=false;});refuse([](Input& x){x.cpu=false;});
    refuse([](Input& x){x.no_textures=false;});refuse([](Input& x){x.vertex="foreign";});
    for(unsigned i=0;i<6;++i)refuse([i](Input& x){++x.raster[i];});
    for(unsigned i=0;i<4;++i)refuse([i](Input& x){x.fsi.color[i].mask=1;});
    refuse([](Input& x){x.min=.25f;});refuse([](Input& x){x.max=.75f;});
    refuse([](Input& x){x.min=std::numeric_limits<float>::quiet_NaN();});
    refuse([](Input& x){x.max=std::numeric_limits<float>::infinity();});
    for(unsigned i=0;i<4;++i){
        auto bias=[i](Input& x)->float&{switch(i){case 0:return x.fsi.front_slope_bias;case 1:return x.fsi.front_constant_bias;
            case 2:return x.fsi.back_slope_bias;default:return x.fsi.back_constant_bias;}};
        refuse([&](Input& x){bias(x)=.125f;});
        refuse([&](Input& x){bias(x)=std::numeric_limits<float>::quiet_NaN();});
    }
}
// Independent complete tile bounds: fixed surface has8x23 whole tiles. Pages
// are4KiB; a tile is5KiB. Primary ends exactly at page1800, secondary starts there.
void FootprintsAndAuthority(){
    constexpr unsigned primary_words=1440*1280,whole_words=10*1024*1024/4;
    constexpr unsigned secondary_first=1440*5120/4096,secondary_end=(1624*5120+4095)/4096;
    std::bitset<2048> tiles;std::bitset<2560> pages;
    for(unsigned row=0;row<(360+15)/16;++row)for(unsigned column=0;column<(640+79)/80;++column){
        const unsigned tile=1440+row*8+column;Require(tile>=1440&&tile<1624,"Secondary tile escaped exact interval");
        Require(!tiles.test(tile),"Secondary tile alias");tiles.set(tile);
        for(unsigned page=tile*5120/4096;page<(tile*5120+5120+4095)/4096;++page)pages.set(page);
    }
    Require(tiles.count()==184&&pages.count()==230,"Full secondary tile/page envelope wrong");
    Require(secondary_first==1800&&secondary_end==2030,"Secondary page envelope wrong");
    for(unsigned page=0;page<1800;++page)Require(!pages.test(page),"Secondary aliases retained primary physical page");
    std::array<uint64_t,2560> versions;versions.fill(1);
    auto version=[&](unsigned first,unsigned end){return *std::max_element(versions.begin()+first,versions.begin()+end);};
    std::fill(versions.begin(),versions.begin()+900,3);std::fill(versions.begin()+900,versions.begin()+1800,2);
    NativeRetainedFp24DepthAuthority depth;depth.Admit(1,kNativeRetainedFp24DepthLayout,1);
    depth.AcceptedDraw(depth.ticket(),1,3,true,false);
    NativeRetainedOpaqueAuthority opaque;opaque.Admit(1,kNativeRetainedOpaqueLayout,1,1);
    opaque.AcceptedDraw(opaque.ticket(),1,1,2,3);
    const auto dt=depth.ticket();const auto ot=opaque.ticket();
    // Canonical primary deliberately remains stale; fencing/readonly queries
    // must not pretend it contains the retained writes. All stencil bytes stay.
    std::vector<uint32_t> canonical(whole_words),expected(whole_words);
    for(unsigned i=0;i<whole_words;++i)canonical[i]=expected[i]=(i*1664525u+1013904223u);
    auto authoritative=canonical;
    for(unsigned i=0;i<900*1024;++i)authoritative[i]=(canonical[i]&255u)|0xec000000u;
    for(unsigned i=900*1024;i<primary_words;++i)authoritative[i]^=0x98765400u;
    for(uint64_t serial=4;serial<132;++serial){
        for(unsigned page=secondary_first;page<secondary_end;++page)versions[page]=serial;
        Require(depth.Reusable(kNativeRetainedFp24DepthLayout,version(0,900)),"Disjoint readonly invalidated depth authority");
        Require(opaque.Reusable(kNativeRetainedOpaqueLayout,version(900,1800),version(0,900)),"Disjoint readonly invalidated pair authority");
        Require(depth.ticket().generation==dt.generation&&depth.ticket().depth==dt.depth&&depth.dirty(),"Depth token/dirty state promoted by disjoint query");
        Require(opaque.ticket().generation==ot.generation&&opaque.ticket().color==ot.color&&opaque.ticket().depth==ot.depth&&opaque.dirty(),"Pair token/dirty state promoted by disjoint query");
    }
    Require(canonical==expected&&canonical!=authoritative,"Fence/query exposed or silently replaced stale canonical primary");
    // A true overlapping reinterpretation is a consumer, not a disjoint case.
    auto foreign=versions;foreign[899]=999;
    const auto foreign_depth=*std::max_element(foreign.begin(),foreign.begin()+900);
    Require(!depth.Reusable(kNativeRetainedFp24DepthLayout,foreign_depth),"Foreign depth overlap ignored");
    Require(!opaque.Reusable(kNativeRetainedOpaqueLayout,version(900,1800),foreign_depth),"Foreign pair overlap ignored");
    std::copy(authoritative.begin(),authoritative.begin()+primary_words,canonical.begin());
    std::copy(authoritative.begin(),authoritative.begin()+primary_words,expected.begin());
    Require(canonical==expected,"Strong primary materialization changed disjoint/untouched words");
    for(unsigned i=0;i<900*1024;++i)Require((canonical[i]&255u)==(authoritative[i]&255u),"Strong depth export changed stencil");
    depth.Materialized(dt,true);opaque.Materialized(ot,true);
    Require(!depth.present()&&!opaque.present()&&!depth.dirty()&&!opaque.dirty(),"Strong materialization kept stale authority");
}
}
int main(){try{Admission();FootprintsAndAuthority();std::cout<<"PASS retained disjoint query policy "<<checks<<" checks; pure CPU metadata/full-word model only\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
