#include "renderer/native/texture_data.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
// 2D fetch constant: tiled, pitch in 32-texel units, base address, extent.
std::array<uint32_t,6> Fetch(uint32_t format,uint32_t width,uint32_t height,uint32_t pitch,bool tiled,
                             uint32_t endian,uint32_t base,bool packed) {
    return {2u|(pitch/32u)<<22|(tiled?1u<<31:0u),format|endian<<6|base,(width-1)|(height-1)<<13,0,0,
            1u<<9|(packed?2048u:0u)};
}
}
int main() try {
    std::mt19937 random(1234);
    const uint32_t formats[]{6,26,38,18,19,20,49,59,2,10,24};
    uint64_t cases=0,whole=0;
    for(const uint32_t format:formats)for(int i=0;i<300;++i) {
        const auto block=TextureFormat(format);
        const uint32_t width=1+random()%(i<150?300:2048),height=1+random()%(i<150?300:2048);
        const uint32_t blocks_wide=(width+block.block_width-1)/block.block_width;
        // Pitch in texels: the block pitch aligned to 32 blocks, sometimes wider.
        const uint32_t pitch=((blocks_wide+31)/32*32+(random()%3==0?32:0))*block.block_width;
        if(pitch/32>511)continue;
        const bool tiled=random()%4!=0;
        const auto fetch=Fetch(format,width,height,pitch,tiled,random()%4,(0x100000u+(random()%4096)*4096u),random()%2);
        std::optional<std::array<int32_t,4>> rectangle;
        if(random()%2) {
            const int32_t left=random()%width,top=random()%height;
            rectangle=std::array<int32_t,4>{left,top,left+1+int32_t(random()%(width-left)),top+1+int32_t(random()%(height-top))};
        }
        const auto fast=TextureBaseFootprint(fetch,rectangle),reference=TextureBaseFootprint(fetch,rectangle,true);
        if(fast!=reference) {
            std::cerr<<"Footprint mismatch: format "<<format<<" "<<width<<"x"<<height<<" pitch "<<pitch
                     <<" tiled "<<tiled<<"\n";
            return 1;
        }
        ++cases;whole+=tiled&&block.bytes>=4&&width>=64*block.block_width&&height>=64*block.block_height;
    }
    // Every level of mip chains (packed tails included) over the same block
    // rectangle, as the resolve-memory footprint enumerates them.
    uint64_t levels=0;
    const auto merged=[](TextureFootprintRanges ranges) {
        std::sort(ranges.begin(),ranges.end());TextureFootprintRanges out;
        for(auto [first,last]:ranges) {
            if(!out.empty()&&first<=out.back().second)out.back().second=std::max(last,out.back().second);
            else out.emplace_back(first,last);
        }
        return out;
    };
    for(const uint32_t format:formats)for(int i=0;i<120;++i) {
        const auto block=TextureFormat(format);
        const uint32_t width=1+random()%1024,height=1+random()%1024;
        const uint32_t pitch=((width+block.block_width-1)/block.block_width+31)/32*32*block.block_width;
        if(pitch/32>511)continue;
        auto fetch=Fetch(format,width,height,pitch,random()%4!=0,random()%4,0x100000u+(random()%1024)*4096u,random()%2);
        fetch[4]=(random()%12)<<6;fetch[5]|=0x8000000u;
        const auto layout=DescribeTexture(fetch);
        const uint32_t right=1+random()%((width+block.block_width-1)/block.block_width);
        const uint32_t bottom=1+random()%((height+block.block_height-1)/block.block_height);
        const uint32_t left=random()%right,top=random()%bottom;
        for(const auto& level:layout.levels) {
            TextureFootprintRanges fast,reference;
            AppendTextureLevelFootprint(fast,layout,level,left,top,right,bottom,0x2000000u,~0ull);
            AppendTextureLevelFootprint(reference,layout,level,left,top,right,bottom,0x2000000u,~0ull,true);
            if(merged(fast)!=merged(reference)) {
                std::cerr<<"Level footprint mismatch: format "<<format<<" "<<width<<"x"<<height<<" mip "<<level.mip<<"\n";
                return 1;
            }
            ++levels;
        }
    }
    std::cout<<levels<<" mip levels match; ";
    std::cout<<"Texture footprint PASS: "<<cases<<" layouts ("<<whole<<" with whole tiles) match the block enumeration\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
