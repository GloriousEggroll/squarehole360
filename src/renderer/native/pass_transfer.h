#pragma once
#include "plume_render_interface.h"
#include "retired_descriptor_pool.h"
#include <map>
#include <span>

namespace plume { class VulkanFramebufferRenderPassCache; }

namespace gow3::renderer {
class NativeTransferBatch;
struct NativeSynchronousGpuTimingContext;
class NativeSynchronousGpuTimingScope;
// Native GPU transfers between rendered surfaces and sampled textures. The
// validation executor waits for each submission; callers retain all images.
// No guest physical memory readback or Xenos command translation is involved.
class PassTransfer {
public:
    enum class Conversion : uint32_t { Color, DepthUNorm24, DepthFloat24, UNorm10, Float7e3ToUNorm10, UNorm10ToFloat7e3,
                                      UNorm8ToUNorm10, UNorm10ToUNorm8, UNorm8 };
    PassTransfer(plume::RenderDevice& device, plume::RenderCommandQueue& queue,
                 std::span<const uint32_t> vertex, std::span<const uint32_t> pixel,
                 std::span<const uint32_t> depth_pixel={}, NativeTransferBatch* batch=nullptr,
                 std::span<const uint32_t> stencil_export_pixel={});
    ~PassTransfer();
    using DescriptorPool=RetiredDescriptorPool<plume::RenderDescriptorSet>;
    // Startup-only. Reuse backings only after their immutable packet lease retires.
    void SetDescriptorPoolEnabled(bool enabled);
    DescriptorPool::Statistics DescriptorPoolStatistics() const;
    struct RenderPassStatistics {
        bool enabled=false;
        uint64_t requests=0,hits=0,misses=0,creations=0;
        uint64_t unsupported_bypasses=0,capacity_bypasses=0,create_failures=0;
        uint64_t cached_entries=0,maximum_entries=16;
    };
    void SetRenderPassCacheEnabled(bool enabled);
    RenderPassStatistics RenderPassCacheStatistics() const;
    void Clear(plume::RenderTexture* color, plume::RenderTexture* depth,
               bool clear_color, bool clear_depth, bool clear_stencil,
               const plume::RenderRect& rectangle, const plume::RenderColor& value,
               float depth_value, uint32_t stencil_value = 0);
    void Resolve(plume::RenderTexture& source, plume::RenderTexture& destination,
                 const plume::RenderRect& source_rectangle, const plume::RenderRect& destination_rectangle,
                 int32_t exponent_bias = 0, Conversion conversion = Conversion::Color,
                 uint32_t channel_swizzle = 0x688,
                 const NativeSynchronousGpuTimingContext* presentation_timing=nullptr);
    // Input R is depth, A is stencil/255. The optional export module may be
    // supplied only after the caller proves its device enabled the extension.
    // Empty/failed optional module retains the portable nine-draw Native path.
    // A nonnull region requires every input texel in that rectangle to be
    // initialized. Neither stencil nor depth outside it is read or overwritten.
    void RestoreDepth(plume::RenderTexture& source,plume::RenderTexture& destination,
        const plume::RenderRect* region=nullptr);
    struct RestoreStatistics {
        uint64_t calls=0,legacy_calls=0,export_calls=0,draws=0,stencil_clears=0,export_failures=0;
    };
    const RestoreStatistics& restore_statistics() const {return restore_statistics_;}
private:
    NativeTransferBatch* batch_;
    plume::RenderDevice& device_;
    plume::RenderCommandQueue& queue_;
    std::unique_ptr<plume::RenderCommandList> commands_;
    std::unique_ptr<plume::RenderCommandFence> fence_;
    std::unique_ptr<plume::RenderShader> vertex_, pixel_, depth_pixel_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::map<plume::RenderFormat, std::unique_ptr<plume::RenderPipeline>> pipelines_;
    std::array<std::unique_ptr<plume::RenderPipeline>,9> depth_pipelines_;
    std::unique_ptr<plume::RenderShader> stencil_export_pixel_;
    std::unique_ptr<plume::RenderPipeline> stencil_export_pipeline_;
    RestoreStatistics restore_statistics_;
    bool stencil_export_failed_=false;
    std::unique_ptr<DescriptorPool> descriptor_pool_;
    uint64_t descriptor_synchronous_bypasses_=0;
    bool descriptor_pool_configured_=false,descriptor_dispatch_started_=false;
    std::unique_ptr<plume::VulkanFramebufferRenderPassCache> render_pass_cache_;
    bool render_pass_cache_configured_=false,render_pass_dispatch_started_=false;
    std::unique_ptr<plume::RenderFramebuffer> CreateFramebuffer(const plume::RenderFramebufferDesc& desc);
    std::unique_ptr<plume::RenderDescriptorSet> AcquireDescriptor(
        std::shared_ptr<plume::RenderDescriptorSet>& lease);
    bool PrepareStencilExport();
    void Submit(NativeSynchronousGpuTimingScope* presentation_scope=nullptr);
};
}
