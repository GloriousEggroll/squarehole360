#pragma once
#include "builtin_readonly_query.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gow3::renderer {
static_assert(sizeof(float)==4&&std::numeric_limits<float>::is_iec559);
// Exact order embedding of every Xenos unsigned20-mantissa/4-exponent code.
// Decode its numerical value and scale both comparison operands by exactly1/2:
// the largest code remains below1, the smallest positive code is2^-35, and no
// float32 precision/range is lost. Stencil is deliberately ignored, unchanged.
inline float NativeReadonlyDepthD32(uint32_t packed_word) noexcept {
    const uint32_t code=packed_word>>8,exponent=code>>20,mantissa=code&0xfffffu;
    if(exponent)return std::bit_cast<float>(((exponent+111u)<<23)|(mantissa<<3));
    return float(mantissa)*0x1p-35f;
}
// The actual incoming value is the same already-interpolated fragment depth
// consumed by Native's original depth-only epilogue. This is its exact float24
// rounding, preceded by its original NaN/clamp policy; never native float32
// depth as a substitute for guest precision.
inline uint32_t NativeReadonlyDepthCode(float depth) noexcept {
    depth=std::clamp(std::isnan(depth)?0.f:depth,0.f,1.f);
    if(!(depth>0.f))return 0;
    const uint32_t bits=std::bit_cast<uint32_t>(depth);
    const uint32_t rounded=bits<0x38800000u?
        ((0x800000u|(bits&0x7fffffu))>>std::min(113u-(bits>>23),24u)):
        bits+0xc8000000u;
    return std::min((rounded+3u+((rounded>>3)&1u))>>3,0xffffffu);
}
inline float NativeReadonlyIncomingD32(float depth) noexcept {
    return NativeReadonlyDepthD32(NativeReadonlyDepthCode(depth)<<8);
}
// A stricter subcase of the existing immutable builtin query classifier.
// Admit only complete forward/reversed unit viewport depth ranges. FragCoord
// already contains the unchanged original viewport interpolation; both pairs
// have the same permitted[0,1] FragDepth interval. Refuse all polygon bias and
// partial ranges, never approximate hardware bias/interpolation. Format
// capability and original source/vertex authentication are separate mandatory
// device/ABI gates. A false result keeps the owned ordinary Native FSI path.
inline bool NativeReadonlyDepthAttachmentEligible(const NativeFsiConstants& raster,
    uint32_t width,uint32_t height,uint32_t raw_color_mask,bool pixel_shader_present,
    bool active_query,bool audited_vertex,float viewport_min_depth,float viewport_max_depth,
    uint32_t surface_info=0) noexcept {
    return BuiltinReadonlyQueryAllowed(raster,width,height,raw_color_mask,pixel_shader_present,
        active_query,audited_vertex,surface_info)&&
        ((viewport_min_depth==0.f&&viewport_max_depth==1.f)||
         (viewport_min_depth==1.f&&viewport_max_depth==0.f))&&
        raster.front_slope_bias==0.f&&raster.front_constant_bias==0.f&&
        raster.back_slope_bias==0.f&&raster.back_constant_bias==0.f;
}
}
