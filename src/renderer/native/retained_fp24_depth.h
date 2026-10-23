#pragma once
#include "fsi_state.h"
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace gow3::renderer {
inline constexpr std::string_view kNativeRetainedFp24DepthBaselineVersion="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline constexpr std::string_view kNativeRetainedFp24DepthIdentity="native-retained-fp24-depth-writer-v1-zero-bias-load-store";
struct NativeRetainedFp24DepthSource {std::string_view source,spirv;};
inline constexpr std::array<NativeRetainedFp24DepthSource,2> kNativeRetainedFp24DepthSources{{
    {"a14c7ab2b53a15fd0f1a11ee1a001cd6fb2bc839aa299f3bb10c039cd2ec8665","5ca1d26332f621747b741c431e784f23199065b7abcfbc663121901be2746c2e"},
    {"3f771dd73ee00c6649fb5356ed20dd677ad74156f8505b61e67315be642e15c5","55e3044c9832a75c74107e40a9dcb35b6e93950fb6648074fbd3e5980715a781"}
}};
inline std::optional<std::size_t> NativeRetainedFp24DepthSourceIndex(std::string_view source) noexcept {
    for(std::size_t i=0;i<kNativeRetainedFp24DepthSources.size();++i)if(source==kNativeRetainedFp24DepthSources[i].source)return i;
    return {};
}
inline bool NativeRetainedFp24DepthRaster(const std::array<uint32_t,6>& raster) noexcept {
    return raster[0]==3&&(raster[1]==1||raster[1]==2)&&raster[2]==1&&raster[3]==4&&!raster[4]&&!raster[5];
}
// Every actual draw rechecks final decoded Native state. Source/compiled module,
// CPU-owned geometry and immutable pipeline residency are separate Scene gates.
inline bool NativeRetainedFp24DepthAllowed(const NativeFsiConstants& fsi,uint32_t width,uint32_t height,
        uint32_t surface_info,uint32_t raw_mask,bool pixel_present,std::string_view vertex,
        const std::array<uint32_t,6>& raster,float viewport_min,float viewport_max) noexcept {
    if(!NativeRetainedFp24DepthSourceIndex(vertex)||pixel_present||raw_mask||!NativeRetainedFp24DepthRaster(raster)||
       width!=1280||height!=720||fsi.pitch!=1280||fsi.depth_base||fsi.depth_format!=1||
       fsi.depth_control!=0x700766||(surface_info&0x30000u)||viewport_min!=1.f||viewport_max!=0.f||
       fsi.front_slope_bias!=0.f||fsi.front_constant_bias!=0.f||fsi.back_slope_bias!=0.f||fsi.back_constant_bias!=0.f)return false;
    for(const auto& color:fsi.color)if(color.mask)return false;
    return true;
}
}
