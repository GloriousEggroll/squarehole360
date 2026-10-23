#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace gow3::renderer {
inline constexpr std::string_view kNativePackedOpaquePixel="49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced";
inline constexpr std::string_view kNativePackedOpaqueVertex="f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
inline constexpr std::string_view kNativePackedOpaqueIdentity="native-packed-r32uint-d32-opaque-49f2-v1";
// This source adapter is admitted only after the caller authenticates the
// exact pixel container and unchanged canonical compiler. It changes the
// final output ABI, never the guest material/texture/control-flow clauses.
inline std::optional<std::string> TryNativePackedOpaqueSource(
    std::string_view baseline,std::string_view common) {
    auto unique=[](std::string_view value,std::string_view token) {
        const auto at=value.find(token);
        return at!=value.npos&&value.find(token,at+token.size())==value.npos;
    };
    constexpr std::string_view fsi_marker="\n#ifdef GOW3_NATIVE_FSI\n";
    if(common.empty()||!unique(baseline,common)||!unique(common,fsi_marker))return std::nullopt;
    const auto fsi_at=common.find(fsi_marker);
    const auto fsi=common.substr(fsi_at+fsi_marker.size());
    constexpr std::string_view pure_begin="struct Gow3FsiRasterState {";
    constexpr std::string_view pure_end="bool nativeDepthStencil(";
    constexpr std::string_view load_store="uint loadWord(uint at) {return gow3Edram.Load(at*4);}\nvoid storeWord(uint at,uint value) {gow3Edram.Store(at*4,value);}\n";
    constexpr std::string_view address_begin="uint edramWord(";
    constexpr std::string_view address_end="uint pack7e3(";
    if(!unique(fsi,pure_begin)||!unique(fsi,pure_end)||!unique(fsi,load_store)||
       !unique(fsi,address_begin)||!unique(fsi,address_end))return std::nullopt;
    const auto first=fsi.find(pure_begin),last=fsi.find(pure_end);
    if(first>=last)return std::nullopt;
    auto pure=std::string(fsi.substr(first,last-first));
    const auto memory=pure.find(load_store);
    if(memory==pure.npos)return std::nullopt;
    pure.erase(memory,load_store.size());
    const auto addr=pure.find(address_begin),pack=pure.find(address_end);
    if(addr==pure.npos||pack==pure.npos||addr>=pack)return std::nullopt;
    pure.erase(addr,pack-addr);
    // Keep the canonical packing functions byte-for-byte. Specializing the
    // two raster formats below eliminates their unused format branches.
    if(pure.find("gow3Edram")!=pure.npos||pure.find("Interlock")!=pure.npos||
       pure.find("DeviceMemoryBarrier")!=pure.npos)return std::nullopt;
    constexpr std::string_view helper=R"G3PACK(
float gow3PackedDepthValue(uint code) {
    uint exponent=code>>20,mantissa=code&0xfffffu;
    if(exponent!=0)return asfloat(((exponent+111u)<<23)|(mantissa<<3));
    return float(mantissa)*2.910383045673370361328125e-11;
}
void gow3NativePackedOpaqueOutput(float depth,bool front_face,float4 color,float2 depth_slope,
                                  out uint packed_color,out float packed_depth) {
    uint64_t address=g_PushConstants.SharedConstants+2496;
    uint4 stencil=vk::RawBufferLoad<uint4>(address+16);
    float2 bias=front_face?asfloat(stencil.zw):float2(
        vk::RawBufferLoad<float>(address+116),vk::RawBufferLoad<float>(address+120));
    precise float slope=max(abs(depth_slope.x),abs(depth_slope.y))*bias.x;
    precise float offset=slope+bias.y;
    depth+=offset;
    depth=clamp(isnan(depth)?0.0:depth,0.0,1.0);
    raster.depth_format=1u;raster.color_format=3u;
    packed_color=packColor(color).x;
    packed_depth=gow3PackedDepthValue(packDepth(depth));
}
)G3PACK";
    const std::string candidate_common=std::string(common.substr(0,fsi_at))+"\n#ifdef GOW3_NATIVE_FSI\n"+
        pure+std::string(helper)+"\n#endif\n";
    constexpr std::string_view old_output=",\n\tout float4 oC0 : SV_Target0";
    constexpr std::string_view new_output=",\n\tout uint gow3PackedColor : SV_Target0,\n\tout float gow3PackedDepth : SV_Depth";
    constexpr std::string_view entry=")\n{\n#ifdef GOW3_NATIVE_FSI\nfloat2 gow3FsiDepthSlope = float2(ddx_coarse(iPos.z), ddy_coarse(iPos.z));\n#endif\n";
    constexpr std::string_view old_terminal="gow3NativeRasterOutput(iPos.xy, iPos.z, iFace, oC0, float4(0,0,0,0), float4(0,0,0,0), float4(0,0,0,0), 1u, false, gow3FsiDepthSlope);";
    constexpr std::string_view new_terminal="gow3NativePackedOpaqueOutput(iPos.z, iFace, oC0, gow3FsiDepthSlope, gow3PackedColor, gow3PackedDepth);";
    if(!unique(baseline,old_output)||!unique(baseline,entry)||!unique(baseline,old_terminal)||
       baseline.find("SV_Target1")!=baseline.npos||baseline.find("SV_Target2")!=baseline.npos||
       baseline.find("SV_Target3")!=baseline.npos||baseline.find("SV_Depth")!=baseline.npos)return std::nullopt;
    std::string candidate(baseline);
    candidate.replace(candidate.find(common),common.size(),candidate_common);
    candidate.replace(candidate.find(old_output),old_output.size(),new_output);
    candidate.replace(candidate.find(entry),entry.size(),std::string(entry)+"\tfloat4 oC0;\n");
    candidate.replace(candidate.find(old_terminal),old_terminal.size(),new_terminal);
    // Restoring the isolated common and three ABI edits must recover every
    // original byte, including material instructions, alpha and color scale.
    std::string restored(candidate);
    restored.replace(restored.find(candidate_common),candidate_common.size(),common);
    restored.replace(restored.find(new_output),new_output.size(),old_output);
    const std::string added_entry=std::string(entry)+"\tfloat4 oC0;\n";
    restored.replace(restored.find(added_entry),added_entry.size(),entry);
    restored.replace(restored.find(new_terminal),new_terminal.size(),old_terminal);
    if(restored!=baseline)return std::nullopt;
    return candidate;
}
}
