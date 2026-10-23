#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gow3::renderer {
inline constexpr std::string_view kOpaqueColorOverwriteIdentity =
    "native-fsi-opaque-format3-full-word-overwrite-v1";

// These are exact, closed source shapes, not a general HLSL rewriter. The
// canonical compiler and its cache namespace stay unchanged. A separate
// experimental compiler must key its output/module/pipeline by the identity
// above and a digest of this source before selecting the candidate module.
inline constexpr std::string_view kOpaqueColorOverwriteOriginal = R"G3FUNC(void nativeColorOutput(uint2 p,float4 color) {
    if(raster.color_mask!=0) {
        bool wide=raster.color_format==5||raster.color_format==7||raster.color_format==15;
        uint at=edramWord(p,raster.color_base,false,wide);
        uint2 previous=uint2(loadWord(at),wide?loadWord(at+1):0u);
        uint2 packed=packColor(blendColor(color,unpackColor(previous))),mask=colorMask();
        packed=(previous&~mask)|(packed&mask); // Preserve noncanonical masked bytes.
        storeWord(at,packed.x);if(wide)storeWord(at+1,packed.y);
    }
})G3FUNC";

inline constexpr std::string_view kOpaqueColorOverwriteReplacement = R"G3FUNC(void nativeColorOutput(uint2 p,float4 color) {
    if(raster.color_mask!=0) {
        bool wide=raster.color_format==5||raster.color_format==7||raster.color_format==15;
        uint at=edramWord(p,raster.color_base,false,wide);
        uint2 mask=colorMask();
        // Format3 occupies one complete 32-bit packed color word. Disabled
        // blending returns the unmodified source color. The original mask
        // merge then discards every bit of the previous word. Keep the exact
        // packColor implementation, ordered interlock and target store order.
        if(raster.color_format==3u&&raster.blend_enabled==0u&&
           mask.x==0xffffffffu&&mask.y==0u) {
            storeWord(at,packColor(color).x);
            return;
        }
        uint2 previous=uint2(loadWord(at),wide?loadWord(at+1):0u);
        uint2 packed=packColor(blendColor(color,unpackColor(previous)));
        packed=(previous&~mask)|(packed&mask); // Preserve noncanonical masked bytes.
        storeWord(at,packed.x);if(wide)storeWord(at+1,packed.y);
    }
})G3FUNC";

inline std::optional<std::string> TryOpaqueColorOverwriteSource(std::string_view source) {
    const auto offset=source.find(kOpaqueColorOverwriteOriginal);
    if(offset==std::string_view::npos||
       source.find(kOpaqueColorOverwriteOriginal,offset+1)!=std::string_view::npos||
       source.find(kOpaqueColorOverwriteReplacement)!=std::string_view::npos)
        return std::nullopt;
    std::string result(source);
    result.replace(offset,kOpaqueColorOverwriteOriginal.size(),kOpaqueColorOverwriteReplacement);
    return result;
}
} // namespace gow3::renderer
