#include "renderer/native/packed_zero_depth_clear.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

using namespace gow3::renderer;
namespace {
size_t checks=0;
void Check(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
uint32_t Address(uint32_t x,uint32_t y,uint32_t pitch,uint32_t base) {
    // Independent scalar 80x16 D24FS8 address, swapped 40-pixel half-tiles.
    const uint32_t tile=(base+(y/16)*((pitch+79)/80)+x/80)%2048;
    return tile*1280+(y%16)*80+(x%80<40?x%80+40:x%80-40);
}
}
int main()try {
    const PackedZeroDepthClearInput input{true,true,0x30,1280,720,1280,0,1,1280,
        {0,0,1280,720},0.0,0};
    Check(PackedZeroDepthClearAllowed(input),"Observed complete zero clear rejected");
    for(uint32_t flags=0;flags<256;++flags) {
        auto v=input;v.flags=flags;Check(PackedZeroDepthClearAllowed(v)==(flags==0x30),"Flags gate changed");
    }
    for(unsigned bit=0;bit<32;++bit) {
        auto v=input;v.stencil=1u<<bit;Check(!PackedZeroDepthClearAllowed(v),"Nonzero stencil accepted");
    }
    for(double depth:{-0.0,1.0,-1.0,0.5,1e-300,-1e-300,
        std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto v=input;v.depth=depth;Check(!PackedZeroDepthClearAllowed(v),"Non-positive-zero depth accepted");
    }
    for(uint32_t info:{1u<<16,2u<<16,3u<<16}) {
        auto v=input;v.surface_info|=info;Check(!PackedZeroDepthClearAllowed(v),"Multisampled depth accepted");
    }
    for(unsigned field=0;field<4;++field)for(int delta:{-1,1}) {
        auto v=input;v.rectangle[field]+=delta;Check(!PackedZeroDepthClearAllowed(v),"Partial/invalid rectangle accepted");
    }
    for(uint32_t value:{0u,2u,UINT32_MAX}) {
        auto v=input;v.format=value;Check(!PackedZeroDepthClearAllowed(v),"Unknown depth encoding accepted");
    }
    {auto v=input;v.native_fsi=false;Check(!PackedZeroDepthClearAllowed(v),"Nonpacked scene selected");}
    {auto v=input;v.depth_surface=false;Check(!PackedZeroDepthClearAllowed(v),"Color surface selected");}
    for(uint32_t value:{0u,8193u,UINT32_MAX}) {
        auto v=input;v.width=value;Check(!PackedZeroDepthClearAllowed(v),"Invalid width selected");
        v=input;v.height=value;Check(!PackedZeroDepthClearAllowed(v),"Invalid height selected");
    }
    for(uint32_t value:{0u,16384u,UINT32_MAX}) {
        auto v=input;v.pitch=value;Check(!PackedZeroDepthClearAllowed(v),"Invalid pitch selected");
    }
    for(uint32_t value:{2048u,UINT32_MAX}) {
        auto v=input;v.base=value;Check(!PackedZeroDepthClearAllowed(v),"Invalid base selected");
    }
    // Independent address-set proof over boundary widths, pitches and wrapped
    // bases. Every accepted layout is finite and has no overlapping pixel words.
    for(uint32_t width:{1u,39u,40u,41u,79u,80u,81u,159u,160u,161u})
      for(uint32_t height:{1u,15u,16u,17u,31u,32u})
       for(uint32_t pitch:{1u,40u,79u,80u,81u,159u,160u,161u,320u})
        for(uint32_t base:{0u,1u,2047u}) {
            const bool accepted=PackedZeroDepthLayoutAllowed(width,height,pitch,base,1,true);
            if(!accepted)continue;
            std::set<uint32_t> words;
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
                words.insert(Address(x,y,pitch,base));
            Check(words.size()==uint64_t(width)*height&&*words.rbegin()<2048*1280,
                  "Accepted layout overlaps or exceeds EDRAM");
        }
    Check(PackedZeroDepthLayoutAllowed(320,8192,320,2047,1,true),"Exact 2048-tile bound rejected");
    Check(!PackedZeroDepthLayoutAllowed(80,8192,400,2047,1,true),"Repeated-tile footprint accepted");
    Check(!PackedZeroDepthLayoutAllowed(8192,8192,8192,0,1,true),"Large repeated footprint accepted");
    std::cout<<"PASS: "<<checks<<" exact zero-state, format, full rectangle, bounds and independent wrapped-depth address checks\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
