#pragma once
// Optional Native FSI pipeline-state specialization; default disabled.
// Values specialize only the unchanged Native FSI raster epilogue.
#include "renderer/native/fsi_state.h"
#include <array>
#include <cstdint>
#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace gow3::renderer {
inline constexpr uint32_t kFsiStateMaxPipelines=256;
inline constexpr char kFsiStateSpecializationIdentity[] = "native-fsi-pso-state-specialization-v1";
struct NativeFsiStateSpecialization {
    // Values are taken from the exact final NativeFsiConstants used by this draw.
    // Never use host dummy attachment formats in place of guest packed formats.
    std::array<uint32_t, 15> words{};
    auto operator<=>(const NativeFsiStateSpecialization&) const = default;
    static NativeFsiStateSpecialization From(const NativeFsiConstants& raster) {
        NativeFsiStateSpecialization result;
        result.words[0]=raster.depth_control;
        result.words[1]=raster.depth_format;
        result.words[2]=raster.blend_enabled;
        for (uint32_t target=0;target<4;++target) {
            result.words[3+target*3]=raster.color[target].format;
            result.words[4+target*3]=raster.color[target].mask;
            result.words[5+target*3]=raster.color[target].blend;
        }
        return result;
    }
};
inline constexpr std::string_view kFsiSpecOriginal0 = R"G3SPEC(static Gow3FsiRasterState raster;
)G3SPEC";
inline constexpr std::string_view kFsiSpecReplacement0 = R"G3SPEC(static Gow3FsiRasterState raster;
// Experimental PSO-only state specialization. ID0 remains guest alpha test.
[[vk::constant_id(32)]] const uint gow3FsiSpecEnable = 0u;
[[vk::constant_id(33)]] const uint gow3FsiSpecDepthControl = 0u;
[[vk::constant_id(34)]] const uint gow3FsiSpecDepthFormat = 0u;
[[vk::constant_id(35)]] const uint gow3FsiSpecBlendEnabled = 0u;
[[vk::constant_id(36)]] const uint gow3FsiSpecColorFormat0 = 0u;
[[vk::constant_id(37)]] const uint gow3FsiSpecColorMask0 = 0u;
[[vk::constant_id(38)]] const uint gow3FsiSpecBlendControl0 = 0u;
[[vk::constant_id(39)]] const uint gow3FsiSpecColorFormat1 = 0u;
[[vk::constant_id(40)]] const uint gow3FsiSpecColorMask1 = 0u;
[[vk::constant_id(41)]] const uint gow3FsiSpecBlendControl1 = 0u;
[[vk::constant_id(42)]] const uint gow3FsiSpecColorFormat2 = 0u;
[[vk::constant_id(43)]] const uint gow3FsiSpecColorMask2 = 0u;
[[vk::constant_id(44)]] const uint gow3FsiSpecBlendControl2 = 0u;
[[vk::constant_id(45)]] const uint gow3FsiSpecColorFormat3 = 0u;
[[vk::constant_id(46)]] const uint gow3FsiSpecColorMask3 = 0u;
[[vk::constant_id(47)]] const uint gow3FsiSpecBlendControl3 = 0u;
)G3SPEC";
inline constexpr std::string_view kFsiSpecOriginal1 = R"G3SPEC(    raster.pitch=ds.x;raster.depth_base=ds.y;raster.depth_control=ds.z;raster.depth_format=ds.w;
)G3SPEC";
inline constexpr std::string_view kFsiSpecReplacement1 = R"G3SPEC(    raster.pitch=ds.x;raster.depth_base=ds.y;raster.depth_control=ds.z;raster.depth_format=ds.w;
    if(gow3FsiSpecEnable!=0u) {
        raster.depth_control=gow3FsiSpecDepthControl;raster.depth_format=gow3FsiSpecDepthFormat;
    }
)G3SPEC";
inline constexpr std::string_view kFsiSpecOriginal2 = R"G3SPEC(    uint blending=vk::RawBufferLoad<uint>(address+112);
)G3SPEC";
inline constexpr std::string_view kFsiSpecReplacement2 = R"G3SPEC(    uint blending=vk::RawBufferLoad<uint>(address+112);
    if(gow3FsiSpecEnable!=0u)blending=gow3FsiSpecBlendEnabled;
)G3SPEC";
inline constexpr std::string_view kFsiSpecOriginal3 = R"G3SPEC(        raster.blend_control=rt.w;raster.blend_enabled=(blending>>target)&1u;
)G3SPEC";
inline constexpr std::string_view kFsiSpecReplacement3 = R"G3SPEC(        raster.blend_control=rt.w;raster.blend_enabled=(blending>>target)&1u;
        if(gow3FsiSpecEnable!=0u) {
            raster.color_format=target==0u?gow3FsiSpecColorFormat0:target==1u?gow3FsiSpecColorFormat1:target==2u?gow3FsiSpecColorFormat2:gow3FsiSpecColorFormat3;
            raster.color_mask=target==0u?gow3FsiSpecColorMask0:target==1u?gow3FsiSpecColorMask1:target==2u?gow3FsiSpecColorMask2:gow3FsiSpecColorMask3;
            raster.blend_control=target==0u?gow3FsiSpecBlendControl0:target==1u?gow3FsiSpecBlendControl1:target==2u?gow3FsiSpecBlendControl2:gow3FsiSpecBlendControl3;
        }
)G3SPEC";
inline std::optional<std::string> TryFsiStateSpecializationSource(std::string_view canonical) {
    std::string result(canonical);
    const auto replace = [&](std::string_view before, std::string_view after) {
        const auto at=result.find(before);
        if (at==std::string::npos || result.find(before,at+before.size())!=std::string::npos ||
            result.find(after)!=std::string::npos) return false;
        result.replace(at,before.size(),after);
        return true;
    };
    if (!replace(kFsiSpecOriginal0,kFsiSpecReplacement0)) return std::nullopt;
    if (!replace(kFsiSpecOriginal1,kFsiSpecReplacement1)) return std::nullopt;
    if (!replace(kFsiSpecOriginal2,kFsiSpecReplacement2)) return std::nullopt;
    if (!replace(kFsiSpecOriginal3,kFsiSpecReplacement3)) return std::nullopt;
    return result;
}
}
