#pragma once
#include "content_hash.h"
#include "fsi_state.h"
#include "retained_opaque_catalog.h"

namespace gow3::renderer {
// These are the unchanged compiler outputs, not the retained-attachment
// variants. The fixture audits the original coherent epilogues and vertex
// stores. A compiler change must be reviewed before it can join this cohort.
inline bool NativePackedColorModuleCertified(std::string_view source, bool pixel,
                                             std::span<const uint8_t> code) {
    struct Pin {std::string_view source,spirv;size_t bytes;};
    static constexpr Pin vertices[]{
        {kRetainedOpaqueCatalogPairs[0].vertex,"1e5be1eb3e3a59d19f33d96dccd4539ff1cc12d9027fe671188964e3f76ab999",35272},
        {kRetainedOpaqueCatalogPairs[1].vertex,"34a3d98d8385d2ab97d1fd366ab0136a29b0a2cf81e2711a4b24469d7ba125df",33808}};
    static constexpr Pin pixels[]{
        {kRetainedOpaqueCatalogPairs[0].pixel,"8dbb0ddbef881cd8bdeab0a61d4b80493095799d2ef62be10411bec0269f3a54",109664},
        {kRetainedOpaqueCatalogPairs[1].pixel,"4c590ab0717edce95b173114ae12848857e0232f964a12f83aa6f0bedce6d42a",84116},
        {kRetainedOpaqueCatalogPairs[2].pixel,"96e0370ac9d536b4d43bb112527e891554e8ba3f6f25b14e108646056c37a107",74552}};
    for(const auto& pin:pixel?std::span<const Pin>(pixels):std::span<const Pin>(vertices))
        if(source==pin.source)return code.size()==pin.bytes&&ContentHash(code)==pin.spirv;
    return false;
}
struct NativePackedColorPassProof {
    bool audited_pair=false,unmodified=false,cpu_geometry=false;
    bool active_query=false,discarded=false,rectangle=false,private_framebuffer=false;
    bool bounded_scissor=false,primary_surfaces=false;
};
inline bool NativePackedColorPassAllowed(const NativeFsiConstants& raster,
                                         const NativePackedColorPassProof& p) noexcept {
    if(!p.audited_pair||!p.unmodified||!p.cpu_geometry||p.active_query||p.discarded||
       p.rectangle||!p.private_framebuffer||!p.bounded_scissor||!p.primary_surfaces||
       raster.pitch!=1280||raster.depth_base!=0||raster.depth_format!=1||
       raster.depth_control!=0x700766||raster.color[0].base!=720||
       raster.color[0].format!=3||raster.color[0].mask!=15)return false;
    for(unsigned i=1;i<4;++i)if(raster.color[i].mask)return false;
    return true;
}
// Format3 is one32-bit word per pixel. The two disjoint720-tile planes
// contain no wrap or cross-pixel aliases. Depth's bank swap is bijective.
inline uint32_t NativePackedColorPassWord(uint32_t x,uint32_t y,bool depth) noexcept {
    return ((depth?0:720)+(y/16)*16+x/80)*1280+(y%16)*80+
        (depth?(x%80+40)%80:x%80);
}
}
