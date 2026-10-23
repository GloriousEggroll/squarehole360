#pragma once
#include <cstdint>
#include <string_view>

namespace gow3::renderer {
inline constexpr std::string_view kNativeFsiMaterialShaderCacheHeadroomPolicy =
    "native-fsi-material-shader-cache-headroom-v1";
struct NativeFsiMaterialShaderCacheLimits {
    uint32_t entries;
    uint64_t bytes;
};
inline constexpr uint32_t kNativeFsiMaterialShaderCacheExtraEntries = 256;
inline constexpr uint64_t kNativeFsiMaterialShaderCacheExtraBytes = 16u * 1024 * 1024;

// Runtime/replay select the same bounded ceiling. This helper never mutates a
// Scene, reserves memory, changes optional admission, or promotes a small test
// budget. Smaller explicit budgets remain valid for existing pressure fixtures.
inline constexpr NativeFsiMaterialShaderCacheLimits
NativeFsiMaterialShaderCacheMaximum(bool extra_headroom) noexcept {
    return {1024u + (extra_headroom ? kNativeFsiMaterialShaderCacheExtraEntries : 0u),
            64u * 1024 * 1024 + (extra_headroom ? kNativeFsiMaterialShaderCacheExtraBytes : 0u)};
}
inline constexpr bool NativeFsiMaterialShaderCacheBoundsValid(
    bool extra_headroom, bool material_composition, bool specialize_fsi_state,
    uint64_t entries, uint64_t bytes) noexcept {
    if (extra_headroom && (!material_composition || !specialize_fsi_state)) return false;
    const auto maximum = NativeFsiMaterialShaderCacheMaximum(extra_headroom);
    return entries >= 2 && entries <= maximum.entries && bytes && bytes <= maximum.bytes;
}
} // namespace gow3::renderer
