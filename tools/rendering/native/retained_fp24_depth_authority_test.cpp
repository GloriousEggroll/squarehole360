// Pure metadata/inverse oracle. Actual shader/raster/query/fence proof is separate.
#include "renderer/native/retained_fp24_depth_authority.h"
#include "renderer/native/retained_fp24_depth.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>
#include <vector>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool pass,const char* why){++checks;if(!pass)throw std::runtime_error(why);}
template<class F>void Poison(F f){NativeRetainedFp24DepthAuthority a;a.Admit(1,kNativeRetainedFp24DepthLayout,7);bool threw=false;try{f(a);}catch(const std::logic_error&){threw=true;}Check(threw&&a.failed(),"Accepted invalid authority must poison");}
uint32_t DecodeBits(uint32_t code){const auto e=code>>20,m=code&0xfffff;return e?((e+111)<<23)|(m<<3):std::bit_cast<uint32_t>(float(m)*std::ldexp(1.f,-35));}
uint32_t ShaderInverse(float d){const auto bits=std::bit_cast<uint32_t>(d);return bits<0x38000000?uint32_t(d*std::ldexp(1.f,35)):(((bits>>23)-111)<<20)|((bits&0x7fffff)>>3);}
size_t Word(uint32_t x,uint32_t y){return size_t((y/16)*16+x/80)*1280+(y%16)*80+((x%80+40)%80);}
struct Pages {
    std::vector<uint64_t> versions=std::vector<uint64_t>(2560,1);uint64_t serial=1;
    uint64_t Depth()const{return *std::max_element(versions.begin(),versions.begin()+900);}
    void WriteDepth(){++serial;std::fill(versions.begin(),versions.begin()+900,serial);}
    void Unrelated(){versions.back()=++serial;}
    void Overlap(){versions[2]=++serial;}
};
}
int main()try{
static_assert(!std::is_copy_constructible_v<NativeRetainedFp24DepthAuthority> && !std::is_move_constructible_v<NativeRetainedFp24DepthAuthority>);
NativeFsiConstants fsi{};fsi.pitch=1280;fsi.depth_control=0x700766;fsi.depth_format=1;
const std::array<uint32_t,6> raster{3,1,1,4,0,0};const auto source=kNativeRetainedFp24DepthSources[0].source;
const auto allowed=[&](const auto& f,const auto& r){return NativeRetainedFp24DepthAllowed(f,1280,720,1280,0,false,source,r,1.f,0.f);};
Check(allowed(fsi,raster),"Exact fixed state must be admitted");
for(unsigned field=0;field<12;++field){auto bad=fsi;switch(field){case 0:bad.pitch=1279;break;case 1:bad.depth_base=1;break;case 2:bad.depth_format=0;break;case 3:bad.depth_control=0x700762;break;case 4:bad.color[0].mask=1;break;case 5:bad.color[3].mask=15;break;case 6:bad.front_slope_bias=.125f;break;case 7:bad.back_constant_bias=.125f;break;case 8:bad.front_constant_bias=std::numeric_limits<float>::quiet_NaN();break;case 9:bad.back_slope_bias=std::numeric_limits<float>::infinity();break;case 10:bad.depth_control|=1;break;case 11:bad.depth_control=0x700736;break;}Check(!allowed(bad,raster),"One-field final FSI refusal failed");}
for(unsigned field=0;field<6;++field){auto bad=raster;bad[field]=field>=4?1:0;Check(!allowed(fsi,bad),"One-field raster refusal failed");}
Check(!NativeRetainedFp24DepthAllowed(fsi,1279,720,1280,0,false,source,raster,1,0),"Extent refusal failed");
Check(!NativeRetainedFp24DepthAllowed(fsi,1280,720,1280,1,false,source,raster,1,0),"Raw mask refusal failed");
Check(!NativeRetainedFp24DepthAllowed(fsi,1280,720,1280,0,true,source,raster,1,0),"Present pixel shader refusal failed");
Check(!NativeRetainedFp24DepthAllowed(fsi,1280,720,1280,0,false,"unknown",raster,1,0),"Unknown source refusal failed");
Check(!NativeRetainedFp24DepthAllowed(fsi,1280,720,1280|0x10000,0,false,source,raster,1,0),"Multisample refusal failed");
Check(!NativeRetainedFp24DepthAllowed(fsi,1280,720,1280,0,false,source,raster,0,1),"Forward viewport refusal failed");
fsi.front_constant_bias=-0.f;fsi.back_slope_bias=-0.f;Check(allowed(fsi,raster),"Signed zero effective bias incorrectly refused");
// Independent double arithmetic proves every original code is exactly representable
// at half scale and that the shader's integer inverse restores the ORIGINAL code.
for(uint32_t code=0;code<0x1000000;++code){const auto e=code>>20,m=code&0xfffff;
    const double oracle=e?std::ldexp(1.+double(m)/1048576.,int(e)-16):std::ldexp(double(m),-35);
    const float d=std::bit_cast<float>(DecodeBits(code));
    Check(double(d)==oracle&&d>=0.f&&d<1.f,"Half FP24 representation differs from independent arithmetic");
    Check(ShaderInverse(d)==code,"Half FP24 inverse altered original packed code");}
constexpr size_t words=2048u*1280;std::vector<uint32_t> baseline(words),retained;
uint32_t random=0x73123456;for(auto& word:baseline){random^=random<<13;random^=random>>17;random^=random<<5;word=random;}retained=baseline;
std::vector<uint32_t> image(1280u*720);for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x)image[y*1280+x]=baseline.at(Word(x,y))>>8;
NativeRetainedFp24DepthAuthority a;Pages pages;a.Admit(1,kNativeRetainedFp24DepthLayout,pages.Depth());uint64_t modeled_samples=0;
for(unsigned draw=0;draw<96;++draw){const auto ticket=a.ticket();const auto pre=pages.Depth();
    // Alternating source labels have no authority effect; both accepted sources
    // compare GE and replace the same exact FP24 code. Only selective live pixels
    // are changed, so the eventual full10MiB oracle checks untouched words too.
    for(uint32_t k=0;k<128;++k){const auto x=(k*37+draw*11)%1280,y=(k*13+draw*7)%720;const size_t physical=Word(x,y),linear=y*1280+x;
        const uint32_t incoming=((draw+3)*1234567u+k*73)&0xffffff;const bool pass=incoming>=(baseline[physical]>>8);
        Check(pass==(incoming>=image[linear]),"Retained/native GE comparison differs");
        if(pass){baseline[physical]=(incoming<<8)|(baseline[physical]&255);image[linear]=incoming;++modeled_samples;}}
    pages.WriteDepth();a.AcceptedDraw(ticket,pre,pages.Depth(),true,false);
    Check(a.dirty()&&a.Reusable(kNativeRetainedFp24DepthLayout,pages.Depth()),"Accepted writer did not retain exact authority");
    pages.Unrelated();Check(a.Reusable(kNativeRetainedFp24DepthLayout,pages.Depth()),"Unrelated serial changed exact depth token");}
Check(modeled_samples>0,"Nonvacuous modeled GE replacement required");
const auto ticket=a.ticket();a.BeforeMaterialize(pages.Depth());a.Materialized(ticket,false);Check(a.dirty()&&a.present(),"Failed admission discarded authoritative bytes");
// An already fenced generation remains dirty until explicit canonical export.
Check(retained!=baseline,"Fenced dirty model unexpectedly canonical before store");
for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x){const auto physical=Word(x,y);retained.at(physical)=(ShaderInverse(std::bit_cast<float>(DecodeBits(image[y*1280+x])))<<8)|(retained[physical]&255);}
Check(retained==baseline,"Full10MiB depth/color/stencil/untouched oracle differs");a.Materialized(ticket,true);Check(!a.present()&&!a.dirty(),"Accepted export failed to close authority");
a.Admit(2,kNativeRetainedFp24DepthLayout,pages.Depth());Check(a.present(),"Fresh generation not reusable");
Poison([](auto& x){auto t=x.ticket();++t.generation;x.AcceptedDraw(t,7,8,true,false);});
Poison([](auto& x){x.AcceptedDraw(x.ticket(),6,8,true,false);});
Poison([](auto& x){x.AcceptedDraw(x.ticket(),7,7,true,false);});
Poison([](auto& x){x.AcceptedDraw(x.ticket(),7,8,false,false);});
Poison([](auto& x){x.AcceptedDraw(x.ticket(),7,8,true,true);});
Poison([](auto& x){x.BeforeMaterialize(9);});
Poison([](auto& x){auto t=x.ticket();++t.depth;x.Materialized(t,true);});
Poison([](auto& x){auto layout=kNativeRetainedFp24DepthLayout;++layout[3];x.Admit(2,layout,7);});
Poison([](auto& x){x.Admit(1,kNativeRetainedFp24DepthLayout,7);});
NativeRetainedFp24DepthAuthority b;b.Admit(1,kNativeRetainedFp24DepthLayout,7);auto wrong=kNativeRetainedFp24DepthLayout;++wrong[2];Check(!b.Reusable(wrong,7)&&!b.Reusable(kNativeRetainedFp24DepthLayout,8),"Layout/token mismatch reused image");
std::cout<<"PASS "<<checks<<" checks, all16777216 FP24 codes,96 two-source modeled draws, full10485760 bytes, depth-only exact authority; actual GPU proof pending\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
