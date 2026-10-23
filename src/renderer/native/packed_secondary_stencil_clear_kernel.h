#pragma once
#include <array>
#include <cstdint>

namespace gow3::renderer {
inline constexpr uint32_t kNativePackedSecondaryStencilClearWidth=640;
inline constexpr uint32_t kNativePackedSecondaryStencilClearHeight=360;
inline constexpr uint32_t kNativePackedSecondaryStencilClearWords=640*360;
inline constexpr uint32_t kNativePackedSecondaryStencilClearUnitDepth=0xf00000;
// Scene proves query state and stale/absent clean packed provenance first.
// The final eight tiles have only eight valid rows; padding is never written.
inline bool NativePackedSecondaryStencilClearLayoutAllowed(uint32_t width,uint32_t height,
        uint32_t pitch,uint32_t base,uint32_t format,bool depth,
        const std::array<int32_t,4>& r) noexcept {
    return width==640&&height==360&&pitch==640&&base==1440&&format==1&&depth&&
        r[0]>=0&&r[1]>=0&&r[0]<r[2]&&r[1]<r[3]&&r[2]<=640&&r[3]<=360;
}
inline std::array<uint32_t,8> NativePackedSecondaryStencilClearParameters(
        const std::array<int32_t,4>& r,uint32_t stencil) noexcept {
    return {uint32_t(r[0]),uint32_t(r[1]),uint32_t(r[2]),uint32_t(r[3]),stencil&255,0,0,0};
}
}
