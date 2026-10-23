#pragma once
#include <array>
#include <cstdint>
namespace gow3::renderer {
// Coordinate proof only. The caller separately authenticates original canonical
// RGBA alpha/pre-encode provenance and owns that source through the real fence.
inline bool NativeAtlasRgbaStencilStoreRegionAllowed(uint32_t width,uint32_t height,uint32_t format,
    uint32_t source_width,uint32_t source_height,const std::array<int32_t,4>& r,int32_t dx,int32_t dy) noexcept {
    return width==1120&&height==688&&format==22&&source_width==1120&&source_height==688&&
        r[0]>=0&&r[1]>=0&&r[2]>r[0]&&r[3]>r[1]&&r[2]<=1120&&r[3]<=688&&
        int64_t(r[0])+dx>=0&&int64_t(r[1])+dy>=0&&int64_t(r[2])+dx<=1120&&int64_t(r[3])+dy<=688;
}
} // namespace gow3::renderer
