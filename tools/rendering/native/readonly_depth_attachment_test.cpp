#include "renderer/native/readonly_depth_attachment.h"
#include "renderer/native/readonly_depth_less.h"
#include "readonly_depth_attachment_frag_spv.h"
#include <vector>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool value) {
    ++checks;if(!value)throw std::runtime_error("Readonly native depth independent oracle failed");
}
double OriginalFloat24(uint32_t code) {
    const uint32_t exponent=code/1048576u,mantissa=code%1048576u;
    return std::ldexp(double(exponent?mantissa+1048576u:mantissa),exponent?int(exponent)-35:-34);
}
// Independent numerical oracle preserves the original subnormal guard-bit
// truncation before nearest-even rounding. This intentionally is not an ideal
// IEEE quantizer; just-above-half ties can truncate onto a tie in the guest
// format. Binary search uses numerical values, no candidate bit formula.
uint32_t Quantize(float value) {
    double v=std::isnan(value)?0.0:std::clamp(double(value),0.0,1.0);
    if(v<0x1p-14)v=std::floor(v*0x1p37)*0x1p-37;
    uint32_t first=0,last=0xffffffu;
    while(first<last) {
        const auto mid=first+(last-first)/2;
        if(OriginalFloat24(mid)<v)first=mid+1;else last=mid;
    }
    if(!first)return 0;
    const auto upper=OriginalFloat24(first),lower=OriginalFloat24(first-1);
    const double above=upper-v,below=v-lower;
    if(above<below)return first;
    if(below<above)return first-1;
    return (first&1)?first-1:first;
}
void CheckDepthLessModule() {
    const std::span<const uint32_t> original(readonly_depth_attachment_frag_spv);
    Require(original.size()==1175);
    Require(ContentHash({reinterpret_cast<const uint8_t*>(original.data()),original.size_bytes()})==
        kNativeReadonlyDepthLessSourceSpirv);
    const auto transformed=TryNativeReadonlyDepthLess(original);
    Require(transformed.has_value()&&transformed->size()==original.size()+3);
    // Build the expected change independently from the original instruction
    // stream. Exact whole-vector equality proves all IDs/arithmetic/stores stay.
    size_t insertion=0;unsigned origin=0,replacing=0;
    for(size_t at=5;at<original.size();) {
        const auto count=original[at]>>16,opcode=original[at]&0xffffu;
        Require(count&&count<=original.size()-at);
        if(opcode==16) {
            Require(count==3&&original[at+1]==4);
            if(original[at+2]==7)++origin;
            else if(original[at+2]==12){++replacing;insertion=at+count;}
            else Require(false);
        }
        at+=count;
    }
    Require(origin==1&&replacing==1&&insertion>5);
    std::vector<uint32_t> expected(original.begin(),original.end());
    expected.insert(expected.begin()+insertion,{0x00030010u,4u,15u});
    Require(*transformed==expected&&(*transformed)[3]==original[3]);
    Require(!TryNativeReadonlyDepthLess(*transformed)); // Never double-insert.
    // Every original word, including header/debug/interface/control arithmetic,
    // is authenticated. An unrelated valid module is not an accepted source.
    std::vector<uint32_t> changed(original.begin(),original.end());
    for(size_t word=0;word<changed.size();++word) {
        changed[word]^=1u;Require(!TryNativeReadonlyDepthLess(changed));changed[word]^=1u;
    }
    for(size_t size:{size_t{0},size_t{4},original.size()-1})
        Require(!TryNativeReadonlyDepthLess(original.first(size)));
    changed.push_back(0);Require(!TryNativeReadonlyDepthLess(changed));changed.pop_back();
    // Same-size duplicate/early/conservative modes also fail the pinned source.
    for(uint32_t mode:{9u,14u,15u,16u}) {
        changed[insertion-1]=mode;Require(!TryNativeReadonlyDepthLess(changed));
    }
    std::cout<<"DepthLess pinned SPIR-V PASS1175 source-word mutations; exact sole3-word execution-mode insertion\n";
}

void CheckDepthLessIntervals() {
    // For positive finite z<2^-14 the original guard integer is floor(z*2^37).
    // At/above2^-14 it is a monotone offset of the positive float bit pattern;
    // the two branches join at adjacent integers0x7fffff/0x800000. Nearest-even
    // R(p)=(p+3+((p>>3)&1))/8 is nondecreasing for integer p. Thus exact first-
    // input anchors for EVERY output code define all intervals, without any
    // assumption that a handful of random values covers their interiors.
    const auto zero_end=std::nextafter(5.f*0x1p-37f,0.f);
    Require(NativeReadonlyDepthCode(0.f)==0&&NativeReadonlyDepthCode(-0.f)==0);
    Require(NativeReadonlyDepthCode(zero_end)==0);
    Require(std::numeric_limits<float>::min()<zero_end);
    for(float z:{std::numeric_limits<float>::denorm_min(),
                 std::nextafter(std::numeric_limits<float>::min(),0.f),
                 std::numeric_limits<float>::min()}) {
        Require(NativeReadonlyDepthCode(z)==0&&NativeReadonlyIncomingD32(z)==0.f);
    }
    float previous_minimum=0.f;uint32_t intervals=1;
    for(uint32_t code=1;code<=0xf00000u;++code) {
        float minimum;
        if(code<0x100000u) {
            // Odd ties need the NEXT guard-grid point, not merely nextfloat.
            const uint64_t guard=uint64_t(code)*8-((code&1u)?3u:4u);
            const double exact=std::ldexp(double(guard),-37);
            minimum=float(exact);Require(double(minimum)==exact);
        } else {
            const double midpoint=(OriginalFloat24(code-1)+OriginalFloat24(code))*0.5;
            minimum=float(midpoint);Require(double(minimum)==midpoint);
            if(code&1u)minimum=std::nextafter(minimum,INFINITY);
        }
        const float predecessor=std::nextafter(minimum,0.f);
        Require(std::isfinite(minimum)&&minimum>previous_minimum&&minimum<=1.f);
        Require(NativeReadonlyDepthCode(minimum)==code);
        Require(NativeReadonlyDepthCode(predecessor)==code-1);
        const float encoded=NativeReadonlyDepthD32(code<<8);
        Require(double(encoded)==OriginalFloat24(code)*0.5&&encoded<=minimum);
        previous_minimum=minimum;++intervals;
    }
    Require(intervals==0xf00001u&&NativeReadonlyDepthCode(1.f)==0xf00000u);
    // Every interval has constant output<=its smallest input; monotonicity and
    // predecessor checks cover every defined float in[0,1], including all
    // float32 subnormals inside interval0. Values above this output range are
    // unreachable from[0,1], though the existing all24-bit decode oracle remains.
    const std::array<float,9> derivatives{0.f,-0.f,1.f,-1.f,
        std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),
        INFINITY,-INFINITY,NAN};
    for(float z:{-0.f,0.f,std::numeric_limits<float>::denorm_min(),zero_end,
                 0x1p-14f,.125f,.5f,1.f})
        for(float slope_bias:{0.f,-0.f})for(float constant_bias:{0.f,-0.f})
            for(float dx:derivatives)for(float dy:derivatives) {
                // Separate multiply/add mirrors the original precise policy.
                // Infinite/NaN derivatives may make0*bias nonfinite; the
                // existing NaN->0 quantizer still produces0<=original finite z.
                volatile float slope=std::max(std::abs(dx),std::abs(dy))*slope_bias;
                volatile float offset=slope+constant_bias;
                const float biased=z+offset;
                Require(std::isnan(biased)||biased==z);
                Require(NativeReadonlyIncomingD32(biased)<=z);
            }
    Require(NativeReadonlyIncomingD32(NAN)==0.f);
    std::cout<<"DepthLess interval bound PASS "<<intervals
        <<" quantizer intervals; all defined unit floats by monotonic partition, signedzero/subnormal/NaN-bias policy; no nonfinite FragCoord claim\n";
}
}
int main() {
    CheckDepthLessModule();
    CheckDepthLessIntervals();
    float previous=-1.f;
    // Every24-bit code, including every exponent boundary and all subnormals,
    // is exactly decoded, finite and injective in the native D32 representation.
    for(uint32_t code=0;code<=0xffffffu;++code) {
        const float original=float(OriginalFloat24(code)*0.5);
        const auto actual=NativeReadonlyDepthD32((code<<8)|0xa7u);
        Require(std::bit_cast<uint32_t>(actual)==std::bit_cast<uint32_t>(original));
        Require(std::isfinite(actual)&&actual>=0.f&&actual<1.f&&actual>previous);
        Require(NativeReadonlyDepthD32(code<<8)==actual);previous=actual;
    }
    for(float value:{-INFINITY,-1.f,-0.f,0.f,0x1p-36f,0x1p-35f,0x1p-34f,
                     0x1p-14f,.125f,.5f,1.f,INFINITY,NAN})
        Require(NativeReadonlyDepthCode(value)==Quantize(value));
    // Original subnormal arithmetic discards bits below its three guard bits.
    // Preserve that behavior even just above a mathematically exact half tie.
    Require(NativeReadonlyDepthCode(std::nextafter(0x1p-35f,INFINITY))==0);
    for(uint32_t code=0;code<4096;++code) {
        const float tie=float((OriginalFloat24(code)+OriginalFloat24(code+1))*0.5);
        for(float value:{std::nextafter(tie,0.f),tie,std::nextafter(tie,INFINITY)})
            Require(NativeReadonlyDepthCode(value)==Quantize(value));
    }
    uint32_t random=0x4f571a32;
    for(uint32_t i=0;i<65536;++i) {
        random=random*1664525u+1013904223u;const uint32_t a=random&0xffffffu;
        random=random*1664525u+1013904223u;const uint32_t b=random&0xffffffu;
        Require((a>=b)==(NativeReadonlyDepthD32(a<<8)>=NativeReadonlyDepthD32(b<<8)));
        const float exact=float(OriginalFloat24(a));
        for(float value:{exact,std::nextafter(exact,0.f),std::nextafter(exact,INFINITY)})
            Require(NativeReadonlyDepthCode(value)==Quantize(value));
        // All source float32 bits, including NaN/Inf/sign, enter the original
        // clamp+quantizer policy, not a guessed normalized hardware depth.
        Require(NativeReadonlyDepthCode(std::bit_cast<float>(random))==Quantize(std::bit_cast<float>(random)));
    }
    NativeFsiConstants raster;raster.pitch=640;raster.depth_base=1440;raster.depth_format=1;raster.depth_control=0x700762;
    auto eligible=[&](const NativeFsiConstants& r,float lo=0.f,float hi=1.f) {
        return NativeReadonlyDepthAttachmentEligible(r,640,360,0,false,true,true,lo,hi,640);
    };
    Require(eligible(raster));
    Require(eligible(raster,1.f,0.f));
    auto signed_zero=raster;signed_zero.front_constant_bias=signed_zero.back_constant_bias=-0.f;
    Require(eligible(signed_zero)&&eligible(signed_zero,1.f,0.f));
    for(uint32_t control:{0u,0x700776u,0x700763u,0x700752u}) {auto bad=raster;bad.depth_control=control;Require(!eligible(bad));}
    for(unsigned field=0;field<4;++field)for(float value:{std::numeric_limits<float>::denorm_min(),-std::numeric_limits<float>::denorm_min(),0x1p-24f,-.125f,NAN,INFINITY}) {
        auto bad=raster;
        if(field==0)bad.front_slope_bias=value;
        if(field==1)bad.front_constant_bias=value;
        if(field==2)bad.back_slope_bias=value;
        if(field==3)bad.back_constant_bias=value;
        Require(!eligible(bad)&&!eligible(bad,1.f,0.f));
    }
    const std::array<std::array<float,2>,12> ranges{{{.1f,1.f},{0.f,.9f},{1.f,.1f},{.9f,0.f},
        {0.f,0.f},{1.f,1.f},{NAN,1.f},{1.f,NAN},{INFINITY,0.f},{1.f,-INFINITY},
        {std::nextafter(1.f,0.f),0.f},{1.f,std::nextafter(0.f,1.f)}}};
    for(const auto& range:ranges)
        Require(!eligible(raster,range[0],range[1]));
    Require(!NativeReadonlyDepthAttachmentEligible(raster,640,360,1,false,true,true,0,1));
    Require(!NativeReadonlyDepthAttachmentEligible(raster,640,360,0,true,true,true,0,1));
    Require(!NativeReadonlyDepthAttachmentEligible(raster,640,360,0,false,false,true,0,1));
    Require(!NativeReadonlyDepthAttachmentEligible(raster,640,360,0,false,true,false,0,1));
    Require(!NativeReadonlyDepthAttachmentEligible(raster,640,360,0,false,true,true,0,1,0x30000));
    std::cout<<"Readonly D32 exact scaled-float24 oracle PASS "<<checks<<" checks, all16777216 depth codes; no GPU or live FPS proof\n";
}
