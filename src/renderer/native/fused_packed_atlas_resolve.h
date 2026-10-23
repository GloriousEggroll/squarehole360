#pragma once
#include "plume_render_interface.h"
#include <array>
#include <memory>
#include <span>
namespace gow3::renderer {
class NativeTransferBatch;
// Only same-coordinate captured rectangles. Scene additionally proves the
// complete direct-atlas layout, packed provenance, query and alias conditions.
inline bool NativeFusedPackedAtlasRectangleAllowed(const std::array<int32_t,4>& a,
                                                   const std::array<int32_t,4>& b) noexcept {
    if(a!=b||a[0]||a[1])return false;
    return (a[2]==544&&(a[3]==352||a[3]==288))||(a[2]==96&&a[3]==96)||
        (a[2]==160&&(a[3]==96||a[3]==64));
}
// Read-only canonical packed decode + original UNORM24 resolve quantizer.
// The second MRT is a packet-owned stencil carrier, NOT a host depth receipt.
// Caller retains source buffer, destination, this object and its device until
// the covering fence; the returned scratch must be retained by every later
// packet consuming it. Only the supplied rectangle is initialized in scratch.
class NativeFusedPackedAtlasResolve {
public:
    NativeFusedPackedAtlasResolve(plume::RenderDevice&,plume::RenderCommandQueue&,
        std::span<const uint32_t> vertex,std::span<const uint32_t> pixel,NativeTransferBatch*);
    bool supported() const noexcept {return bool(pipeline_);}
    // Requires an active transfer batch. false is possible only before recording
    // any new MRT command. In addition to the five Scene rectangles, the primitive
    // admits the full exact atlas for exhaustive GPU codec validation. After accepting
    // MRT work, callers MUST complete the unchanged physical depth/stencil store.
    bool Resolve(plume::RenderBuffer& packed,plume::RenderTexture& destination,
        const plume::RenderRect& rectangle,std::shared_ptr<plume::RenderTexture>& stencil);
private:
    plume::RenderDevice& device_;
    NativeTransferBatch* batch_;
    std::unique_ptr<plume::RenderShader> vertex_,pixel_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderPipeline> pipeline_;
};
}
