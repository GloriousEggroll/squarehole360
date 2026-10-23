#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
namespace gow3::renderer {
inline constexpr uint32_t kNativeAtlasDepthClearWidth=1120,kNativeAtlasDepthClearHeight=688;
inline constexpr uint64_t kNativeAtlasDepthClearMaxReceiptBytes=32ull*1024*1024;
inline bool NativeAtlasDepthClearLayoutAllowed(uint32_t width,uint32_t height,uint32_t pitch,
    uint32_t base,uint32_t format,bool depth,const std::array<int32_t,4>& r) noexcept {
    return depth&&width==1120&&height==688&&pitch==1120&&base==1440&&format==0&&
        r[0]>=0&&r[1]>=0&&r[2]>r[0]&&r[3]>r[1]&&r[2]<=1120&&r[3]<=688;
}
// This controls an entire surface round trip, not a rectangle-only fill.
inline std::array<uint32_t,8> NativeAtlasDepthClearParameters(uint32_t width,uint32_t height,
    uint32_t pitch,uint32_t base,uint32_t format,bool depth,const std::array<int32_t,4>& r) {
    if(!NativeAtlasDepthClearLayoutAllowed(width,height,pitch,base,format,depth,r))
        throw std::invalid_argument("Unsupported canonical atlas depth clear");
    return {width,height,pitch,base,uint32_t(r[0]),uint32_t(r[1]),uint32_t(r[2]),uint32_t(r[3])};
}
} // namespace gow3::renderer
