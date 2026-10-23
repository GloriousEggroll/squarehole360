#pragma once
#include "retained_fp24_depth_authority.h"
#include "retained_opaque_resource_budget.h"
#include "plume_render_interface.h"
#include <memory>
#include <span>

namespace gow3::renderer {
class NativeTransferBatch;
class NativeRetainedFp24DepthWriter;
struct NativeRetainedFp24DepthScratch;
struct NativeRetainedFp24DepthGeneration {
    const NativeRetainedFp24DepthWriter* conversion_owner=nullptr;
    std::shared_ptr<plume::RenderTexture> depth;
    std::shared_ptr<NativeRetainedFp24DepthScratch> scratch;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer; // Dies before its depth image.
    NativeRetainedFp24DepthAuthority authority;
    uint64_t depth_bytes=0;
};
class NativeRetainedFp24DepthWriter {
public:
    static constexpr uint64_t kMaximumRetainedBytes=32u*1024*1024;
    struct Prepared {
        plume::RenderBuffer* canonical=nullptr;
        std::shared_ptr<NativeRetainedFp24DepthGeneration> generation;
        bool store=false,consumed=false;
    };
    struct Statistics {uint64_t preparations=0,restores=0,exports=0,budget_refusals=0,allocation_refusals=0;};
    NativeRetainedFp24DepthWriter(plume::RenderDevice&,std::span<const uint32_t> restore,std::span<const uint32_t> store);
    bool supported()const noexcept{return supported_;}
    bool ValidateGeneration(const NativeRetainedFp24DepthGeneration&)const noexcept;
    std::unique_ptr<Prepared> PrepareRestore(plume::RenderBuffer&,uint64_t generation,uint64_t depth_version);
    std::unique_ptr<Prepared> PrepareStore(plume::RenderBuffer&,const std::shared_ptr<NativeRetainedFp24DepthGeneration>&);
    bool Record(Prepared&,NativeTransferBatch&);
    const Statistics& statistics()const noexcept{return statistics_;}
    uint64_t retained_bytes()const noexcept{return budget_->bytes.load(std::memory_order_relaxed);}
    uint64_t peak_retained_bytes()const noexcept{return budget_->peak.load(std::memory_order_relaxed);}
private:
    plume::RenderDevice& device_;
    std::shared_ptr<NativeRetainedOpaqueResourceBudget> budget_=std::make_shared<NativeRetainedOpaqueResourceBudget>();
    std::unique_ptr<plume::RenderShader> restore_,store_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderPipeline> restore_pipeline_,store_pipeline_;
    bool supported_=false;
    Statistics statistics_;
};
}
