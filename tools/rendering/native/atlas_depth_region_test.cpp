#include "renderer/native/atlas_depth_region.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cfenv>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
NativeAtlasDepthRegionInput Input() {
    NativeAtlasDepthRegionInput x;
    x.source_width=x.source_pitch=x.destination_width=x.destination_pitch=1120;
    x.source_height=x.destination_height=688;x.source_base=1440;x.source_depth=true;
    x.source_surface_info=293602400;x.destination_format=22;
    x.destination_dimension=x.destination_layers=x.destination_level_count=1;
    x.destination_endian=2;x.destination_tiled=true;
    x.destination_block_width=x.destination_block_height=1;x.destination_block_bytes=4;
    x.flags=x.source_slot=4;x.source_rectangle=x.destination_rectangle={0,0,160,96};return x;
}
// Independent console80x16 depth half-tile oracle, not the push-based address
// interpreter below. Full packed word data includes every stencil byte and
// important UNORM24 quantization boundaries, with unrelated tiles nonzero.
uint32_t SourceWord(uint32_t x,uint32_t y) {
    const uint32_t tile=1440+(y/16)*14+x/80;
    const uint32_t column=(x%80+40)%80;
    return ((tile%2048)*1280+(y%16)*80+column)%(2048*1280);
}
uint32_t ShaderAddress(const std::array<uint32_t,16>& push,uint32_t x,uint32_t y) {
    x+=push[4];y+=push[5];
    const uint32_t tile=(y/16)*push[6]+x/80;
    const uint32_t column=x%80<40?x%80+40:x%80-40;
    return (push[12]+tile*5120+((y%16)*80+column)*4)%(2048*5120)/4;
}
struct Texel {float depth,alpha;};
Texel Decode(uint32_t word) {
    const auto d=word>>8;return {float(d+(d>>23))/16777216.f,float(word&255)/255.f};
}
uint8_t Stencil(Texel t) {return uint8_t(std::nearbyint(std::clamp(t.alpha,0.f,1.f)*255.f));}
bool Same(Texel x,Texel y) {
    return std::bit_cast<uint32_t>(x.depth)==std::bit_cast<uint32_t>(y.depth)&&
        std::bit_cast<uint32_t>(x.alpha)==std::bit_cast<uint32_t>(y.alpha);
}
template<class F>void Reject(F&& f) {
    bool refused=false;try{f();}catch(const std::invalid_argument&){refused=true;}
    Require(refused,"Malformed production dispatch accepted");
}
}
int main()try {
    Require(std::fegetround()==FE_TONEAREST,"CPU fixture requires original ties-to-even rounding mode");
    auto input=Input();Require(NativeAtlasDepthRegionAllowed(input),"Actual160x96 transfer refused");
    // Every scalar admission field and rectangle category is independently
    // mutated, reaching the actual production predicate rather than a digest.
    const auto refuse=[&](auto mutate) {auto bad=input;mutate(bad);
        Require(!NativeAtlasDepthRegionAllowed(bad),"Unsupported transfer family admitted");};
    refuse([](auto& x){x.source_depth=false;});refuse([](auto& x){x.source_width=1119;});
    refuse([](auto& x){x.source_height=689;});refuse([](auto& x){x.source_pitch=1152;});
    refuse([](auto& x){x.source_base=0;});refuse([](auto& x){x.source_format=1;});
    refuse([](auto& x){x.source_surface_info|=0x10000;});
    refuse([](auto& x){x.destination_width=1280;});refuse([](auto& x){x.destination_height=720;});
    refuse([](auto& x){x.destination_format=23;});refuse([](auto& x){x.destination_dimension=2;});
    refuse([](auto& x){x.destination_layers=2;});refuse([](auto& x){x.destination_endian=0;});
    refuse([](auto& x){x.destination_level_count=2;});refuse([](auto& x){x.destination_min_mip=1;});
    refuse([](auto& x){x.destination_max_mip=1;});refuse([](auto& x){x.destination_block_width=4;});
    refuse([](auto& x){x.destination_block_height=4;});refuse([](auto& x){x.destination_block_bytes=8;});
    refuse([](auto& x){x.destination_pitch=1152;});refuse([](auto& x){x.destination_tiled=false;});
    refuse([](auto& x){x.flags=0;});refuse([](auto& x){x.level=1;});
    refuse([](auto& x){x.source_slot=0;});refuse([](auto& x){x.exponent_bias=1;});
    refuse([](auto& x){x.destination_swap=true;});
    for(unsigned component=0;component<4;++component) {
        refuse([&](auto& x){x.source_rectangle[component]=component<2?-1:2000;});
        refuse([&](auto& x){x.destination_rectangle[component]=component<2?-1:2000;});
    }
    refuse([](auto& x){x.source_rectangle[2]=0;});
    refuse([](auto& x){x.source_rectangle[3]=0;});
    refuse([](auto& x){++x.destination_rectangle[2];});
    refuse([](auto& x){++x.destination_rectangle[3];});
    refuse([](auto& x){x.source_rectangle=x.destination_rectangle={0,0,161,96};});
    refuse([](auto& x){x.source_rectangle=x.destination_rectangle={0,0,160,97};});
    refuse([](auto& x){x.source_rectangle=x.destination_rectangle={0,0,544,352};});
    refuse([](auto& x){x.source_rectangle=x.destination_rectangle={0,0,1120,688};});
    auto offset=input;offset.source_rectangle={41,17,179,91};offset.destination_rectangle={0,0,138,74};
    Require(NativeAtlasDepthRegionAllowed(offset),"Exact bounded source/destination offset transfer refused");
    std::vector<uint32_t> words(2048*1280);
    constexpr uint32_t depths[]{0,1,0x3fffff,0x400000,0x400001,0x7fffff,0x800000,0x800001,0xfffffe,0xffffff};
    for(uint32_t i=0;i<words.size();++i)words[i]=(depths[i%std::size(depths)]<<8)|(i&255);
    std::vector<Texel> full(1120*688);
    for(uint32_t y=0;y<688;++y)for(uint32_t x=0;x<1120;++x)full[y*1120+x]=Decode(words[SourceWord(x,y)]);
    constexpr std::array<int32_t,4> regions[]{
        {0,0,96,96},{0,0,160,96},{0,0,160,64},
        {41,17,179,91},{1046,614,1120,688},{79,15,82,18},{1119,687,1120,688}};
    const Texel poison{std::bit_cast<float>(0x7fc00a71u),std::bit_cast<float>(0x7fc00237u)};
    for(const auto& rectangle:regions) {
        auto actual=input;actual.source_rectangle=actual.destination_rectangle=rectangle;
        Require(NativeAtlasDepthRegionAllowed(actual),"Bounded region refused");
        const auto push=NativeAtlasDepthRegionLoadParameters(1120,688,1120,1440,0,true,rectangle);
        Require(push[0]==uint32_t(rectangle[2]-rectangle[0])&&push[1]==uint32_t(rectangle[3]-rectangle[1])&&
            push[2]==uint32_t(rectangle[0])&&push[3]==uint32_t(rectangle[1])&&push[6]==14&&
            push[7]==2&&push[8]==3&&push[9]==0&&push[10]==22&&push[12]==1440*5120&&
            push[13]==2048*1280&&push[4]==0&&push[5]==0&&push[11]==0&&push[14]==0&&push[15]==0,
            "Production push ABI or original full coordinates changed");
        std::vector<Texel> cropped(full.size(),poison),restored(full.size(),poison);
        // Interpret actual changed production push words as the unchanged shader
        // does, including rounded dispatch and guard invocations.
        for(uint32_t gy=0;gy<(push[1]+7)/8*8;++gy)for(uint32_t gx=0;gx<(push[0]+7)/8*8;++gx) {
            if(gx>=push[0]||gy>=push[1])continue;
            const auto x=gx+push[2],y=gy+push[3];
            cropped[y*1120+x]=Decode(words[ShaderAddress(push,x,y)]);
        }
        for(uint32_t y=0;y<688;++y)for(uint32_t x=0;x<1120;++x) {
            const bool inside=x>=uint32_t(rectangle[0])&&x<uint32_t(rectangle[2])&&
                y>=uint32_t(rectangle[1])&&y<uint32_t(rectangle[3]);
            const auto i=y*1120+x;
            Require(Same(cropped[i],inside?full[i]:poison),"Source crop changed a read pixel or wrote outside region");
            if(inside) {
                // Both existing stencil implementations are the same original
                // byte inside the scissor: export once or eight masked draws.
                uint8_t legacy=0,expected=uint8_t(words[SourceWord(x,y)]);
                for(unsigned bit=0;bit<8;++bit)if(Stencil(cropped[i])&(1u<<bit))legacy|=1u<<bit;
                Require(legacy==expected&&Stencil(cropped[i])==expected,"Stencil crop changed a packed byte");
                restored[i]=cropped[i];
            }
            Require(Same(restored[i],inside?full[i]:poison),"Region restoration certified or overwrote an outside pixel");
        }
        // The unchanged destination resolve and stencil-store offset both read
        // this initialized absolute source coordinate for every destination pixel.
        for(int32_t y=0;y<rectangle[3]-rectangle[1];++y)
            for(int32_t x=0;x<rectangle[2]-rectangle[0];++x) {
                const auto at=size_t(y+rectangle[1])*1120+x+rectangle[0];
                Require(Same(restored[at],full[at])&&Stencil(restored[at])==Stencil(full[at]),
                    "Offset resolve read outside initialized source coordinates");
            }
        // Independent lifetime model: cropped scratch is usable only for this
        // transfer. A generic full read must repair the deliberately old token,
        // even after prior deferred transfer owners have completed and released.
        const uint64_t old_token=31,current_token=37;
        const auto retained=NativeAtlasPartialRestoreVersion(old_token,current_token);
        Require(retained==old_token&&retained!=current_token,"Partial restoration falsely made whole host image current");
        if(retained!=current_token)restored=full;
        for(size_t i=0;i<full.size();++i)Require(Same(restored[i],full[i]),"Generic full restoration left stale pixels");
    }
    Reject([]{NativeAtlasDepthRegionLoadParameters(1120,688,1120,1440,1,true,{0,0,96,96});});
    Reject([]{NativeAtlasDepthRegionLoadParameters(1120,688,1120,0,0,true,{0,0,96,96});});
    Reject([]{NativeAtlasDepthRegionLoadParameters(1120,688,1120,1440,0,false,{0,0,96,96});});
    Reject([]{NativeAtlasDepthRegionLoadParameters(1120,688,1120,1440,0,true,{-1,0,96,96});});
    Reject([]{NativeAtlasDepthRegionLoadParameters(1120,688,1120,1440,0,true,{0,0,0,96});});
    Reject([]{NativeAtlasDepthRegionLoadParameters(1120,688,1120,1440,0,true,{0,0,544,352});});
    Require(NativeAtlasPartialRestoreVersion(0,UINT64_MAX)==0,"Old-token preservation changed at boundary");
    std::cout<<"PASS "<<checks<<" atlas source-region guard/dispatch/stencil/version CPU model checks; no GPU proof\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
