// Source-only authored fixture: floating decode uses exact powers-of-two.
// Actual GLSL exp2/clamp/D32 roundtrip is proved by the separate GPU fixture.
#include "renderer/native/packed_secondary_stencil_clear_kernel.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <vector>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool pass,const char* why){++checks;if(!pass)throw std::runtime_error(why);}
uint32_t DecodeBits(uint32_t d){
    const auto e=d>>20,m=d&0xfffff;
    if(e)return ((e+112)<<23)|(m<<3);
    if(!m)return 0;
    const auto k=31u-std::countl_zero(m);
    return ((k+93)<<23)|((m-(1u<<k))<<(23-k));
}
uint32_t CanonicalEncodeModel(float value){
    if(!(value>0))return 0;
    uint32_t b=std::bit_cast<uint32_t>(value);b=std::min(b,0x3ffffff8u);
    const uint32_t p=b<0x38800000u?((0x800000u|(b&0x7fffffu))>>std::min(113u-(b>>23),24u)):b+0xc8000000u;
    return std::min((p+3+((p>>3)&1))>>3,0xffffffu);
}
}
int main()try{
    const std::array<int32_t,4> full{0,0,640,360};
    Check(NativePackedSecondaryStencilClearLayoutAllowed(640,360,640,1440,1,true,full),"Exact secondary layout declined");
    for(unsigned f=0;f<6;++f){std::array<uint32_t,5> a{640,360,640,1440,1};bool depth=true;if(f<5)++a[f];else depth=false;
        Check(!NativePackedSecondaryStencilClearLayoutAllowed(a[0],a[1],a[2],a[3],a[4],depth,full),"Changed secondary layout admitted");}
    for(const auto& r:std::vector<std::array<int32_t,4>>{{-1,0,1,1},{0,-1,1,1},{0,0,641,1},{0,0,1,361},{1,0,1,1},{0,2,1,1}})
        Check(!NativePackedSecondaryStencilClearLayoutAllowed(640,360,640,1440,1,true,r),"Invalid secondary rectangle admitted");
    const auto push=NativePackedSecondaryStencilClearParameters({79,351,82,360},0x123);
    Check(push==std::array<uint32_t,8>{79,351,82,360,0x23,0,0,0},"Secondary32-byte push ABI changed");
    for(uint32_t d=0;d<0x1000000;++d){const auto e=d>>20,m=d&0xfffff;
        const double exact=e?std::ldexp(double(0x100000u+m),int(e)-35):std::ldexp(double(m),-34);
        const float decoded=std::bit_cast<float>(DecodeBits(d));
        Check(double(decoded)==exact,"Independent fullFP24 decode arithmetic differs");
        Check(std::isfinite(decoded)&&decoded>=0&&(!d||std::isnormal(decoded)),"Packed depth produced a nonnormal special value");
        Check(CanonicalEncodeModel(decoded)==d,"Ideal helper inverse differs");
        Check(CanonicalEncodeModel(std::min(decoded,1.f))==(d<=0xf00000?d:0xf00000),"Actual viewport clamp model differs");
    }
    Check(DecodeBits(0xf00001)>std::bit_cast<uint32_t>(1.f)&&CanonicalEncodeModel(1.f)==0xf00000,
          "Above-unit raw stencil-mask counterexample disappeared");

    std::vector<bool> words(2048*1280);uint64_t unique=0,padding=0;
    for(uint32_t y=0;y<360;++y)for(uint32_t x=0;x<640;++x){
        const uint32_t column=x%80,swapped=column<40?column+40:column-40;
        const uint32_t index=(1440+(y/16)*8+x/80)*1280+(y%16)*80+swapped;
        Check(index>=1440*1280&&index<1624*1280&&!words[index],"Secondary word map escaped/became noninjective");
        words[index]=true;++unique;
    }
    for(uint32_t tile=1616;tile<1624;++tile)for(uint32_t row=8;row<16;++row)for(uint32_t column=0;column<80;++column){
        Check(!words[tile*1280+row*80+column],"Secondary map writes final-tile padding");++padding;
    }
    Check(unique==230400&&padding*4==20480,"Secondary exact footprint/padding differs");
    std::cout<<"PASS "<<checks<<" checks; unchanged all16777216 FP24 clamp model, exact secondaryword map and20480 untouched padding bytes; actual GPU proof separate\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
