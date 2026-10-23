#include "renderer/native/packed_primary_stencil_clear.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
NativePackedPrimaryStencilClearInput Input() {
    NativePackedPrimaryStencilClearInput x;
    x.native_fsi=x.source_depth=true;x.width=x.pitch=1280;x.height=720;
    x.format=1;x.surface_info=335545600;x.flags=32;x.depth=0.f;
    x.rectangle={0,0,1280,720};return x;
}
// Independent CPU numerical model. ldexp is exact at these representable
// powers; this does NOT establish actual GLSL dynamic-exp2 precision or the
// original Vulkan D32 clamp. The actual canonical GPU roundtrip is required.
float Decode(uint32_t d) {
    const uint32_t e=d>>20,m=d&0xfffffu;
    return e?std::ldexp(float(m|0x100000u),int(e)-35):std::ldexp(float(m),-34);
}
uint32_t Encode(float value) {
    if(!(value>0.f))return 0;
    const uint32_t bits=std::bit_cast<uint32_t>(std::min(value,std::bit_cast<float>(0x3ffffff8u)));
    const uint32_t shifted=bits<0x38800000u?
        ((0x800000u|(bits&0x7fffffu))>>std::min(113u-(bits>>23),24u)):bits+0xc8000000u;
    return std::min((shifted+3u+((shifted>>3)&1u))>>3,0xffffffu);
}
uint32_t OriginalModel(uint32_t word,bool in_rectangle,uint32_t stencil) {
    const float restored=std::clamp(Decode(word>>8),0.f,1.f);
    return (Encode(restored)<<8)|(in_rectangle?(stencil&255u):(word&255u));
}
uint32_t CandidateModel(uint32_t word,bool in_rectangle,uint32_t stencil) {
    return (std::min(word>>8,0xf00000u)<<8)|(in_rectangle?(stencil&255u):(word&255u));
}
struct HostModel {
    bool present=false,dirty=false;
    uint64_t host_version=0,packed_version=0;
    float host_depth=0.f;
    uint32_t packed_depth=0;
    float Read() {
        if(!present||host_version!=packed_version) {
            host_depth=std::clamp(Decode(packed_depth),0.f,1.f);
            host_version=packed_version;present=true;
        }
        return host_depth;
    }
};
}
int main()try {
    const auto input=Input();Require(NativePackedPrimaryStencilClearAllowed(input),"Primary clear refused");
    const auto refuse=[&](auto mutate) {auto x=input;mutate(x);
        Require(!NativePackedPrimaryStencilClearAllowed(x),"Unsupported clear admitted");};
    refuse([](auto& x){x.native_fsi=false;});refuse([](auto& x){x.query_active=true;});
    refuse([](auto& x){x.source_depth=false;});refuse([](auto& x){x.width=1279;});
    refuse([](auto& x){x.height=721;});refuse([](auto& x){x.pitch=1120;});
    refuse([](auto& x){x.base=1440;});refuse([](auto& x){x.format=0;});
    refuse([](auto& x){x.surface_info^=1;});refuse([](auto& x){x.surface_info|=0x10000;});
    refuse([](auto& x){x.surface_info|=0x20000;});refuse([](auto& x){x.flags=16;});
    refuse([](auto& x){x.flags=48;});refuse([](auto& x){x.flags=33;});
    refuse([](auto& x){x.depth=-1.f;});refuse([](auto& x){x.depth=std::nextafter(1.f,2.f);});
    refuse([](auto& x){x.depth=std::numeric_limits<float>::quiet_NaN();});
    refuse([](auto& x){x.depth=std::numeric_limits<float>::infinity();});
    refuse([](auto& x){x.rectangle[0]=-1;});refuse([](auto& x){x.rectangle[1]=-1;});
    refuse([](auto& x){x.rectangle[2]=1281;});refuse([](auto& x){x.rectangle[3]=721;});
    refuse([](auto& x){x.rectangle[2]=x.rectangle[0];});
    refuse([](auto& x){x.rectangle[3]=x.rectangle[1];});
    for(const auto& rectangle:std::vector<std::array<int32_t,4>>{
            {0,0,1280,720},{512,320,608,384},{1279,719,1280,720},{41,17,179,91}}) {
        auto x=input;x.rectangle=rectangle;
        Require(NativePackedPrimaryStencilClearAllowed(x),"In-bounds primary rectangle refused");
    }
    for(float depth:{0.f,-0.f,0.5f,1.f}) {
        auto x=input;x.depth=depth;
        Require(NativePackedPrimaryStencilClearAllowed(x),"Valid unused depth scalar refused");
    }
    for(unsigned bits=0;bits<128;++bits) {
        const bool defined=bits&1,present=bits&2,dirty=bits&4,current=bits&8,
            receipt=bits&16,other_dirty=bits&32,zero_token=bits&64;
        const uint64_t packed=zero_token?0:19,host=current?packed:7;
        const auto result=NativePackedPrimaryStencilClearChooseSource(
            defined,present,dirty,host,packed,receipt,other_dirty);
        const auto expected=(!defined||dirty||receipt||other_dirty||zero_token)?
            NativePackedPrimaryStencilClearSource::Refuse:
            present&&current?NativePackedPrimaryStencilClearSource::CurrentHost:
                NativePackedPrimaryStencilClearSource::Packed;
        Require(result==expected,"Packed provenance truth table changed");
    }
    Require(NativePackedPrimaryStencilClearChooseSource(true,true,false,19,20,false,false)==
        NativePackedPrimaryStencilClearSource::Packed,"Post-alias stale token refused");
    Require(NativePackedPrimaryStencilClearChooseSource(true,true,false,20,20,false,false)==
        NativePackedPrimaryStencilClearSource::CurrentHost,"Current restored D32 incorrectly admitted");
    const std::vector<uint32_t> depths{0,1,2,0x7ffff,0xfffff,0x100000,0x100001,
        0x1fffff,0x200000,0x7fffff,0x800000,0xefffff,0xf00000,0xf00001,0xffffff};
    for(uint32_t d:depths)for(uint32_t old_stencil=0;old_stencil<256;++old_stencil)
        for(uint32_t replacement:{0u,1u,127u,255u,256u,0xffffffffu})for(bool inside:{false,true}) {
            const uint32_t word=(d<<8)|old_stencil;
            Require(OriginalModel(word,inside,replacement)==CandidateModel(word,inside,replacement),
                "Normalized stencil model differs from clamped original roundtrip");
            Require((CandidateModel(word,inside,replacement)&255u)==
                (inside?(replacement&255u):old_stencil),"Stencil lanes changed outside their intended scope");
        }
    Require(Decode(0xf00001u)>1.f&&Encode(1.f)==0xf00000u,"Over-one clamp witness is vacuous");
    Require(CandidateModel(0xf0000103u,false,0)!=0xf0000103u,"Full normalization became a raw mask");
    // Keep the old token. Future same-source consumers restore normalized
    // packed bytes; a later mutation must independently refresh again.
    HostModel stale{true,false,17,19,0.125f,0xf00001u};
    stale.packed_depth=std::min(stale.packed_depth,0xf00000u);++stale.packed_version;
    Require(stale.host_version==17,"Candidate spuriously promoted host token");
    Require(std::bit_cast<uint32_t>(stale.Read())==std::bit_cast<uint32_t>(1.f),
        "Future consumer failed to restore canonical normalized packed depth");
    stale.packed_depth=0x800001u;++stale.packed_version;
    Require(std::bit_cast<uint32_t>(stale.Read())==std::bit_cast<uint32_t>(Decode(0x800001u)),
        "Later packed mutation retained obsolete host float");
    HostModel absent{false,false,0,19,0.f,0xf00001u};
    absent.packed_depth=0xf00000u;++absent.packed_version;
    Require(std::bit_cast<uint32_t>(absent.Read())==std::bit_cast<uint32_t>(1.f),
        "Absent future host changed normalized depth");
    // Current host can be clamped while SAME-token packed bits are still high:
    // Surface() full restore itself does not StoreSurface. Always refuse it.
    HostModel current{true,false,19,19,1.f,0xf00001u};
    Require(NativePackedPrimaryStencilClearChooseSource(true,true,false,19,19,false,false)==
        NativePackedPrimaryStencilClearSource::CurrentHost,"Restore-only current token shortcut admitted");
    Require(Encode(current.Read())!=current.packed_depth,"Current-token counterexample is vacuous");
    std::cout<<"Packed primary stencil guard CPU PASS "<<checks
        <<" checks; numerical/host model only, no GLSL, Vulkan, GPU or Scene execution\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
