#include "renderer/native/early_depth_lower_bound.h"
#include "renderer/native/readonly_depth_attachment.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool value) {
    ++checks;
    if(!value)throw std::runtime_error("Conservative early-depth numerical contract failed");
}
// Arithmetic oracle independent of the candidate's float32 bit construction.
double FullFp24(uint32_t code) {
    const uint32_t exponent=code/1048576u,mantissa=code%1048576u;
    return std::ldexp(double(exponent?mantissa+1048576u:mantissa),
                      exponent?int(exponent)-35:-34);
}
}
int main() {
    float previous=0.f;
    for(uint32_t anchor=0;anchor<=0xffffffu;++anchor) {
        const float oracle=float(std::min(1.0,FullFp24(anchor?anchor-1u:0u)));
        const float lower=NativeEarlyDepthLowerBound((anchor<<8)|0xa7u);
        Require(std::bit_cast<uint32_t>(lower)==std::bit_cast<uint32_t>(oracle));
        Require(std::isfinite(lower)&&lower>=previous&&lower>=0.f&&lower<=1.f);
        Require(NativeEarlyDepthLowerBound(anchor<<8)==lower); // Stencil unaffected.
        if(anchor&&lower>0.f) {
            const float largest_rejected=std::nextafter(lower,0.f);
            Require(NativeReadonlyDepthCode(largest_rejected)<anchor);
            // The boundary is deliberately conservative, retaining the previous
            // quantization bin rather than trying to encode a fragile half tie.
            if(anchor<=0xf00000u)Require(NativeReadonlyDepthCode(lower)==anchor-1u);
        }
        if(anchor<=0xf00000u) {
            const float center=float(FullFp24(anchor));
            Require(NativeReadonlyDepthCode(center)==anchor);
            Require(center>=lower);
            const float successor=std::nextafter(center,1.f);
            Require(NativeReadonlyDepthCode(successor)>=anchor&&successor>=lower);
        }
        previous=lower;
    }
    // Original subnormal guard-bit truncation is part of the real quantizer.
    for(uint32_t code=0;code<8192;++code) {
        const float tie=float((FullFp24(code)+FullFp24(code+1u))*0.5);
        for(float z:{std::nextafter(tie,0.f),tie,std::nextafter(tie,1.f)}) {
            const uint32_t q=NativeReadonlyDepthCode(z);
            Require(z>=NativeEarlyDepthLowerBound(q<<8));
            if(q<0xffffffu&&z<NativeEarlyDepthLowerBound((q+1u)<<8))
                Require(NativeReadonlyDepthCode(z)<q+1u);
        }
    }
    Require(NativeEarlyDepthLowerBound(0)==0.f);
    Require(NativeEarlyDepthLowerBound(0x000001ffu)==0.f);
    Require(NativeEarlyDepthLowerBound(0xffffff00u)==1.f);
    std::cout<<"Early-depth conservative full-FP24 predecessor PASS "<<checks
             <<" checks, all16777216 anchors; conditional numerical proof only, no GPU/FPS claim\n";
}
