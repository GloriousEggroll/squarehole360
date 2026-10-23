#pragma once
#include "fsi_state.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace gow3::renderer {
// General FSI draw pass groups. Ordered pixel interlock orders overlapping
// fragments at the same pixel across draws in one subpass, so consecutive FSI
// draws need no EDRAM barrier while every EDRAM word maps to one pixel only.
// A draw's planes (active color targets and depth) are tile ranges modulo the
// 2048-tile EDRAM. Draws share a pass only with the same pitch, and each plane
// either identical to a pass plane (same base, kind and width) or disjoint
// from all of them. Any other change closes the pass with full barriers.
inline constexpr uint32_t kNativeEdramTiles=2048;
struct NativeFsiPlane {
    uint32_t base=0,tiles=0;
    bool depth=false,wide=false;
    bool SameMapping(const NativeFsiPlane& o) const noexcept {return base==o.base&&depth==o.depth&&wide==o.wide;}
};
struct NativeFsiPassPlanes {
    uint32_t pitch=0,count=0;
    std::array<NativeFsiPlane,5> planes{};
};
inline bool NativeFsiFormatWide(uint32_t format) noexcept {return format==5||format==7||format==15;}
inline bool NativeFsiPlanesOverlap(const NativeFsiPlane& a,const NativeFsiPlane& b) noexcept {
    // Half-open ranges on a circle of kNativeEdramTiles tiles.
    const uint32_t offset=(b.base+kNativeEdramTiles-a.base)%kNativeEdramTiles;
    return offset<a.tiles||(kNativeEdramTiles-offset)%kNativeEdramTiles<b.tiles;
}
inline bool NativeFsiPlanesDisjoint(const NativeFsiPassPlanes& p) noexcept {
    for(uint32_t i=0;i<p.count;++i)for(uint32_t j=i+1;j<p.count;++j)
        if(NativeFsiPlanesOverlap(p.planes[i],p.planes[j]))return false;
    return true;
}
// Depth is always included: the epilogue may read it whatever the enables.
inline bool NativeFsiPassPlanesFor(const NativeFsiConstants& raster,uint32_t height,NativeFsiPassPlanes& out) noexcept {
    out={};
    if(!raster.pitch||!height||raster.pitch>8192||height>8192)return false;
    out.pitch=raster.pitch;
    const uint32_t rows=(height+15)/16;
    auto add=[&](uint32_t base,bool depth,bool wide) {
        const uint32_t tiles=rows*((raster.pitch*(wide?2:1)+79)/80);
        if(tiles>=kNativeEdramTiles||base>=kNativeEdramTiles)return false;
        out.planes[out.count++]={base,tiles,depth,wide};
        return true;
    };
    if(!add(raster.depth_base,true,false))return false;
    for(const auto& color:raster.color)
        if(color.mask&&!add(color.base,false,NativeFsiFormatWide(color.format)))return false;
    return NativeFsiPlanesDisjoint(out);
}
// Merges a disjoint draw plane set into an open pass; false means a boundary.
inline bool NativeFsiPassJoin(const NativeFsiPassPlanes& pass,const NativeFsiPassPlanes& draw,NativeFsiPassPlanes& merged) noexcept {
    if(pass.pitch!=draw.pitch)return false;
    merged=pass;
    for(uint32_t i=0;i<draw.count;++i) {
        const auto& plane=draw.planes[i];
        auto* same=std::find_if(merged.planes.begin(),merged.planes.begin()+merged.count,
            [&](const NativeFsiPlane& p){return p.SameMapping(plane);});
        if(same!=merged.planes.begin()+merged.count)same->tiles=std::max(same->tiles,plane.tiles);
        else if(merged.count==merged.planes.size())return false;
        else merged.planes[merged.count++]=plane;
    }
    return NativeFsiPlanesDisjoint(merged);
}
}
