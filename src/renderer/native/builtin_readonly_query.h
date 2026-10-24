#pragma once

#include "edram_extent.h"
#include "fsi_state.h"
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gow3::renderer {

inline constexpr std::string_view kBuiltinReadonlyQueryIdentity =
    "builtin-readonly-depth-query-v2-resident";
inline constexpr std::string_view kReadonlyQueryVertexSource =
    "13b59b3b0b99eac0b63381b62f1baaaa75d4802aeb7559b1aed11d5f9a3ec55a";
inline constexpr std::string_view kReadonlyQueryVertexSpirv =
    "1df4cf1429fa42f11e2b01b727fe3ab1be56abc1f53988ab1a454b33c8073e8b";
inline constexpr std::string_view kReadonlyQueryBuiltinSpirv =
    "0a3fb61d9ff86baad8e1c341cf4309340be244b37598e833b0a39a03babb94da";

// These immutable module identities refer to the audited pinned native vertex
// (Function/Output stores only) and the original depth-only builtin. A compiler
// change is a conservative Native-generic miss, never a guessed shader rewrite.
bool AuditedReadonlyQueryVertex(std::string_view source, std::span<const uint8_t> spirv);
std::optional<std::vector<uint32_t>> TryBuiltinReadonlyQuery(std::span<const uint32_t> spirv);

// Independent of pipeline recipes: every actual draw must prove its dynamic
// state. No descriptor or old recipe is evidence of a read-only draw.
inline bool BuiltinReadonlyQueryAllowed(const NativeFsiConstants& raster,
                                       uint32_t width, uint32_t height,
                                       uint32_t raw_color_mask,
                                       bool pixel_shader_present, bool active_query,
                                       bool audited_vertex, uint32_t surface_info=0) {
    if (pixel_shader_present || !active_query || !audited_vertex || raw_color_mask ||
        (surface_info & 0x30000u) ||
        raster.depth_control != 0x700762 || raster.depth_format != 1 ||
        !width || !height || width > 8192 || height > 8192 ||
        !raster.pitch || raster.pitch > 16383 || raster.depth_base >= 2048)
        return false;
    for (const auto& color : raster.color)
        if (color.mask) return false;
    // This exact control enables depth GE, disables depth writes and stencil.
    // A removed store reconstructs the previous uint32 word exactly.
    // Retain the previously audited finite/injective packed-depth footprint.
    // Read/read overlap itself is safe, but wider layouts are outside this pinned variant.
    const uint64_t columns = (uint64_t(width) + 79) / 80;
    const uint64_t rows = (uint64_t(height) + 15) / 16;
    const uint64_t pitch = (uint64_t(raster.pitch) + 79) / 80;
    return pitch >= columns && (rows - 1) * pitch + columns - 1 < 2048 &&
           NativeEdramLegacyWrapSafe(raster.depth_base, (rows - 1) * pitch + columns - 1);
}

} // namespace gow3::renderer
