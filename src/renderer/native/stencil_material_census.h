#pragma once
#include "stencil_material_cutoff.h"
#include "plume_render_interface.h"
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace gow3::renderer {
// One physical DrawSlot owns this resource until its exact existing fence.
// No shared counter, early host read, wait, query manipulation or 64-bit atomic.
class NativeStencilMaterialCounterSlot {
public:
    NativeStencilMaterialCounterSlot(plume::RenderDevice&);
    plume::RenderBuffer& buffer(){return *gpu_;}
    void Prepare(uint64_t token,uint64_t event,uint64_t frame);
    void Begin(plume::RenderCommandList&);
    void End(plume::RenderCommandList&);
    void Accept(uint64_t token);
    nlohmann::json Retire(uint64_t token,uint64_t submission);
    bool pending()const noexcept{return owner_.pending();}
private:
    std::unique_ptr<plume::RenderBuffer> gpu_,readback_;
    plume::RenderDevice* device_=nullptr;
    NativeStencilMaterialCounterOwnership owner_;
    uint64_t token_=0,event_=0,frame_=0;
};
} // namespace gow3::renderer
