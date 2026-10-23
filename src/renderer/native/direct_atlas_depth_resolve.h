#pragma once
#include "atlas_depth_region.h"

namespace gow3::renderer {
inline constexpr uint64_t kNativeDirectAtlasDepthResolvePixels=1120ull*688ull;
struct NativeDirectAtlasDepthResolveInput : NativeAtlasDepthRegionInput {
    bool native_fsi=false,query_active=false;
};
// Full, initialized RGBA source only. This preserves the original resolve
// shader and BOTH resolve/store depth quantizers; it does not crop a decode.
inline bool NativeDirectAtlasDepthResolveAllowed(const NativeDirectAtlasDepthResolveInput& input) noexcept {
    if(!input.native_fsi||input.query_active||!input.source_depth||
       input.source_width!=1120||input.source_height!=688||input.source_pitch!=1120||
       input.source_base!=1440||input.source_format!=0||
       (input.source_surface_info&16383u)!=input.source_pitch||(input.source_surface_info&0x30000u)||
       input.destination_width!=1120||input.destination_height!=688||input.destination_format!=22||
       input.destination_dimension!=1||input.destination_layers!=1||input.destination_endian!=2||
       !input.destination_tiled||input.destination_level_count!=1||input.destination_min_mip||
       input.destination_max_mip||input.destination_block_width!=1||input.destination_block_height!=1||
       input.destination_block_bytes!=4||input.destination_pitch!=1120||input.flags!=4||input.level||
       input.source_slot!=4||input.exponent_bias||input.destination_swap)return false;
    const auto valid=[](const auto& r) {
        return r[0]>=0&&r[1]>=0&&r[2]>r[0]&&r[3]>r[1]&&r[2]<=1120&&r[3]<=688;
    };
    const auto& a=input.source_rectangle;const auto& b=input.destination_rectangle;
    return valid(a)&&valid(b)&&int64_t(a[2])-a[0]==int64_t(b[2])-b[0]&&
        int64_t(a[3])-a[1]==int64_t(b[3])-b[1];
}
enum class NativeDirectAtlasDepthResolveSource {Refuse,CurrentHost,Packed,Receipt};
// Observe versions only AFTER the original dirty overlapping alias commits.
// A current host can preserve floats different from Decode(packed), and must
// remain on its D32 path. A current pre-encode receipt preserves those exact
// floats too. Fresh packed decode is valid only for absent/stale clean hosts.
inline NativeDirectAtlasDepthResolveSource NativeDirectAtlasDepthResolveChooseSource(
    bool fully_defined,bool host_present,bool host_dirty,uint64_t host_version,
    uint64_t packed_version,bool receipt_present,uint64_t receipt_version) noexcept {
    if(!fully_defined||!packed_version||host_dirty)return NativeDirectAtlasDepthResolveSource::Refuse;
    if(receipt_present&&receipt_version&&receipt_version==packed_version)
        return NativeDirectAtlasDepthResolveSource::Receipt;
    if(host_present&&host_version==packed_version)return NativeDirectAtlasDepthResolveSource::CurrentHost;
    return NativeDirectAtlasDepthResolveSource::Packed;
}
} // namespace gow3::renderer
