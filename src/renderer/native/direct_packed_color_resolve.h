#pragma once
#include <array>
#include <cstdint>

namespace gow3::renderer {
inline constexpr uint64_t kNativeDirectPackedColorResolvePixels=1280ull*720ull;
struct NativeDirectPackedColorResolveInput {
    bool native_fsi=false,source_depth=false;
    uint32_t source_width=0,source_height=0,source_pitch=0,source_base=0,source_format=0,
        source_surface_info=0;
    uint32_t destination_width=0,destination_height=0,destination_format=0,
        destination_dimension=0,destination_layers=0,destination_endian=0,
        destination_level_count=0,destination_min_mip=0,destination_max_mip=0,
        destination_block_width=0,destination_block_height=0,destination_block_bytes=0,
        destination_pitch=0;
    bool destination_tiled=false,destination_swap=false;
    uint32_t flags=0,level=0,source_slot=0;
    int32_t exponent_bias=0;
    std::array<int32_t,4> source_rectangle{},destination_rectangle{};
};
// Closed observed color family only. The omitted Surface() transfer is Color,
// scale=1 and identity swizzle between two RGBA32F images. Its decoded UNORM10/2
// inputs are finite nonnegative normalized floats, not arbitrary host floats.
// DescribeTexture retains raw fetch format54; only the descriptive guest
// identity and canonical ResolveMemory shader dispatch normalize54 to7.
inline bool NativeDirectPackedColorResolveAllowed(const NativeDirectPackedColorResolveInput& i) noexcept {
    if(!i.native_fsi||i.source_depth||i.source_width!=1280||i.source_height!=720||
       i.source_pitch!=1280||i.source_base!=720||i.source_format!=2||
       (i.source_surface_info&16383u)!=1280||(i.source_surface_info&0x30000u)||
       i.destination_width!=1280||i.destination_height!=720||i.destination_format!=54||
       i.destination_dimension!=1||i.destination_layers!=1||i.destination_endian!=2||
       !i.destination_tiled||i.destination_level_count!=1||i.destination_min_mip||i.destination_max_mip||
       i.destination_block_width!=1||i.destination_block_height!=1||i.destination_block_bytes!=4||
       i.destination_pitch!=1280||i.flags||i.level||i.source_slot||i.exponent_bias||i.destination_swap)
        return false;
    return i.source_rectangle==i.destination_rectangle&&
        (i.source_rectangle==std::array<int32_t,4>{0,0,1280,720}||
         i.source_rectangle==std::array<int32_t,4>{512,320,608,384});
}
enum class NativeDirectPackedColorResolveSource {Refuse,CurrentHost,Packed};
// Observe the packed version AFTER Surface's original overlapping-alias commits.
// A clean current host may hold pre-encode floats; always retain that source.
// Never promote a stale host token or retain a decoded image across operations.
inline NativeDirectPackedColorResolveSource NativeDirectPackedColorResolveChooseSource(
    bool fully_defined,bool host_present,bool host_dirty,uint64_t host_version,
    uint64_t packed_version,bool receipt_present) noexcept {
    if(!fully_defined||!packed_version||host_dirty||receipt_present)
        return NativeDirectPackedColorResolveSource::Refuse;
    if(host_present&&host_version==packed_version)return NativeDirectPackedColorResolveSource::CurrentHost;
    if(host_present&&host_version>packed_version)return NativeDirectPackedColorResolveSource::Refuse;
    return NativeDirectPackedColorResolveSource::Packed;
}
} // namespace gow3::renderer
