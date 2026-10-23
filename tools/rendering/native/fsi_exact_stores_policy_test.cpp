#include "renderer/native/fsi_exact_stores_policy.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

using namespace gow3::renderer;
namespace {
uint64_t MaskOracle(uint32_t format,uint32_t components) {
    const std::array<uint32_t,4> widths=format==0?std::array<uint32_t,4>{8,8,8,8}:
        (format==2||format==3)?std::array<uint32_t,4>{10,10,10,2}:
        (format==4||format==6)?std::array<uint32_t,4>{16,16,0,0}:
        (format==5||format==7)?std::array<uint32_t,4>{16,16,16,16}:
        format==14?std::array<uint32_t,4>{32,0,0,0}:std::array<uint32_t,4>{32,32,0,0};
    uint64_t result=0;unsigned offset=0;
    for(unsigned c=0;c<4;++c){if(widths[c]&&(components&(1u<<c)))result|=((uint64_t(1)<<widths[c])-1)<<offset;offset+=widths[c];}
    return result;
}
uint32_t Word(uint32_t x,uint32_t y,uint32_t pitch,uint32_t base,bool depth,bool wide) {
    const uint32_t columns=(pitch+79)/80,tile=(y/16)*columns*(wide?2:1)+x/(wide?40:80);
    const uint32_t column=depth?(x%80<40?x%80+40:x%80-40):x%(wide?40:80);
    return ((base+tile)*1280+(y%16)*80+column*(wide?2:1))%(2048*1280);
}
}
int main() try {
    size_t checks=0;
    const auto check=[&](bool condition){++checks;if(!condition)throw std::runtime_error("Exact native store policy/oracle mismatch");};
    // Factorized exhaustive packed-depth identity: every24-bit code and all
    // low8 stencil values. No floating point decode/repack is used anywhere.
    for(uint32_t code=0;code<(1u<<24);++code) {
        const uint32_t previous=(code<<8)|(code&255u);
        const uint32_t original=((previous>>8)<<8)|(previous&255u);
        check(original==previous);
    }
    for(uint32_t stencil=0;stencil<256;++stencil)for(uint32_t code:{0u,1u,0x7fffffu,0x800000u,0xffffffu}) {
        const auto previous=(code<<8)|stencil;check((((previous>>8)<<8)|(previous&255u))==previous);
    }
    for(uint32_t bits=0;bits<8;++bits)check(NativeFsiExactDepthIdentityControl(0x700760u|bits)==(bits==2));
    // Arbitrary packed source outputs include every possible numeric packing
    // result. The untouched production packColor supplies these words; this
    // integer oracle does not claim to reimplement its floating point math.
    uint32_t random=0x97301b2f;
    const auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(uint32_t format:{0u,2u,3u,4u,5u,6u,7u,14u,15u})for(uint32_t components=0;components<16;++components) {
        const auto mask=MaskOracle(format,components);check(mask==NativeFsiExactStoresColorMask(format,components));
        const bool wide=format==5||format==7||format==15;
        const bool full=uint32_t(mask)==0xffffffffu&&(!wide||uint32_t(mask>>32)==0xffffffffu);
        for(unsigned blend=0;blend<2;++blend) {
            check(NativeFsiExactColorOverwrite(format,components,blend)==(blend==0&&full));
            for(unsigned trial=0;trial<1024;++trial) {
                const uint64_t before=uint64_t(next())|(uint64_t(next())<<32);
                const uint64_t packed_source=uint64_t(next())|(uint64_t(next())<<32);
                const uint64_t blended=uint64_t(next())|(uint64_t(next())<<32);
                const auto original=(before&~mask)|((blend?blended:packed_source)&mask);
                auto candidate=original;
                if(NativeFsiExactColorOverwrite(format,components,blend))candidate=wide?packed_source:(before&0xffffffff00000000ull)|uint32_t(packed_source);
                check(candidate==original);
            }
        }
    }
    for(uint32_t format:{1u,8u,9u,10u,11u,12u,13u,16u,0xffffffffu})check(!NativeFsiExactColorOverwrite(format,15,0));
    NativeFsiConstants state;state.pitch=1280;state.depth_base=0;state.depth_format=1;state.depth_control=0x700762;
    state.color[0]={720,3,15,0x1000101};check(NativeFsiExactDepthStoreNoAliases(state,1280,720));
    for(uint32_t base:{0u,1u,719u}){auto changed=state;changed.color[0].base=base;check(!NativeFsiExactDepthStoreNoAliases(changed,1280,720));}
    auto changed=state;changed.color[0].format=7;check(!NativeFsiExactDepthStoreNoAliases(changed,1280,720));
    changed=state;changed.color[0].mask=0;changed.depth_base=1328;check(NativeFsiExactDepthStoreNoAliases(changed,1280,720));
    changed.depth_base=1329;check(!NativeFsiExactDepthStoreNoAliases(changed,1280,720));
    for(uint32_t bit:{1u,4u}){changed=state;changed.depth_control|=bit;check(!NativeFsiExactDepthStoreNoAliases(changed,1280,720));}
    changed=state;changed.pitch=1279;check(!NativeFsiExactDepthStoreNoAliases(changed,1280,720));
    changed=state;changed.pitch=16384;check(!NativeFsiExactDepthStoreNoAliases(changed,1280,720));
    for(uint32_t extent:{0u,8193u})check(!NativeFsiExactDepthStoreNoAliases(state,extent,720));
    changed=state;changed.color[1]={0,0,1,0};check(!NativeFsiExactDepthStoreNoAliases(changed,1280,720));
    changed.color[1].mask=0;check(NativeFsiExactDepthStoreNoAliases(changed,1280,720));
    for(uint32_t width:{1u,40u,41u,80u,81u,160u})for(uint32_t height:{1u,16u,17u,32u})for(uint32_t format:{0u,3u,7u,15u}) {
        NativeFsiConstants small;small.pitch=width;small.depth_base=0;small.depth_control=0x700762;small.depth_format=1;
        small.color[0]={128,format,15,0};check(NativeFsiExactDepthStoreNoAliases(small,width,height));
        std::unordered_set<uint32_t> addresses;
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)check(addresses.insert(Word(x,y,width,0,true,false)).second);
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
            const bool wide=NativeFsiExactStoresWideColor(format);const auto at=Word(x,y,width,128,false,wide);
            check(!addresses.contains(at));if(wide)check(!addresses.contains(at+1));
        }
    }
    std::cout<<"PASS: "<<checks<<" exact packed identity/mask/alias policy checks; all24-bit depth codes, no GPU or floating numeric packing claim\n";
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
