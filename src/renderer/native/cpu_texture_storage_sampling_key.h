#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace gow3::renderer {
// Storage identity only. Keep the original fetch for all layout/validation,
// payload/ownership/decode/upload and per-draw binding operations. Mip limits,
// signs, swizzles, number format, addresses and physical versions stay exact.
inline std::array<uint32_t,6> CpuTextureStorageSamplingKey(
    std::array<uint32_t,6> fetch,bool enabled) noexcept {
    if(enabled) {
        fetch[3]&=~uint32_t{0x01800000}; // mip filter, not mip extent
        fetch[4]&=~uint32_t{0x003ff000}; // signed LOD bias, not min/max mip
    }
    return fetch;
}
// Reserve four of the existing sixteen bindings for the observed zero-bias
// final states. Nonzero animated biases still use the original fence-retained
// transient fallback at this bound. Never erase or mutate an in-flight view.
// This is a bounded admission preference, not a promise that all possible
// nonzero final biases or dynamic anisotropy-setting combinations stay cached.
inline std::size_t CpuTextureSamplingBindingLimit(
    const std::array<uint32_t,6>& fetch,bool shared_cpu_storage) noexcept {
    return shared_cpu_storage&&(fetch[4]&0x003ff000u)?12u:16u;
}
} // namespace gow3::renderer
