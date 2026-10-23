#pragma once
#include "readonly_depth_attachment.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace gow3::renderer {
inline constexpr std::string_view kNativeRetainedDisjointQueryIdentity =
    "native-retained-primary-disjoint-query-v1-base1440";
inline constexpr std::array<uint32_t,6> kNativeRetainedDisjointQueryRaster{3,2,1,4,0,0};
// Complete conservative FP24 single-sample tile footprint. Each tile is5120B.
//640x360/pitch640 covers8 columns x23 rows, starting at1440, with no wrap.
// It is disjoint from primary depth[0,720) and packed color[720,1440).
inline constexpr std::array<uint32_t,2> kNativeRetainedDisjointQueryTiles{1440,1624};
static_assert(kNativeRetainedDisjointQueryTiles[0]>=1440&&kNativeRetainedDisjointQueryTiles[1]<=2048);
// Resource/device ownership, exact primary page tokens and actual resident D32
// shader identity are mandatory additional Scene gates. This is neither a
// general surface-alias certificate nor permission to keep arbitrary shaders.
inline bool NativeRetainedDisjointQueryAllowed(const NativeFsiConstants& fsi,
    uint32_t width,uint32_t height,uint32_t surface_info,uint32_t raw_mask,
    bool pixel_present,bool active_query,bool audited_vertex,std::string_view vertex,
    const std::array<uint32_t,6>& raster,float viewport_min,float viewport_max,
    bool cpu_geometry,bool no_textures) noexcept {
    return vertex==kReadonlyQueryVertexSource&&raster==kNativeRetainedDisjointQueryRaster&&
        cpu_geometry&&no_textures&&width==640&&height==360&&fsi.pitch==640&&
        fsi.depth_base==1440&&fsi.depth_format==1&&
        NativeReadonlyDepthAttachmentEligible(fsi,width,height,raw_mask,pixel_present,
            active_query,audited_vertex,viewport_min,viewport_max,surface_info);
}
}
