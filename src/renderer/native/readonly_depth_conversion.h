#pragma once
#include "readonly_depth_version.h"
#include "plume_render_interface.h"
#include <memory>
#include <atomic>
#include <span>
namespace gow3::renderer {
class NativeTransferBatch;
struct SurfaceLayout;
struct NativeReadonlyDepthGeneration {
    // Framebuffer dies before its image. DrawSlots retain the whole generation;
    // conversion packets independently retain the same budgeted image owner.
    std::shared_ptr<plume::RenderTexture> image;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    NativeReadonlyDepthVersion version;
    bool read_only_pass=false;
    NativeReadonlyDepthGeneration(uint64_t generation,NativeReadonlyDepthLayoutKey key,uint64_t source)
        :version(generation,key,source) {}
};
class NativeReadonlyDepthConversion {
public:
    static constexpr uint64_t kMaximumRetainedImageBytes=32u*1024*1024;
    struct Budget {std::atomic<uint64_t> bytes{0},peak{0};};
    struct BudgetSnapshot {uint64_t bytes=0,peak=0;};
    struct Prepared {
        plume::RenderBuffer* source=nullptr; // Scene-owned EDRAM; never a guest/CPU span.
        std::shared_ptr<NativeReadonlyDepthGeneration> generation;
        std::unique_ptr<plume::RenderBuffer> scratch;
        std::unique_ptr<plume::RenderDescriptorSet> descriptors;
        uint64_t scratch_bytes=0,image_bytes=0;
    };
    struct Statistics {uint64_t preparations=0,conversions=0,budget_refusals=0,allocation_refusals=0,scratch_bytes=0;};
    NativeReadonlyDepthConversion(plume::RenderDevice&,std::span<const uint32_t> compute,bool read_only_pass=false);
    bool supported()const noexcept{return supported_;}
    bool read_only_pass()const noexcept{return read_only_pass_;}
    // No GPU commands: all failure remains a safe ordinary-Native refusal.
    std::unique_ptr<Prepared> Prepare(plume::RenderBuffer& source,const SurfaceLayout&,uint64_t version,uint64_t generation);
    // Only inside the existing owned transfer scope. A false pressure refusal
    // has not recorded this conversion; every post-record failure is fatal via
    // NativeTransferBatch's existing accepted-work contract.
    bool Record(Prepared&,NativeTransferBatch&);
    const Statistics& statistics()const noexcept{return statistics_;}
    BudgetSnapshot budget()const noexcept {return {budget_->bytes.load(std::memory_order_relaxed),budget_->peak.load(std::memory_order_relaxed)};}
private:
    plume::RenderDevice& device_;
    std::shared_ptr<Budget> budget_=std::make_shared<Budget>();
    std::unique_ptr<plume::RenderShader> compute_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderPipeline> pipeline_;
    bool supported_=false;
    bool read_only_pass_=false;
    Statistics statistics_;
};
}
