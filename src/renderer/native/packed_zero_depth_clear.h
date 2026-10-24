#pragma once

#include "edram_extent.h"
#include <array>
#include <bit>
#include <cstdint>

namespace gow3::renderer {

// Restrict v1 to finite, injective D24FS8 footprints. The existing validated
// SurfaceFootprint still supplies exact wrapped byte ranges, including depth's
// swapped horizontal half-tiles. A zero packed word needs no float conversion.
inline bool PackedZeroDepthLayoutAllowed(uint32_t width,uint32_t height,uint32_t pitch,
                                        uint32_t base,uint32_t format,bool depth) noexcept {
    if(!depth||format!=1||!width||!height||width>8192||height>8192||
       !pitch||pitch>16383||base>=2048)return false;
    const uint64_t columns=(uint64_t(width)+79)/80;
    const uint64_t rows=(uint64_t(height)+15)/16;
    const uint64_t stride=(uint64_t(pitch)+79)/80;
    return stride>=columns&&(rows-1)*stride+columns<=2048&&NativeEdramLegacyWrapSafe(base,(rows-1)*stride+columns-1);
}
struct PackedZeroDepthClearInput {
    bool native_fsi,depth_surface;
    uint32_t flags,width,height,pitch,base,format,surface_info;
    std::array<int32_t,4> rectangle;
    double depth;
    uint32_t stencil;
};
inline bool PackedZeroDepthClearAllowed(const PackedZeroDepthClearInput& input) noexcept {
    return input.native_fsi&&input.flags==0x30&&std::bit_cast<uint64_t>(input.depth)==0&&input.stencil==0&&
        !(input.surface_info&0x30000)&&
        PackedZeroDepthLayoutAllowed(input.width,input.height,input.pitch,input.base,input.format,input.depth_surface)&&
        input.rectangle==std::array<int32_t,4>{0,0,int32_t(input.width),int32_t(input.height)};
}

} // namespace gow3::renderer
