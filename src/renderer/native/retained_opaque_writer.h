#pragma once
#include "retained_opaque_authority.h"
#include "retained_opaque_attachment_sequence.h"
#include "retained_opaque_resource_budget.h"
#include "retained_opaque_scratch_sequence.h"
#include "retired_image_pool.h"
#include "plume_render_interface.h"
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace gow3::renderer {
class NativeTransferBatch;
class NativeGpuWorkQueue;
struct NativeRetainedOpaqueProgram {
    std::string source_sha256,vertex_sha256,variant_version,baseline_version,spirv_sha256;
    uint32_t specialization_mask=0;
    std::vector<uint32_t> spirv;
};
inline constexpr const char* kRetainedOpaquePixel="49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced";
inline constexpr const char* kRetainedOpaqueVertex="f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
// Diagnostic observations do not retain resources or alter queue progress.
// Weak lease expiry proves old control-block retirement; pointer equality alone
// cannot prove ownership, authority, actual fence completion or physical reuse.
struct NativeRetainedOpaqueResourceWitness {
    bool valid=false,pooled=false,dirty=false,shared_scratch=false,shared_attachments=false;
    NativeRetainedOpaqueAuthority::Ticket ticket;
    uintptr_t color=0,depth=0,framebuffer=0,readonly_framebuffer=0,scratch=0,descriptors=0,canonical=0;
    uint64_t color_bytes=0,depth_bytes=0,scratch_bytes=0,charged_bytes=0;
    uint64_t accepted_work=0,completed_work=0;
    plume::RenderTextureLayout color_before_restore=plume::RenderTextureLayout::UNKNOWN;
    plume::RenderTextureLayout depth_before_restore=plume::RenderTextureLayout::UNKNOWN;
    plume::RenderTextureLayout color_layout=plume::RenderTextureLayout::UNKNOWN;
    plume::RenderTextureLayout depth_layout=plume::RenderTextureLayout::UNKNOWN;
    std::weak_ptr<void> generation_lease,attachment_lease,scratch_lease;
};
struct NativeRetainedOpaqueGeneration {
    std::shared_ptr<plume::RenderTexture> color,depth;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    // Optional lease owns both image allocations AND their framebuffer. Color
    // and depth alias this same owner, including every fenced transfer packet.
    std::shared_ptr<void> pooled_resources,reserved_store_scratch;
    plume::RenderFramebuffer* pooled_framebuffer=nullptr;
    std::shared_ptr<void> shared_attachments;
    plume::RenderFramebuffer* shared_framebuffer=nullptr;
    plume::RenderFramebuffer* Framebuffer()const noexcept {
        return shared_attachments?shared_framebuffer:pooled_resources?pooled_framebuffer:framebuffer.get();
    }
    // Optional no-color LOAD/STORE view of the SAME authoritative depth owner.
    // Both framebuffers are destroyed before the shared image owners.
    std::unique_ptr<plume::RenderFramebuffer> readonly_framebuffer;
    NativeRetainedOpaqueAuthority authority;
    const void* conversion_owner=nullptr;
    uint64_t color_bytes=0,depth_bytes=0;
    plume::RenderTextureLayout color_before_restore=plume::RenderTextureLayout::UNKNOWN;
    plume::RenderTextureLayout depth_before_restore=plume::RenderTextureLayout::UNKNOWN;
};
class NativeRetainedOpaqueWriter {
public:
    static constexpr uint64_t kMaximumRetainedImageBytes=32u*1024*1024;
    using Budget=NativeRetainedOpaqueResourceBudget;
    struct Statistics {uint64_t restores=0,exports=0,allocation_refusals=0,budget_refusals=0;
        uint64_t attachment_allocations=0,scratch_allocations=0,attachment_reuses=0,scratch_reuses=0;
        uint64_t reused_restore_layouts=0,pooled_restores=0,pooled_exports=0;
        uint64_t shared_scratch_allocations=0,shared_scratch_reuses=0,shared_scratch_bytes=0;
        uint64_t shared_scratch_restores=0,shared_scratch_exports=0,shared_scratch_refusals=0,shared_scratch_recording_failures=0;
        uint64_t shared_attachment_allocations=0,shared_attachment_reuses=0,shared_attachment_closes=0;
        uint64_t shared_attachment_clean_closes=0,shared_attachment_refusals=0,shared_attachment_failures=0;};
    struct ResourcePoolStatistics {
        struct Pool {uint64_t hits=0,misses=0,returns=0,admitted=0,evictions=0,rejected=0,
            idle_images=0,idle_bytes=0,peak_idle_images=0,peak_idle_bytes=0;};
        Pool attachments,scratch;
    };
    struct Prepared {
        std::shared_ptr<NativeRetainedOpaqueGeneration> generation;
        plume::RenderBuffer* canonical=nullptr;
        std::unique_ptr<plume::RenderBuffer> scratch;
        std::unique_ptr<plume::RenderDescriptorSet> descriptors;
        std::shared_ptr<void> pooled_scratch;
        // Separate from legacy pool leases: immutable descriptor bindings and
        // one allocation shared only by this writer's accepted Native FIFO.
        std::shared_ptr<void> shared_scratch;
        uint64_t scratch_bytes=0;
        bool store=false;
        std::shared_ptr<NativeRetainedOpaqueAttachmentSequence> attachment_sequence;
        NativeRetainedOpaqueAttachmentSequence::Token attachment_token;
        ~Prepared();
    };
    NativeRetainedOpaqueWriter(plume::RenderDevice&,std::span<const uint32_t> restore,std::span<const uint32_t> store,bool reuse_resources=false,
                              NativeTransferBatch* shared_scratch_batch=nullptr,
                              NativeGpuWorkQueue* shared_attachment_queue=nullptr);
    ~NativeRetainedOpaqueWriter();
    bool reuses_resources()const noexcept{return reuse_resources_;}
    bool shares_scratch()const noexcept{return shared_scratch_batch_!=nullptr;}
    bool shares_attachments()const noexcept{return shared_attachment_queue_!=nullptr;}
    bool shared_attachments_failed()const noexcept{return shared_attachment_sequence_&&shared_attachment_sequence_->failed();}
    // Called only after the exact RunTransferScope successfully sealed its packet.
    void AcknowledgeSharedAttachmentConversion(Prepared&);
    // Validates the live ticket/dirty state BEFORE the existing Materialized transition.
    void CloseSharedAttachmentGeneration(const std::shared_ptr<NativeRetainedOpaqueGeneration>&);
    void FailSharedAttachments()noexcept;
    ResourcePoolStatistics pool_statistics()const;
    NativeRetainedOpaqueResourceWitness InspectForFixture(const std::shared_ptr<NativeRetainedOpaqueGeneration>&)const;
    bool supported()const noexcept{return supported_;}
    std::unique_ptr<Prepared> PrepareRestore(plume::RenderBuffer&,uint64_t generation,uint64_t color_version,uint64_t depth_version);
    std::unique_ptr<Prepared> PrepareStore(plume::RenderBuffer&,const std::shared_ptr<NativeRetainedOpaqueGeneration>&);
    // No synchronous fallback. False means no command for this conversion was
    // recorded; a dirty export caller must retry a fresh bounded scope or stop.
    bool Record(Prepared&,NativeTransferBatch&);
    bool PrepareReadonlyFramebuffer(const std::shared_ptr<NativeRetainedOpaqueGeneration>&);
    bool ValidateGeneration(const NativeRetainedOpaqueGeneration&)const noexcept;
    const Statistics& statistics()const noexcept{return statistics_;}
    uint64_t retained_bytes()const noexcept{return budget_->bytes.load(std::memory_order_relaxed);}
    uint64_t peak_bytes()const noexcept{return budget_->peak.load(std::memory_order_relaxed);}
private:
    struct AttachmentResources;
    struct ScratchResources;
    plume::RenderDevice& device_;
    std::shared_ptr<Budget> budget_=std::make_shared<Budget>();
    std::unique_ptr<plume::RenderShader> restore_,store_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderPipeline> restore_pipeline_,store_pipeline_;
    // Pools own unique retired resources; leases hold only a weak pool state.
    // These die before pipelines/layout/device. Budget includes all idle VMA
    // bytes and the generation-owned conversion scratch, not just images.
    std::unique_ptr<RetiredImagePool<AttachmentResources>> attachment_pool_;
    std::unique_ptr<RetiredImagePool<ScratchResources>> scratch_pool_;
    // Lifetime extends through every packet lease. Unlike the legacy pool,
    // image budget/accounting and physical image ownership remain unchanged.
    std::shared_ptr<ScratchResources> shared_scratch_;
    std::shared_ptr<AttachmentResources> shared_attachments_;
    std::weak_ptr<NativeRetainedOpaqueGeneration> shared_attachment_generation_;
    NativeGpuWorkQueue* shared_attachment_queue_=nullptr;
    std::shared_ptr<NativeRetainedOpaqueAttachmentSequence> shared_attachment_sequence_;
    NativeTransferBatch* shared_scratch_batch_=nullptr;
    NativeRetainedOpaqueScratchSequence shared_scratch_sequence_;
    bool reuse_resources_=false;
    bool supported_=false;
    Statistics statistics_;
    std::shared_ptr<plume::RenderTexture> BudgetImage(std::unique_ptr<plume::RenderTexture>,uint64_t& bytes);
    std::shared_ptr<AttachmentResources> AcquireAttachments();
    std::shared_ptr<AttachmentResources> AcquireSharedAttachments();
    NativeRetainedOpaqueAttachmentSequence::Binding SharedAttachmentBinding(plume::RenderBuffer&)const noexcept;
    std::unique_ptr<Prepared> PrepareSharedAttachmentRestore(plume::RenderBuffer&,uint64_t,uint64_t,uint64_t);
    std::shared_ptr<ScratchResources> AcquireScratch(plume::RenderBuffer&);
    std::shared_ptr<ScratchResources> AcquireSharedScratch(plume::RenderBuffer&);
    bool SharedScratchMatches(const ScratchResources&,const plume::RenderBuffer&)const noexcept;
    std::unique_ptr<Prepared> PreparePooledRestore(plume::RenderBuffer&,uint64_t,uint64_t,uint64_t);
    std::unique_ptr<Prepared> PrepareScratch(plume::RenderBuffer&,const std::shared_ptr<NativeRetainedOpaqueGeneration>&,bool store);
};
}
