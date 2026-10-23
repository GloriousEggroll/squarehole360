#pragma once
#include "renderer/native/native_packed_opaque_shader.h"
namespace gow3::renderer {
// Phase-only exact-format specialization. The legacy packed adapters and their
// bytecode/cache identities remain unchanged. The runtime admits format3 only.
inline std::optional<std::string> TryNativeRetainedOpaquePhaseSource(
    std::string_view baseline, std::string_view common) {
    auto unique=[](std::string_view s,std::string_view token) {
        const auto at=s.find(token);
        return at!=s.npos && s.find(token,at+token.size())==s.npos;
    };
    // Authenticate the complete original format3 branch, including its precise
    // alpha qualifier. Unreachable format0/2 precise vector arithmetic must not
    // propagate additional contraction restrictions into this phase's material.
    constexpr std::string_view original_format3="if(f==3) {precise float a=(isnan(c.w)?0.0:clamp(c.w,0.0,1.0))*3.0;return uint2(pack7e3(c.x)|(pack7e3(c.y)<<10)|(pack7e3(c.z)<<20)|(uint(a+.5)<<30),0);}";
    if(!unique(common,original_format3))return std::nullopt;
    auto candidate=TryNativePackedOpaqueSource(baseline,common);
    if(!candidate)return std::nullopt;
    constexpr std::string_view before=R"G3PHASE(    raster.depth_format=1u;raster.color_format=3u;
    packed_color=packColor(color).x;
    packed_depth=gow3PackedDepthValue(packDepth(depth));)G3PHASE";
    constexpr std::string_view after=R"G3PHASE(    raster.depth_format=1u;raster.color_format=3u;
    precise float a=(isnan(color.w)?0.0:clamp(color.w,0.0,1.0))*3.0;
    packed_color=pack7e3(color.x)|(pack7e3(color.y)<<10)|(pack7e3(color.z)<<20)|(uint(a+.5)<<30);
    packed_depth=gow3PackedDepthValue(packDepth(depth));)G3PHASE";
    if(!unique(*candidate,before)||candidate->find(after)!=candidate->npos)return std::nullopt;
    const auto at=candidate->find(before);
    const auto legacy=*candidate;
    candidate->replace(at,before.size(),after);
    if(!unique(*candidate,after))return std::nullopt;
    auto restored=*candidate;
    restored.replace(restored.find(after),after.size(),before);
    if(restored!=legacy)return std::nullopt;
    return candidate;
}

// Separate V4 adapter. V3 and its material/color precision adaptation above are
// unchanged. Only the authenticated depth decoder and output semantic differ.
inline std::optional<std::string> TryNativeRetainedOpaquePhaseConservativeSource(
    std::string_view baseline, std::string_view common) {
    auto candidate=TryNativeRetainedOpaquePhaseSource(baseline,common);
    if(!candidate)return std::nullopt;
    auto unique=[](std::string_view s,std::string_view token) {
        const auto at=s.find(token);
        return at!=s.npos && s.find(token,at+token.size())==s.npos;
    };
    constexpr std::string_view old_depth=R"G3DEPTH(float gow3PackedDepthValue(uint code) {
    uint exponent=code>>20,mantissa=code&0xfffffu;
    if(exponent!=0)return asfloat(((exponent+111u)<<23)|(mantissa<<3));
    return float(mantissa)*2.910383045673370361328125e-11;
})G3DEPTH";
    constexpr std::string_view conservative_depth=R"G3DEPTH(float gow3PackedDepthValue(uint code) {
    if(code==0u)return asfloat(0u);
    uint lower;
    if(code<=0x100000u) {
        uint threshold=8u*code-4u+(code&1u);
        uint leading=uint(firstbithigh(threshold));
        lower=((leading+90u)<<23u)|((threshold<<(23u-leading))&0x7fffffu);
    } else {
        lower=0x38000000u+8u*code-4u+(code&1u);
    }
    return asfloat(min(lower,0x3e800001u+code));
})G3DEPTH";
    constexpr std::string_view old_output="out float gow3PackedDepth : SV_Depth";
    constexpr std::string_view new_output="out float gow3PackedDepth : SV_DepthLessEqual";
    if(!unique(*candidate,old_depth)||!unique(*candidate,old_output)||
       candidate->find(conservative_depth)!=candidate->npos||
       candidate->find(new_output)!=candidate->npos)return std::nullopt;
    const auto original=*candidate;
    candidate->replace(candidate->find(old_depth),old_depth.size(),conservative_depth);
    candidate->replace(candidate->find(old_output),old_output.size(),new_output);
    if(!unique(*candidate,conservative_depth)||!unique(*candidate,new_output))return std::nullopt;
    auto restored=*candidate;
    restored.replace(restored.find(conservative_depth),conservative_depth.size(),old_depth);
    restored.replace(restored.find(new_output),new_output.size(),old_output);
    if(restored!=original)return std::nullopt;
    return candidate;
}
} // namespace gow3::renderer
