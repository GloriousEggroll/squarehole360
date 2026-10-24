#pragma once
#include <cstdint>

namespace gow3::renderer {
// The console's EDRAM holds 2048 tiles of 5120 bytes and addresses wrap there.
// Render sizes above 720p (gow3_render_height) place surfaces past that point.
// The guest D3D stores 12-bit tile bases (below 4096), but at 1080p the last
// surface starting below 4096 still extends past it, so the larger model has
// 8192 tiles and never wraps in practice. Set once before the renderer
// starts; 2048 keeps console behavior.
inline constexpr uint32_t kNativeEdramMaxTiles=8192;
inline uint32_t native_edram_tiles=2048;
inline uint32_t NativeEdramTiles() {return native_edram_tiles;}
inline uint32_t NativeEdramBytes() {return native_edram_tiles*5120u;}
// Fast paths whose shaders wrap at the console's 2048 tiles: in the larger
// model, admit only surfaces that end before tile 2048, where both agree.
inline bool NativeEdramLegacyWrapSafe(uint64_t base,uint64_t last_tile) {
    return native_edram_tiles==2048||base+last_tile<2048;
}
inline void SetNativeEdramTiles(uint32_t tiles) {native_edram_tiles=tiles==kNativeEdramMaxTiles?kNativeEdramMaxTiles:2048;}
// Tile base field of a guest surface: 11 bits on the console, 12 when larger.
inline uint32_t NativeEdramBaseMask() {return native_edram_tiles==2048?2047u:4095u;}
}
