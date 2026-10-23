#include "renderer/native/retained_opaque_authority.h"
#include "renderer/native/readonly_depth_attachment.h"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <type_traits>
static_assert(!std::is_copy_constructible_v<gow3::renderer::NativeRetainedOpaqueAuthority>);
static_assert(!std::is_move_constructible_v<gow3::renderer::NativeRetainedOpaqueAuthority>);
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool condition){++checks;if(!condition)throw std::runtime_error("Retained opaque fixture failed");}
template<class F>void Reject(F f){bool threw=false;try{f();}catch(const std::logic_error&){threw=true;}Require(threw);}
uint32_t Next(uint32_t& state){state=state*1664525u+1013904223u;return state;}
// Independent physical oracle: one 5120-byte tile =16 rows of80words;
// depth swaps each row's40-word halves, while color keeps columns unchanged.
size_t Word(bool depth,uint32_t x,uint32_t y) {
    uint32_t col=x%80;if(depth)col=(col+40)%80;
    return ((depth?0:720)+(y/16)*16+x/80)*1280+(y%16)*80+col;
}
// Inverse of the exact D32 half embedding, deliberately separate from the
// original packed-depth quantizer. Only valid embedded codes are admitted.
uint32_t Code(float half) {
    const auto bits=std::bit_cast<uint32_t>(half);
    if(bits<0x38000000u)return uint32_t(half*0x1p35f);
    return (((bits>>23)-111u)<<20)|((bits&0x7fffffu)>>3);
}
void Policy() {
    NativeRetainedOpaqueAuthority a;Require(!a.present()&&!a.dirty());
    a.Admit(1,kNativeRetainedOpaqueLayout,100,200);Require(a.Reusable(kNativeRetainedOpaqueLayout,100,200));
    auto wrong=kNativeRetainedOpaqueLayout;wrong[3]=719;Require(!a.Reusable(wrong,100,200));
    uint64_t c=100,d=200;
    for(unsigned i=0;i<91;++i) {
        const auto t=a.ticket();a.AcceptedDraw(t,c,d,c+2,d+2);c+=2;d+=2;
        Require(a.dirty()&&a.Reusable(kNativeRetainedOpaqueLayout,c,d));
    }
    a.BeforeMaterialize(c,d);const auto t=a.ticket();a.Materialized(t,false);
    Require(a.dirty()&&a.present());a.Materialized(t,true);Require(!a.present()&&!a.dirty());
    a.Admit(2,kNativeRetainedOpaqueLayout,c+3,d+3);Require(a.present());
    Reject([&]{a.Admit(1,kNativeRetainedOpaqueLayout,c+3,d+3);});Require(a.failed());
    NativeRetainedOpaqueAuthority foreign;foreign.Admit(1,kNativeRetainedOpaqueLayout,1,2);
    auto f=foreign.ticket();foreign.AcceptedDraw(f,1,2,3,4);
    Reject([&]{foreign.BeforeMaterialize(5,4);});Require(foreign.failed()&&foreign.dirty());
    Reject([&]{foreign.Materialized(foreign.ticket(),true);});
    NativeRetainedOpaqueAuthority stale;stale.Admit(1,kNativeRetainedOpaqueLayout,1,2);
    auto old=stale.ticket();stale.AcceptedDraw(old,1,2,3,4);
    Reject([&]{stale.AcceptedDraw(old,3,4,5,6);});Require(stale.failed()&&stale.dirty());
}
void Numeric() {
    uint32_t random=0x865431;
    for(uint32_t e=0;e<16;++e)for(uint32_t m:std::array<uint32_t,9>{0,1,2,3,7,0x7ffff,0xffffd,0xffffe,0xfffff}) {
        uint32_t code=(e<<20)|m;Require(Code(NativeReadonlyDepthD32(code<<8))==code);
    }
    for(unsigned i=0;i<10000;++i) {const auto code=Next(random)&0xffffff;Require(Code(NativeReadonlyDepthD32(code<<8))==code);}
}
void PackedOracle() {
    constexpr uint32_t width=1280,height=720,pixels=width*height;
    std::vector<uint32_t> original(2048*1280),canonical,color(pixels),depth(pixels);
    uint32_t random=0x712fa;for(auto& w:original)w=Next(random);canonical=original;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const auto i=y*width+x;color[i]=original[Word(false,x,y)];depth[i]=original[Word(true,x,y)]>>8;
    }
    // Independent serial packed reference vs retained color/depth arrays.
    // Failed depth must preserve color and stencil; repeated draws overwrite
    // previous accepted pixels, including exact equal-code GE comparisons.
    for(unsigned draw=0;draw<91;++draw)for(unsigned sample=0;sample<4096;++sample) {
        uint32_t x=Next(random)%width,y=Next(random)%height,i=y*width+x;
        const auto da=Word(true,x,y),ca=Word(false,x,y);const auto c=Next(random);
        const auto incoming=sample%4==0&&(original[da]>>8)<=0xf00000u?(original[da]>>8):std::min(Next(random)&0xffffffu,0xf00000u);
        if(incoming>=(original[da]>>8)){original[da]=(incoming<<8)|(original[da]&255);original[ca]=c;}
        if(incoming>=depth[i]){depth[i]=Code(NativeReadonlyDepthD32(incoming<<8));color[i]=c;}
    }
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const auto i=y*width+x;const auto da=Word(true,x,y);canonical[Word(false,x,y)]=color[i];canonical[da]=(depth[i]<<8)|(canonical[da]&255);
    }
    Require(canonical==original);
    // Every unused tile and all stencil bytes retain their original identity;
    // equality above also validates the two disjoint full surface footprints.
}
}
int main(){try{Policy();Numeric();PackedOracle();std::cout<<"PASS retained opaque authority: "<<checks<<" checks, exact91-draw packed-word oracle\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
