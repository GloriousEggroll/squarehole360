#pragma once
#include "packed_color_variant8_catalog.h"
#include "content_hash.h"
namespace gow3::renderer {
// Closed action tuple. This is a packed-pass certificate, not a retained-depth
// or monotonicity certificate. Original compiler bytes and state are unchanged.
inline constexpr std::string_view kPackedColorActionVertex="c2c62310fb6b4157fbb21fe9fdb8736bdb388f8ed45548565a84bf9b82f79ee3";
inline constexpr std::string_view kPackedColorActionPixel="1a70c06695ecde49f80867ce0a93e9880143ed126dd8a45af46173ccfebc50d1";
inline constexpr std::array<uint32_t,6> kPackedColorActionRaster{{3,2,1,4,0,0}};
inline constexpr bool NativePackedColorActionVariant8Tuple(std::string_view vertex,std::string_view pixel,
        const std::array<uint32_t,6>& raster,const NativeFsiStateSpecialization& state,
        int32_t color_format,int32_t depth_format) noexcept {
    return vertex==kPackedColorActionVertex&&pixel==kPackedColorActionPixel&&raster==kPackedColorActionRaster&&
        state.words==kNativePackedColorVariant8State&&color_format==20&&depth_format==-1;
}
inline bool NativePackedColorActionVariant8ModuleCertified(std::string_view source,bool pixel,
        NativeShaderVariant variant,std::span<const uint8_t> bytes) noexcept {
    if(variant!=(pixel?NativeShaderVariant::PipelineStateEpilogueV1:NativeShaderVariant::Unmodified)||
       source!=(pixel?kPackedColorActionPixel:kPackedColorActionVertex)||
       bytes.size()!=(pixel?58836u:34168u))return false;
    try {return ContentHash(bytes)==(pixel?"6e63523f5905a59681de6fdb5ccd1f6f17dde60471a4d63ab729b640d439d659":"77188561d974990b8e3990126eee2437ef7d4ecc5a94f20a33a28b375f92cff9");}
    catch(...) {return false;}
}
inline bool NativePackedColorActionVariant8PassAllowed(bool enabled,const NativeFsiConstants& raster,
        const NativePackedColorVariant8PassProof& proof) noexcept {
    return enabled&&NativePackedColorVariant8PassAllowed(raster,proof);
}
} // namespace gow3::renderer
