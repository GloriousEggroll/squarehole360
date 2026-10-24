#pragma once
#include "edram_extent.h"
#include <array>
#include <cstdint>
#include <stdexcept>

namespace gow3::renderer {
inline constexpr uint32_t kNativeAtlasDepthRegionMaxWidth=160,kNativeAtlasDepthRegionMaxHeight=96;
// A closed observed transfer family. This changes the amount of source-image
// restoration only; the canonical resolve/physical packing remains unchanged.
struct NativeAtlasDepthRegionInput {
    uint32_t source_width=0,source_height=0,source_pitch=0,source_base=0,source_format=0;
    bool source_depth=false;
    uint32_t source_surface_info=0;
    uint32_t destination_width=0,destination_height=0,destination_format=0,
        destination_dimension=0,destination_layers=0,destination_endian=0,
        destination_level_count=0,destination_min_mip=0,destination_max_mip=0,
        destination_block_width=0,destination_block_height=0,destination_block_bytes=0,
        destination_pitch=0;
    bool destination_tiled=false;
    uint32_t flags=0,level=0,source_slot=0;
    int32_t exponent_bias=0;
    bool destination_swap=false;
    std::array<int32_t,4> source_rectangle{},destination_rectangle{};
};
inline bool NativeAtlasDepthRegionAllowed(const NativeAtlasDepthRegionInput& input) noexcept {
    if(!input.source_depth||input.source_width!=1120||input.source_height!=688||
       input.source_pitch!=1120||input.source_base!=1440||input.source_format!=0||
       (input.source_surface_info&0x30000u)||input.destination_width!=1120||
       input.destination_height!=688||input.destination_format!=22||
       input.destination_dimension!=1||input.destination_layers!=1||
       input.destination_endian!=2||!input.destination_tiled||
       input.destination_level_count!=1||input.destination_min_mip||input.destination_max_mip||
       input.destination_block_width!=1||input.destination_block_height!=1||
       input.destination_block_bytes!=4||input.destination_pitch!=1120||
       input.flags!=4||input.level||input.source_slot!=4||input.exponent_bias||
       input.destination_swap)return false;
    const auto in_bounds=[](const auto& rectangle) {
        return rectangle[0]>=0&&rectangle[1]>=0&&rectangle[2]>rectangle[0]&&
            rectangle[3]>rectangle[1]&&rectangle[2]<=1120&&rectangle[3]<=688;
    };
    const auto& source=input.source_rectangle;const auto& destination=input.destination_rectangle;
    return in_bounds(source)&&in_bounds(destination)&&
        int64_t(source[2])-source[0]==int64_t(destination[2])-destination[0]&&
        int64_t(source[3])-source[1]==int64_t(destination[3])-destination[1]&&
        uint32_t(source[2]-source[0])<=kNativeAtlasDepthRegionMaxWidth&&
        uint32_t(source[3]-source[1])<=kNativeAtlasDepthRegionMaxHeight&&
        // Full-source restoration retains its original cache-version behavior.
        !(source[0]==0&&source[1]==0&&source[2]==1120&&source[3]==688);
}
// Partial restoration can never certify untouched source pixels as current.
// The Scene keeps its OLD token; a later generic read must restore the whole
// image if packed writes advanced that token. No token promotion is invented.
inline constexpr uint64_t NativeAtlasPartialRestoreVersion(uint64_t previous,
                                                          uint64_t /*packed*/) noexcept {
    return previous;
}
// Words match the EXISTING resolve_memory.comp Parameters ABI exactly. The
// output image retains full1120x688 coordinates, even though only this dispatch
// rectangle is initialized. This is the production builder used by the crop.
inline std::array<uint32_t,16> NativeAtlasDepthRegionLoadParameters(uint32_t width,uint32_t height,
    uint32_t pitch,uint32_t base,uint32_t format,bool depth,const std::array<int32_t,4>& rectangle) {
    if(!depth||width!=1120||height!=688||pitch!=1120||base!=1440||format!=0||
       rectangle[0]<0||rectangle[1]<0||rectangle[2]<=rectangle[0]||rectangle[3]<=rectangle[1]||
       rectangle[2]>int32_t(width)||rectangle[3]>int32_t(height)||
       uint32_t(rectangle[2]-rectangle[0])>kNativeAtlasDepthRegionMaxWidth||
       uint32_t(rectangle[3]-rectangle[1])>kNativeAtlasDepthRegionMaxHeight)
        throw std::invalid_argument("Invalid canonical atlas source dispatch");
    return {uint32_t(rectangle[2]-rectangle[0]),uint32_t(rectangle[3]-rectangle[1]),
        uint32_t(rectangle[0]),uint32_t(rectangle[1]),0,0,(pitch+79)/80,2,3,0,
        22,0,base*5120,NativeEdramTiles()*1280,0,0};
}
} // namespace gow3::renderer
