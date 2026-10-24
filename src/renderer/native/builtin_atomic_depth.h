#pragma once

#include "edram_extent.h"
#include "fsi_state.h"
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gow3::renderer {

inline constexpr std::string_view kBuiltinAtomicDepthIdentity = "builtin-atomic-depth-max-v1";
inline constexpr std::string_view kAtomicDepthBuiltinSpirv =
    "0a3fb61d9ff86baad8e1c341cf4309340be244b37598e833b0a39a03babb94da";
inline constexpr std::string_view kAtomicDepthVertexSources[] = {
    "a14c7ab2b53a15fd0f1a11ee1a001cd6fb2bc839aa299f3bb10c039cd2ec8665",
    "3f771dd73ee00c6649fb5356ed20dd677ad74156f8505b61e67315be642e15c5"};
inline constexpr std::string_view kAtomicDepthVertexSpirv[] = {
    "5ca1d26332f621747b741c431e784f23199065b7abcfbc663121901be2746c2e",
    "55e3044c9832a75c74107e40a9dcb35b6e93950fb6648074fbd3e5980715a781"};

bool AuditedAtomicDepthVertex(std::string_view source, std::span<const uint8_t> spirv);
std::optional<std::vector<uint32_t>> TryBuiltinAtomicDepth(std::span<const uint32_t> spirv);

// Rechecked on every real draw, independently of pipeline/module cache hits.
// Multisampling has already been rejected by Scene's SurfaceStorage decoding;
// expose it here too so fixture and future callers cannot admit it accidentally.
inline bool BuiltinAtomicDepthAllowed(const NativeFsiConstants& raster,
                                     uint32_t width, uint32_t height,
                                     uint32_t raw_color_mask, bool pixel_shader_present,
                                     bool active_query, bool audited_vertex,
                                     uint32_t surface_info) {
    if (pixel_shader_present || active_query || !audited_vertex || raw_color_mask ||
        (surface_info & 0x30000) || raster.depth_control != 0x700766 ||
        raster.depth_format != 1 || !width || !height || width > 8192 || height > 8192 ||
        !raster.pitch || raster.pitch > 16383 || raster.depth_base >= 2048)
        return false;
    for (const auto& color : raster.color)
        if (color.mask) return false;
    const uint64_t columns = (uint64_t(width) + 79) / 80;
    const uint64_t rows = (uint64_t(height) + 15) / 16;
    const uint64_t pitch = (uint64_t(raster.pitch) + 79) / 80;
    // Depth's half-tile bank swap is a permutation. Unique tile displacements
    // below2048 remain unique after adding a wrapping base modulo2048.
    return pitch >= columns && (rows - 1) * pitch + columns - 1 < 2048 &&
           NativeEdramLegacyWrapSafe(raster.depth_base, (rows - 1) * pitch + columns - 1);
}

} // namespace gow3::renderer
