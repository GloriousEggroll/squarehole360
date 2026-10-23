#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>

namespace gow3::renderer {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
// A conservative hardware GE threshold for a frozen packed FP24 depth word.
// With finite raster z in [0,1], the original monotone quantizer Q, no bias and
// an unchanged raw FragCoord.z, Q(z) >= current >= anchor implies z > F(anchor-1).
// Use the full numerical FP24 predecessor F, never the readonly half embedding.
// This numerical implication does not certify cross-pipeline raster invariance,
// shader side effects, aliasing or the monotonic lifetime of the frozen anchor.
inline float NativeEarlyDepthLowerBound(uint32_t packed_word) noexcept {
    const uint32_t anchor = packed_word >> 8;
    const uint32_t code = anchor ? anchor - 1u : 0u;
    const uint32_t exponent = code >> 20, mantissa = code & 0xfffffu;
    const float value = exponent ?
        std::bit_cast<float>(((exponent + 112u) << 23) | (mantissa << 3)) :
        float(mantissa) * 0x1p-34f;
    // Outside the legal unit range the original quantizer cannot pass. Clamping
    // to one merely accepts a few extra invocations, which the original PS tests.
    return std::min(value, 1.f);
}
}
