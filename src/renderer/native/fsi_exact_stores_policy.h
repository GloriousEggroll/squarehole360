#pragma once
#include "fsi_state.h"
#include <cstdint>

namespace gow3::renderer {
inline constexpr uint32_t kNativeFsiExactDepthIdentityFlag=1u;
inline constexpr uint32_t kNativeFsiExactStoresMaximumImageExtent=8192u;
inline constexpr bool NativeFsiExactDepthIdentityControl(uint32_t control) noexcept {
    return (control&2u)!=0u&&(control&5u)==0u;
}
inline constexpr bool NativeFsiExactStoresColorFormat(uint32_t format) noexcept {
    return format==0||format==2||format==3||format==4||format==5||format==6||format==7||format==14||format==15;
}
inline constexpr bool NativeFsiExactStoresWideColor(uint32_t format) noexcept {
    return format==5||format==7||format==15;
}
inline constexpr uint64_t NativeFsiExactStoresColorMask(uint32_t format,uint32_t components) noexcept {
    uint64_t result=0;
    for(uint32_t c=0;c<4;++c)if((components&(1u<<c))!=0) {
        if(format==0)result|=uint64_t(255)<<(c*8);
        else if(format==2||format==3)result|=uint64_t(c==3?3u:1023u)<<(c*10);
        else if(format>=4&&format<=7) {
            if(c<2||format==5||format==7)result|=uint64_t(65535)<<(c*16);
        } else if(c==0||(c==1&&format==15))result|=uint64_t(0xffffffffu)<<(c*32);
    }
    return result;
}
inline constexpr bool NativeFsiExactColorOverwrite(uint32_t format,uint32_t components,uint32_t blend_enabled) noexcept {
    if(!NativeFsiExactStoresColorFormat(format)||components>15u||blend_enabled!=0)return false;
    const auto mask=NativeFsiExactStoresColorMask(format,components);
    return uint32_t(mask)==0xffffffffu&&(!NativeFsiExactStoresWideColor(format)||uint32_t(mask>>32)==0xffffffffu);
}
// Ordered pixel interlock alone does not serialize packed addresses aliased
// between different framebuffer pixels. Only grant the depth identity flag
// for a conservative injective, nonwrapping tile interval, disjoint from all
// active color writes. Padding is included intentionally; uncertain cases keep
// the original load/write behavior. Draw ordering/barriers remain unchanged.
inline bool NativeFsiExactDepthStoreNoAliases(const NativeFsiConstants& state,
    uint32_t width,uint32_t height) noexcept {
    if(!NativeFsiExactDepthIdentityControl(state.depth_control)||state.depth_format>1u||
       !width||!height||width>kNativeFsiExactStoresMaximumImageExtent||height>kNativeFsiExactStoresMaximumImageExtent||
       state.pitch<width||state.pitch>16383u||state.depth_base>=2048u)return false;
    const uint64_t rows=(uint64_t(height)+15u)/16u;
    const uint64_t pitch_tiles=(uint64_t(state.pitch)+79u)/80u;
    const uint64_t depth_end=uint64_t(state.depth_base)+rows*pitch_tiles;
    if(depth_end>2048u)return false;
    for(const auto& color:state.color)if(color.mask) {
        if(color.mask>15u||!NativeFsiExactStoresColorFormat(color.format)||color.base>=2048u)return false;
        const uint64_t color_end=uint64_t(color.base)+rows*pitch_tiles*(NativeFsiExactStoresWideColor(color.format)?2u:1u);
        if(color_end>2048u||!(depth_end<=color.base||color_end<=state.depth_base))return false;
    }
    return true;
}
} // namespace gow3::renderer
