#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace gow3::renderer {
// Refuse an unreviewed canonical helper/compiler semantic version, even if its
// generated baseline header and source layout were rebuilt consistently.
inline constexpr std::string_view kNativeFsiHoistReviewedCanonicalCacheVersion=
    "1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
enum class NativeFsiHoistMode : unsigned {Disabled=0,ImmutableState=1,ImmutableStateAndIncomingDepth=2};
inline constexpr bool NativeFsiHoistModeKnown(NativeFsiHoistMode mode) {
    return mode==NativeFsiHoistMode::Disabled||mode==NativeFsiHoistMode::ImmutableState||mode==NativeFsiHoistMode::ImmutableStateAndIncomingDepth;
}
inline constexpr std::string_view kNativeFsiImmutableStateHoistIdentity=
    "native-fsi-immutable-state-load-hoist-v1";
// Only unconditional immutable per-draw reads move. The packed memory barrier,
// previous depth/stencil values, conditional RT state reads, color packing,
// destination reads/writes and late query discard stay in their original order.
inline constexpr std::string_view kNativeFsiImmutableStateHoistOriginal=R"G3HOIST(    vk::ext_execution_mode(5366); // PixelInterlockOrderedEXT
    gow3BeginInterlock();
    DeviceMemoryBarrier();
    bool pass=nativeDepthStencil(pixel,depth,front_face);
    raster.blend_constant=vk::RawBufferLoad<float4>(address+96);
    uint blending=vk::RawBufferLoad<uint>(address+112);
    float4 colors[4]={c0,c1,c2,c3};)G3HOIST";
inline constexpr std::string_view kNativeFsiImmutableStateHoistReplacement=R"G3HOIST(    raster.blend_constant=vk::RawBufferLoad<float4>(address+96);
    uint blending=vk::RawBufferLoad<uint>(address+112);
    vk::ext_execution_mode(5366); // PixelInterlockOrderedEXT
    gow3BeginInterlock();
    DeviceMemoryBarrier();
    bool pass=nativeDepthStencil(pixel,depth,front_face);
    float4 colors[4]={c0,c1,c2,c3};)G3HOIST";
inline std::optional<std::string> TryNativeFsiImmutableStateHoistSource(std::string_view source) {
    const auto at=source.find(kNativeFsiImmutableStateHoistOriginal);
    if(at==std::string_view::npos||source.find(kNativeFsiImmutableStateHoistOriginal,at+1)!=std::string_view::npos||
       source.find(kNativeFsiImmutableStateHoistReplacement)!=std::string_view::npos)return std::nullopt;
    std::string result(source);result.replace(at,kNativeFsiImmutableStateHoistOriginal.size(),kNativeFsiImmutableStateHoistReplacement);return result;
}
inline std::optional<std::string> RestoreNativeFsiImmutableStateHoistSource(std::string_view source) {
    const auto at=source.find(kNativeFsiImmutableStateHoistReplacement);
    if(at==std::string_view::npos||source.find(kNativeFsiImmutableStateHoistReplacement,at+1)!=std::string_view::npos||
       source.find(kNativeFsiImmutableStateHoistOriginal)!=std::string_view::npos)return std::nullopt;
    std::string result(source);result.replace(at,kNativeFsiImmutableStateHoistReplacement.size(),kNativeFsiImmutableStateHoistOriginal);return result;
}
// Independent V2 source shape: compute the exact unchanged incoming packed
// code only on the same immutable depth-or-stencil enabled control. Previous
// packed depth/stencil remains a read inside the original ordered interlock.
inline constexpr std::string_view kNativeFsiStateAndIncomingDepthHoistIdentity=
    "native-fsi-immutable-state-incoming-depth-hoist-v2";
inline constexpr std::string_view kNativeFsiIncomingDepthSignatureOriginal=
    "bool nativeDepthStencil(uint2 p,float depth,bool front_face) {";
inline constexpr std::string_view kNativeFsiIncomingDepthSignatureReplacement=
    "bool nativeDepthStencil(uint2 p,uint incoming,bool front_face) {";
inline constexpr std::string_view kNativeFsiIncomingDepthCodeOriginal=
    "        uint incoming=packDepth(depth),reference_mask=front_face||(control&128u)==0?raster.stencil_front:raster.stencil_back;";
inline constexpr std::string_view kNativeFsiIncomingDepthCodeReplacement=
    "        uint reference_mask=front_face||(control&128u)==0?raster.stencil_front:raster.stencil_back;";
inline constexpr std::string_view kNativeFsiStateAndIncomingDepthHoistReplacement=R"G3HOIST(    raster.blend_constant=vk::RawBufferLoad<float4>(address+96);
    uint blending=vk::RawBufferLoad<uint>(address+112);
    uint incoming_depth=0;
    [branch] if((raster.depth_control&3u)!=0u)incoming_depth=packDepth(depth);
    vk::ext_execution_mode(5366); // PixelInterlockOrderedEXT
    gow3BeginInterlock();
    DeviceMemoryBarrier();
    bool pass=nativeDepthStencil(pixel,incoming_depth,front_face);
    float4 colors[4]={c0,c1,c2,c3};)G3HOIST";
inline bool NativeFsiHoistHasExactlyTwoDepthReferences(std::string_view source) {
    constexpr std::string_view token="nativeDepthStencil(";
    const auto first=source.find(token);
    if(first==std::string_view::npos)return false;
    const auto second=source.find(token,first+token.size());
    return second!=std::string_view::npos&&source.find(token,second+token.size())==std::string_view::npos;
}
inline std::optional<std::string> TryNativeFsiIncomingDepthHoistSource(std::string_view state_hoisted_source) {
    // The closed helper has one definition and one caller. A new caller must
    // be reviewed rather than implicitly converting its float argument.
    if(!NativeFsiHoistHasExactlyTwoDepthReferences(state_hoisted_source))return std::nullopt;
    constexpr std::string_view originals[]{kNativeFsiIncomingDepthSignatureOriginal,kNativeFsiIncomingDepthCodeOriginal,kNativeFsiImmutableStateHoistReplacement};
    constexpr std::string_view replacements[]{kNativeFsiIncomingDepthSignatureReplacement,kNativeFsiIncomingDepthCodeReplacement,kNativeFsiStateAndIncomingDepthHoistReplacement};
    for(unsigned i=0;i<3;++i) {
        const auto at=state_hoisted_source.find(originals[i]);
        if(at==std::string_view::npos||state_hoisted_source.find(originals[i],at+1)!=std::string_view::npos||
           state_hoisted_source.find(replacements[i])!=std::string_view::npos)return std::nullopt;
    }
    std::string result(state_hoisted_source);
    for(unsigned i=0;i<3;++i)result.replace(result.find(originals[i]),originals[i].size(),replacements[i]);
    return result;
}
inline std::optional<std::string> RestoreNativeFsiIncomingDepthHoistSource(std::string_view source) {
    if(!NativeFsiHoistHasExactlyTwoDepthReferences(source))return std::nullopt;
    constexpr std::string_view originals[]{kNativeFsiIncomingDepthSignatureOriginal,kNativeFsiIncomingDepthCodeOriginal,kNativeFsiImmutableStateHoistReplacement};
    constexpr std::string_view replacements[]{kNativeFsiIncomingDepthSignatureReplacement,kNativeFsiIncomingDepthCodeReplacement,kNativeFsiStateAndIncomingDepthHoistReplacement};
    for(unsigned i=0;i<3;++i) {
        const auto at=source.find(replacements[i]);
        if(at==std::string_view::npos||source.find(replacements[i],at+1)!=std::string_view::npos||
           source.find(originals[i])!=std::string_view::npos)return std::nullopt;
    }
    std::string result(source);
    for(unsigned i=0;i<3;++i)result.replace(result.find(replacements[i]),replacements[i].size(),originals[i]);
    return result;
}
inline std::optional<std::string> TryNativeFsiStateAndIncomingDepthHoistSource(std::string_view canonical_source) {
    const auto state=TryNativeFsiImmutableStateHoistSource(canonical_source);
    return state?TryNativeFsiIncomingDepthHoistSource(*state):std::nullopt;
}
inline std::optional<std::string> RestoreNativeFsiStateAndIncomingDepthHoistSource(std::string_view source) {
    const auto state=RestoreNativeFsiIncomingDepthHoistSource(source);
    return state?RestoreNativeFsiImmutableStateHoistSource(*state):std::nullopt;
}
inline std::optional<std::string> TryNativeFsiHoistSource(std::string_view source,NativeFsiHoistMode mode) {
    switch(mode) {
    case NativeFsiHoistMode::ImmutableState:return TryNativeFsiImmutableStateHoistSource(source);
    case NativeFsiHoistMode::ImmutableStateAndIncomingDepth:return TryNativeFsiStateAndIncomingDepthHoistSource(source);
    default:return std::nullopt;
    }
}
inline std::optional<std::string> RestoreNativeFsiHoistSource(std::string_view source,NativeFsiHoistMode mode) {
    switch(mode) {
    case NativeFsiHoistMode::ImmutableState:return RestoreNativeFsiImmutableStateHoistSource(source);
    case NativeFsiHoistMode::ImmutableStateAndIncomingDepth:return RestoreNativeFsiStateAndIncomingDepthHoistSource(source);
    default:return std::nullopt;
    }
}
} // namespace gow3::renderer
