#pragma once
#include "builtin_atomic_depth.h"
#include "content_hash.h"
namespace gow3::renderer {
// Closed proof for unchanged canonical FSI commands in one subpass. Their
// coherent ordered interlock covers all overlapping fragments in that subpass,
// across draw commands; the original shader memory barriers remain intact.
// The separately pinned atomic variant uses Device-scope maximum on the same
// words, preserving invariant stencil bits. A pass never mixes these models.
struct NativePackedDepthPassProof {
    uint32_t width=0,height=0,surface_info=0,raw_color_mask=0;
    bool pixel_shader=false,active_query=false,audited_vertex=false,audited_pixel=false;
    bool cpu_geometry=false,texture_free=false,unmodified_variants=false,atomic_depth_variant=false;
    bool discarded=false,rectangle=false,private_color_only_framebuffer=false,bounded_scissor=false;
};
inline bool NativePackedDepthPassPixelCertified(std::span<const uint8_t> code) {
    return ContentHash(code)==kAtomicDepthBuiltinSpirv;
}
inline bool NativePackedDepthPassAllowed(const NativeFsiConstants& raster,
        const NativePackedDepthPassProof& p) noexcept {
    return p.width==1280&&p.height==720&&raster.pitch==1280&&raster.depth_base==0&&
        p.audited_pixel&&p.cpu_geometry&&p.texture_free&&
        (p.unmodified_variants!=p.atomic_depth_variant)&&
        !p.discarded&&!p.rectangle&&p.private_color_only_framebuffer&&p.bounded_scissor&&
        BuiltinAtomicDepthAllowed(raster,p.width,p.height,p.raw_color_mask,p.pixel_shader,
            p.active_query,p.audited_vertex,p.surface_info);
}
// Pure addressing proof used by the fixture:720 distinct tiles, each1280
// distinct words; half-bank depth swap is a permutation, no2048tile wrap.
inline uint32_t NativePackedDepthPassWord(uint32_t x,uint32_t y) noexcept {
    return ((y/16)*16+x/80)*1280+(y%16)*80+(x%80+40)%80;
}
}
