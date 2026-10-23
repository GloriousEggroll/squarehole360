#pragma once
#include <array>
#include <cstdint>

namespace gow3::renderer {
inline constexpr uint32_t kNativePackedPrimaryStencilClearWidth=1280;
inline constexpr uint32_t kNativePackedPrimaryStencilClearHeight=720;
inline constexpr uint32_t kNativePackedPrimaryStencilClearWords=1280*720;
inline constexpr uint32_t kNativePackedPrimaryStencilClearUnitDepth=0xf00000;
// Source provenance, actual MSAA/query state and clear flags belong to Scene.
inline bool NativePackedPrimaryStencilClearLayoutAllowed(uint32_t width,uint32_t height,
        uint32_t pitch,uint32_t base,uint32_t format,bool depth,
        const std::array<int32_t,4>& r) noexcept {
    return width==1280&&height==720&&pitch==1280&&!base&&format==1&&depth&&
        r[0]>=0&&r[1]>=0&&r[0]<r[2]&&r[1]<r[3]&&r[2]<=1280&&r[3]<=720;
}
inline std::array<uint32_t,8> NativePackedPrimaryStencilClearParameters(
        const std::array<int32_t,4>& r,uint32_t stencil) noexcept {
    return {uint32_t(r[0]),uint32_t(r[1]),uint32_t(r[2]),uint32_t(r[3]),stencil&255,0,0,0};
}
}
