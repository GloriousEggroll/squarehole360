#pragma once
#include "plume_render_interface.h"
#include "query_tracker.h"
#include <array>
#include <span>
#include <vector>

namespace gow3::renderer {
// Consumer-side prototype for single-sample packed EDRAM raster operations.
// The device MUST have fragmentStoresAndAtomics and ordered pixel interlock
// enabled. This is an independently tested epilogue, not yet a live backend.
struct FsiRasterState {
    uint32_t pitch=80,color_base=0,depth_base=1024,color_format=0;
    uint32_t color_mask=15,blend_control=0x10001,depth_control=0,depth_format=0;
    uint32_t stencil_front=0x00ffff00,stencil_back=0x00ffff00,blend_enabled=0,reserved=0;
    std::array<float,4> blend_constant{};
};
static_assert(sizeof(FsiRasterState)==64);
struct FsiFragment {std::array<float,4> color{};float depth=0;std::array<float,3> padding{};};
static_assert(sizeof(FsiFragment)==32);
class FsiPrecision {
public:
    static constexpr uint32_t kBytes=2048*5120;
    FsiPrecision(plume::RenderDevice&,plume::RenderCommandQueue&,
        std::span<const uint32_t> vertex,std::span<const uint32_t> fragment,
        bool ordered_pixel_interlock_enabled,bool production_abi=false);
    void Initialize(std::span<const uint32_t>);
    void Draw(const FsiRasterState&,std::span<const FsiFragment>,uint32_t width,uint32_t height,
              std::array<float,4> polygon_bias={});
    std::vector<uint32_t> Read();
    uint64_t passing_samples() const {return passing_samples_;}
private:
    bool production_abi_=false;
    uint64_t passing_samples_=0;
    plume::RenderDevice& device_;
    plume::RenderCommandQueue& queue_;
    std::unique_ptr<plume::RenderBuffer> storage_;
    std::unique_ptr<plume::RenderShader> vertex_,fragment_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderPipeline> pipeline_;
    std::unique_ptr<plume::RenderCommandList> commands_;
    std::unique_ptr<plume::RenderCommandFence> fence_;
    std::unique_ptr<NativeQueryTracker> query_;
    void Submit();
};
}
