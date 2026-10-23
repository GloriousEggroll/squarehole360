#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace gow3::renderer {
inline constexpr std::string_view kNativeFsiExactStoresIdentity="native-fsi-exact-identity-stores-v1";
inline constexpr std::string_view kNativeFsiExactStoresBaseline="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline constexpr std::string_view kNativeFsiExactStoresDepthOriginal="        storeWord(at,(packed_depth<<8)|stencil);";
inline constexpr std::string_view kNativeFsiExactStoresDepthReplacement=R"G3FUNC(        // The host flag proves injective depth addresses and disjoint color
        // writes. With stencil and depth writes off, packed_depth=previous>>8
        // and stencil=previous&255: bit identity, not float decode/repack.
        if((raster.reserved&1u)==0u||(control&5u)!=0u)
            storeWord(at,(packed_depth<<8)|stencil);)G3FUNC";
inline constexpr std::string_view kNativeFsiExactStoresFlagOriginal="    uint2 pixel=uint2(position);";
inline constexpr std::string_view kNativeFsiExactStoresFlagReplacement=R"G3FUNC(    // Immutable existing ABI word; canonical shaders do not consume it.
    raster.reserved=vk::RawBufferLoad<uint>(address+124);
    uint2 pixel=uint2(position);)G3FUNC";
inline constexpr std::string_view kNativeFsiExactStoresColorOriginal=R"G3FUNC(void nativeColorOutput(uint2 p,float4 color) {
    if(raster.color_mask!=0) {
        bool wide=raster.color_format==5||raster.color_format==7||raster.color_format==15;
        uint at=edramWord(p,raster.color_base,false,wide);
        uint2 previous=uint2(loadWord(at),wide?loadWord(at+1):0u);
        uint2 packed=packColor(blendColor(color,unpackColor(previous))),mask=colorMask();
        packed=(previous&~mask)|(packed&mask); // Preserve noncanonical masked bytes.
        storeWord(at,packed.x);if(wide)storeWord(at+1,packed.y);
    }
})G3FUNC";
inline constexpr std::string_view kNativeFsiExactStoresColorReplacement=R"G3FUNC(void nativeColorOutput(uint2 p,float4 color) {
    if(raster.color_mask!=0) {
        bool wide=raster.color_format==5||raster.color_format==7||raster.color_format==15;
        uint at=edramWord(p,raster.color_base,false,wide);
        uint2 mask=colorMask();
        // Disabled blending returns the unmodified source. Full masks discard
        // every old destination bit; keep exact original pack/store ordering.
        if(raster.blend_enabled==0u&&mask.x==0xffffffffu&&
           (!wide||mask.y==0xffffffffu)) {
            uint2 packed=packColor(color);
            storeWord(at,packed.x);if(wide)storeWord(at+1,packed.y);
            return;
        }
        uint2 previous=uint2(loadWord(at),wide?loadWord(at+1):0u);
        uint2 packed=packColor(blendColor(color,unpackColor(previous)));
        packed=(previous&~mask)|(packed&mask); // Preserve noncanonical masked bytes.
        storeWord(at,packed.x);if(wide)storeWord(at+1,packed.y);
    }
})G3FUNC";
inline std::optional<std::string> TryNativeFsiExactStoresSource(std::string_view source) {
    constexpr std::string_view originals[]{kNativeFsiExactStoresDepthOriginal,kNativeFsiExactStoresFlagOriginal,kNativeFsiExactStoresColorOriginal};
    constexpr std::string_view replacements[]{kNativeFsiExactStoresDepthReplacement,kNativeFsiExactStoresFlagReplacement,kNativeFsiExactStoresColorReplacement};
    for(unsigned i=0;i<3;++i) {
        const auto offset=source.find(originals[i]);
        if(offset==std::string_view::npos||source.find(originals[i],offset+1)!=std::string_view::npos||
           source.find(replacements[i])!=std::string_view::npos)return std::nullopt;
    }
    std::string result(source);
    for(unsigned i=0;i<3;++i)result.replace(result.find(originals[i]),originals[i].size(),replacements[i]);
    return result;
}
inline std::optional<std::string> RestoreNativeFsiExactStoresSource(std::string_view source) {
    constexpr std::string_view originals[]{kNativeFsiExactStoresDepthOriginal,kNativeFsiExactStoresFlagOriginal,kNativeFsiExactStoresColorOriginal};
    constexpr std::string_view replacements[]{kNativeFsiExactStoresDepthReplacement,kNativeFsiExactStoresFlagReplacement,kNativeFsiExactStoresColorReplacement};
    for(unsigned i=0;i<3;++i) {
        const auto offset=source.find(replacements[i]);
        if(offset==std::string_view::npos||source.find(replacements[i],offset+1)!=std::string_view::npos)return std::nullopt;
    }
    std::string result(source);
    for(unsigned i=0;i<3;++i)result.replace(result.find(replacements[i]),replacements[i].size(),originals[i]);
    return result;
}
} // namespace gow3::renderer
