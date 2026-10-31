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
// A texture with no GPU-defined bytes decodes from its payloads and layout
// fields alone; decoding uses offsets within each payload, never the guest
// address. Keep only whether each address is present, which selects levels, so
// a texture the game moves unchanged reuses its decoded image.
inline std::array<uint32_t,6> CpuTextureContentKey(std::array<uint32_t,6> fetch) noexcept {
    for(const unsigned word:{1u,5u})
        if(fetch[word]&0x1FFFF000u)fetch[word]=(fetch[word]&~0x1FFFF000u)|0x1000u;
    return fetch;
}
// Content keys never collide with a real version, so they cannot share an
// entry whose image includes GPU-defined bytes.
inline constexpr uint64_t kCpuTextureContentVersion=~uint64_t{0};
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
