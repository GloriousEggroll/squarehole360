#include "retained_opaque_writer.h"
#include "transfer_batch.h"
#include "gpu_work_queue.h"
#include <type_traits>
#include "plume_vulkan.h"
#include <array>
#include <stdexcept>
namespace gow3::renderer {
using namespace plume;
namespace {
template<class T> uintptr_t HandleIdentity(T value)noexcept {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<uintptr_t>(value);
    else return static_cast<uintptr_t>(value);
}
NativeRetainedOpaqueAttachmentSequence::Ticket AttachmentTicket(const NativeRetainedOpaqueAuthority& authority)noexcept {
    const auto t=authority.ticket();return {t.generation,t.color,t.depth};
}
constexpr uint32_t kWidth=1280,kHeight=720,kPixels=kWidth*kHeight,kBytes=kPixels*4;
const std::array<RenderDescriptorRange,2> kRanges{{
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
bool Format(VulkanDevice& d,VkFormat format,VkFormatFeatureFlags attachment,VkImageUsageFlags usage) {
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(d.physicalDevice,format,&p);
    const auto needed=attachment|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((p.optimalTilingFeatures&needed)!=needed)return false;
    VkImageFormatProperties image{};
    return vkGetPhysicalDeviceImageFormatProperties(d.physicalDevice,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&image)==VK_SUCCESS&&
        image.maxExtent.width>=kWidth&&image.maxExtent.height>=kHeight&&(image.sampleCounts&VK_SAMPLE_COUNT_1_BIT)&&image.maxArrayLayers;
}
void Copy(VulkanCommandList& c,VulkanBuffer& scratch,VulkanTexture& image,uint32_t offset,VkImageAspectFlags aspect,bool store) {
    VkBufferImageCopy region{};region.bufferOffset=offset;region.imageSubresource={aspect,0,0,1};region.imageExtent={kWidth,kHeight,1};
    if(store)vkCmdCopyImageToBuffer(c.vk,image.vk,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scratch.vk,1,&region);
    else vkCmdCopyBufferToImage(c.vk,scratch.vk,image.vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
}
}
struct NativeRetainedOpaqueWriter::AttachmentResources {
    const void* owner=nullptr;
    std::shared_ptr<Budget> budget;
    std::unique_ptr<RenderTexture> color,depth;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    uint64_t color_bytes=0,depth_bytes=0,charged_bytes=0;
    VkImage initial_color=VK_NULL_HANDLE,initial_depth=VK_NULL_HANDLE;
    VmaAllocation initial_color_allocation=nullptr,initial_depth_allocation=nullptr;
    VkFramebuffer initial_framebuffer=VK_NULL_HANDLE;
    ~AttachmentResources() {
        framebuffer.reset();depth.reset();color.reset();
        if(budget)budget->Release(charged_bytes);
    }
};
struct NativeRetainedOpaqueWriter::ScratchResources {
    const void* owner=nullptr;
    std::shared_ptr<Budget> budget;
    std::unique_ptr<RenderBuffer> scratch;
    std::unique_ptr<RenderDescriptorSet> descriptors;
    RenderBuffer* canonical=nullptr;
    // Shared mode never rewrites these bindings, even after a completed fence.
    VkBuffer canonical_buffer=VK_NULL_HANDLE;
    VmaAllocation canonical_allocation=nullptr;
    uint64_t canonical_bytes=0;
    uint64_t bytes=0;
    ~ScratchResources() {
        descriptors.reset();scratch.reset();
        if(budget)budget->Release(bytes);
    }
};
NativeRetainedOpaqueWriter::~NativeRetainedOpaqueWriter()=default;
NativeRetainedOpaqueWriter::Prepared::~Prepared() {
    if(attachment_sequence&&attachment_token)attachment_sequence->Cancel(attachment_token);
}
NativeRetainedOpaqueWriter::ResourcePoolStatistics NativeRetainedOpaqueWriter::pool_statistics()const {
    ResourcePoolStatistics out;
    const auto copy=[](const auto& s)->ResourcePoolStatistics::Pool {
        return {s.hits,s.misses,s.returns,s.admitted,s.evictions,s.rejected,
            s.idle_images,s.idle_bytes,s.peak_idle_images,s.peak_idle_bytes};
    };
    if(attachment_pool_)out.attachments=copy(attachment_pool_->statistics());
    if(scratch_pool_)out.scratch=copy(scratch_pool_->statistics());
    return out;
}
NativeRetainedOpaqueResourceWitness NativeRetainedOpaqueWriter::InspectForFixture(
        const std::shared_ptr<NativeRetainedOpaqueGeneration>& generation)const {
    NativeRetainedOpaqueResourceWitness out;
    if(!generation||!ValidateGeneration(*generation))return out;
    const auto& g=*generation;
    out.valid=true;out.pooled=bool(g.pooled_resources);out.dirty=g.authority.dirty();out.ticket=g.authority.ticket();
    out.color=reinterpret_cast<uintptr_t>(g.color.get());out.depth=reinterpret_cast<uintptr_t>(g.depth.get());
    out.framebuffer=reinterpret_cast<uintptr_t>(g.Framebuffer());
    out.readonly_framebuffer=reinterpret_cast<uintptr_t>(g.readonly_framebuffer.get());
    out.generation_lease=generation;
    out.color_bytes=g.color_bytes;out.depth_bytes=g.depth_bytes;out.charged_bytes=retained_bytes();
    out.color_before_restore=g.color_before_restore;out.depth_before_restore=g.depth_before_restore;
    out.color_layout=static_cast<const VulkanTexture&>(*g.color).textureLayout;
    out.depth_layout=static_cast<const VulkanTexture&>(*g.depth).textureLayout;
    out.shared_attachments=bool(g.shared_attachments);
    out.attachment_lease=g.shared_attachments?g.shared_attachments:g.pooled_resources;out.scratch_lease=g.reserved_store_scratch;
    if(shared_scratch_) {
        const auto& r=*shared_scratch_;
        if(!r.canonical||!SharedScratchMatches(r,*r.canonical))
            throw std::logic_error("Retained shared scratch diagnostic encountered a changed immutable lease");
        out.shared_scratch=true;out.scratch_lease=shared_scratch_;
        out.scratch=reinterpret_cast<uintptr_t>(r.scratch.get());out.descriptors=reinterpret_cast<uintptr_t>(r.descriptors.get());
        out.canonical=reinterpret_cast<uintptr_t>(r.canonical);out.scratch_bytes=r.bytes;
    }
    if(g.reserved_store_scratch) {
        const auto r=std::static_pointer_cast<ScratchResources>(g.reserved_store_scratch);
        if(r->owner!=this||r->budget.get()!=budget_.get()||!r->scratch||!r->descriptors)
            throw std::logic_error("Retained resource diagnostic encountered a foreign conversion lease");
        out.scratch=reinterpret_cast<uintptr_t>(r->scratch.get());out.descriptors=reinterpret_cast<uintptr_t>(r->descriptors.get());
        out.canonical=reinterpret_cast<uintptr_t>(r->canonical);out.scratch_bytes=r->bytes;
    }
    return out;
}
NativeRetainedOpaqueWriter::NativeRetainedOpaqueWriter(RenderDevice& d,std::span<const uint32_t> restore,std::span<const uint32_t> store,bool reuse_resources,
        NativeTransferBatch* shared_scratch_batch,NativeGpuWorkQueue* shared_attachment_queue):device_(d),shared_attachment_queue_(shared_attachment_queue),shared_scratch_batch_(shared_scratch_batch),shared_scratch_sequence_(shared_scratch_batch),reuse_resources_(reuse_resources) {
    if(shared_attachment_queue_) {
        if(reuse_resources_||!shared_scratch_batch_||shared_scratch_batch_->work_queue_identity()!=shared_attachment_queue_)
            throw std::invalid_argument("Shared phase attachments require the exact shared scratch FIFO");
        shared_attachment_sequence_=std::make_shared<NativeRetainedOpaqueAttachmentSequence>();
    }
    if(shared_scratch_batch_&&(reuse_resources_||!shared_scratch_batch_->has_work_queue()))
        throw std::invalid_argument("Shared opaque scratch requires one Native FIFO and fresh image ownership");
    auto& vk=static_cast<VulkanDevice&>(d);constexpr auto copy=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(!Format(vk,VK_FORMAT_R32_UINT,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,copy|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)||
       !Format(vk,VK_FORMAT_D32_SFLOAT,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,copy|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))return;
    if(restore.size()<5||store.size()<5||restore.front()!=0x07230203||store.front()!=0x07230203)return;
    const RenderDescriptorSetDesc desc(kRanges.data(),kRanges.size());layout_=d.createPipelineLayout(RenderPipelineLayoutDesc(nullptr,0,&desc,1));
    restore_=d.createShader(restore.data(),restore.size_bytes(),"main",RenderShaderFormat::SPIRV);
    store_=d.createShader(store.data(),store.size_bytes(),"main",RenderShaderFormat::SPIRV);
    if(!layout_||!restore_||!store_||!static_cast<VulkanPipelineLayout&>(*layout_).vk||
       !static_cast<VulkanShader&>(*restore_).vk||!static_cast<VulkanShader&>(*store_).vk)return;
    restore_pipeline_=d.createComputePipeline(RenderComputePipelineDesc(layout_.get(),restore_.get(),8,8,1));
    store_pipeline_=d.createComputePipeline(RenderComputePipelineDesc(layout_.get(),store_.get(),8,8,1));
    supported_=restore_pipeline_&&store_pipeline_&&static_cast<VulkanComputePipeline&>(*restore_pipeline_).vk&&static_cast<VulkanComputePipeline&>(*store_pipeline_).vk;
    if(supported_&&reuse_resources_) {
        attachment_pool_=std::make_unique<RetiredImagePool<AttachmentResources>>(2,kMaximumRetainedImageBytes);
        scratch_pool_=std::make_unique<RetiredImagePool<ScratchResources>>(2,kMaximumRetainedImageBytes);
    }
}
std::shared_ptr<NativeRetainedOpaqueWriter::AttachmentResources> NativeRetainedOpaqueWriter::AcquireAttachments() {
    auto resources=attachment_pool_->Take(kWidth,kHeight,0);
    if(resources)++statistics_.attachment_reuses;
    else {
        resources=std::make_unique<AttachmentResources>();
        resources->owner=this;
        resources->color=device_.createTexture(RenderTextureDesc::ColorTarget(kWidth,kHeight,RenderFormat::R32_UINT));
        resources->depth=device_.createTexture(RenderTextureDesc::DepthTarget(kWidth,kHeight,RenderFormat::D32_FLOAT));
        if(!resources->color||!resources->depth)return {};
        const auto& color=static_cast<VulkanTexture&>(*resources->color);
        const auto& depth=static_cast<VulkanTexture&>(*resources->depth);
        if(!color.vk||!color.allocation||!depth.vk||!depth.allocation)return {};
        resources->color_bytes=color.allocationInfo.size;resources->depth_bytes=depth.allocationInfo.size;
        if(!resources->color_bytes||!resources->depth_bytes||resources->color_bytes>kMaximumRetainedImageBytes||
           resources->depth_bytes>kMaximumRetainedImageBytes-resources->color_bytes) {++statistics_.budget_refusals;return {};}
        resources->charged_bytes=resources->color_bytes+resources->depth_bytes;
        if(!budget_->TryCharge(resources->charged_bytes,kMaximumRetainedImageBytes)) {++statistics_.budget_refusals;return {};}
        resources->budget=budget_;
        const RenderTexture* target=resources->color.get();RenderFramebufferDesc fd;fd.colorAttachments=&target;fd.colorAttachmentsCount=1;
        fd.depthAttachment=resources->depth.get();fd.depthAttachmentReadOnly=false;
        resources->framebuffer=device_.createFramebuffer(fd);
        if(!resources->framebuffer||!static_cast<VulkanFramebuffer&>(*resources->framebuffer).vk||
           !static_cast<VulkanFramebuffer&>(*resources->framebuffer).renderPass)return {};
        ++statistics_.attachment_allocations;
    }
    if(resources->owner!=this||resources->budget.get()!=budget_.get()||!resources->framebuffer)return {};
    const auto color_layout=static_cast<VulkanTexture&>(*resources->color).textureLayout;
    const auto depth_layout=static_cast<VulkanTexture&>(*resources->depth).textureLayout;
    // Layout tracking follows actually accepted commands; never reset reused
    // images to UNKNOWN. Both states are known at the last fence retirement.
    if(!((color_layout==RenderTextureLayout::UNKNOWN&&depth_layout==RenderTextureLayout::UNKNOWN)||
         (color_layout==RenderTextureLayout::COPY_SOURCE&&depth_layout==RenderTextureLayout::COPY_SOURCE)||
         (color_layout==RenderTextureLayout::COLOR_WRITE&&depth_layout==RenderTextureLayout::DEPTH_WRITE)))return {};
    const auto bytes=resources->charged_bytes;
    return attachment_pool_->Share(std::move(resources),kWidth,kHeight,bytes,0);
}

std::shared_ptr<NativeRetainedOpaqueWriter::AttachmentResources> NativeRetainedOpaqueWriter::AcquireSharedAttachments() {
    if(shared_attachments_)return shared_attachments_;
    auto r=std::make_shared<AttachmentResources>();r->owner=this;
    r->color=device_.createTexture(RenderTextureDesc::ColorTarget(kWidth,kHeight,RenderFormat::R32_UINT));
    r->depth=device_.createTexture(RenderTextureDesc::DepthTarget(kWidth,kHeight,RenderFormat::D32_FLOAT));
    if(!r->color||!r->depth)return {};
    const auto& color=static_cast<const VulkanTexture&>(*r->color);
    const auto& depth=static_cast<const VulkanTexture&>(*r->depth);
    if(!color.vk||!color.allocation||!depth.vk||!depth.allocation)return {};
    r->color_bytes=color.allocationInfo.size;r->depth_bytes=depth.allocationInfo.size;
    if(!r->color_bytes||!r->depth_bytes||r->color_bytes>kMaximumRetainedImageBytes||
       r->depth_bytes>kMaximumRetainedImageBytes-r->color_bytes){++statistics_.budget_refusals;return {};}
    r->charged_bytes=r->color_bytes+r->depth_bytes;
    if(!budget_->TryCharge(r->charged_bytes,kMaximumRetainedImageBytes)){++statistics_.budget_refusals;return {};}
    r->budget=budget_;
    const RenderTexture* target=r->color.get();RenderFramebufferDesc desc;
    desc.colorAttachments=&target;desc.colorAttachmentsCount=1;desc.depthAttachment=r->depth.get();desc.depthAttachmentReadOnly=false;
    r->framebuffer=device_.createFramebuffer(desc);
    if(!r->framebuffer)return {};
    const auto& framebuffer=static_cast<const VulkanFramebuffer&>(*r->framebuffer);
    if(!framebuffer.vk||!framebuffer.renderPass)return {};
    r->initial_color=color.vk;r->initial_depth=depth.vk;
    r->initial_color_allocation=color.allocation;r->initial_depth_allocation=depth.allocation;
    r->initial_framebuffer=framebuffer.vk;
    shared_attachments_=r;++statistics_.shared_attachment_allocations;return r;
}
NativeRetainedOpaqueAttachmentSequence::Binding NativeRetainedOpaqueWriter::SharedAttachmentBinding(RenderBuffer& canonical)const noexcept {
    if(!shared_attachment_queue_||!shared_scratch_batch_||shared_scratch_batch_->work_queue_identity()!=shared_attachment_queue_||
       !shared_attachments_||!shared_scratch_||!SharedScratchMatches(*shared_scratch_,canonical))return {};
    const auto& vk=static_cast<const VulkanBuffer&>(canonical);
    return {HandleIdentity(this),HandleIdentity(shared_scratch_batch_),HandleIdentity(shared_attachment_queue_),
        HandleIdentity(&canonical),HandleIdentity(vk.vk),HandleIdentity(vk.allocation),HandleIdentity(shared_attachments_.get()),vk.desc.size,vk.allocationInfo.size};
}
std::unique_ptr<NativeRetainedOpaqueWriter::Prepared> NativeRetainedOpaqueWriter::PrepareSharedAttachmentRestore(
        RenderBuffer& canonical,uint64_t generation,uint64_t color_version,uint64_t depth_version) {
    if(shared_attachment_sequence_->failed())throw std::logic_error("Shared attachment sequence is poisoned");
    if(shared_attachment_sequence_->state()!=NativeRetainedOpaqueAttachmentSequence::State::Idle)
        throw std::logic_error("New shared attachment restore before prior logical generation closure");
    try {
        const bool reused=bool(shared_attachments_);auto physical=AcquireSharedAttachments();
        if(!physical){++statistics_.allocation_refusals;return {};}
        auto g=std::make_shared<NativeRetainedOpaqueGeneration>();g->conversion_owner=this;
        g->shared_attachments=physical;g->shared_framebuffer=physical->framebuffer.get();
        g->color=std::shared_ptr<RenderTexture>(physical,physical->color.get());
        g->depth=std::shared_ptr<RenderTexture>(physical,physical->depth.get());
        g->color_bytes=physical->color_bytes;g->depth_bytes=physical->depth_bytes;
        g->color_before_restore=static_cast<const VulkanTexture&>(*g->color).textureLayout;
        g->depth_before_restore=static_cast<const VulkanTexture&>(*g->depth).textureLayout;
        const auto cl=g->color_before_restore,dl=g->depth_before_restore;
        if(!((!statistics_.restores&&cl==RenderTextureLayout::UNKNOWN&&dl==RenderTextureLayout::UNKNOWN)||
             (cl==RenderTextureLayout::COPY_SOURCE&&dl==RenderTextureLayout::COPY_SOURCE)||
             (cl==RenderTextureLayout::COLOR_WRITE&&dl==RenderTextureLayout::DEPTH_WRITE)))
            throw std::logic_error("Shared attachments lost their actual prior layout");
        g->authority.Admit(generation,kNativeRetainedOpaqueLayout,color_version,depth_version);
        auto out=PrepareScratch(canonical,g,false);if(!out){++statistics_.allocation_refusals;return {};}
        const auto binding=SharedAttachmentBinding(canonical);
        if(!shared_attachment_sequence_->Bind(binding))throw std::logic_error("Shared attachments changed canonical owner");
        out->attachment_sequence=shared_attachment_sequence_;
        out->attachment_token=shared_attachment_sequence_->Prepare(binding,AttachmentTicket(g->authority),false);
        if(!out->attachment_token){++statistics_.shared_attachment_refusals;return {};}
        shared_attachment_generation_=g;statistics_.shared_attachment_reuses+=reused;return out;
    }catch(...){FailSharedAttachments();throw;}
}
void NativeRetainedOpaqueWriter::FailSharedAttachments()noexcept {
    if(shared_attachment_sequence_) {
        if(!shared_attachment_sequence_->failed())++statistics_.shared_attachment_failures;
        shared_attachment_sequence_->Fail();shared_scratch_sequence_.FailRecording();
    }
}
void NativeRetainedOpaqueWriter::AcknowledgeSharedAttachmentConversion(Prepared& p) {
    if(!shares_attachments())throw std::logic_error("Shared attachment acknowledgment is not enabled");
    try {
        if(!p.canonical||!p.generation||p.generation!=shared_attachment_generation_.lock()||
           !ValidateGeneration(*p.generation)||p.attachment_sequence!=shared_attachment_sequence_||
           AttachmentTicket(p.generation->authority)!=p.attachment_token.ticket)
            throw std::logic_error("Shared attachment acknowledgment lost exact generation identity");
        const auto observation=shared_attachment_queue_->ObserveSubmission();const auto& counts=shared_scratch_batch_->statistics();
        if(!shared_attachment_sequence_->Acknowledge(SharedAttachmentBinding(*p.canonical),p.attachment_token,
            {observation.accepted,counts.stages,counts.queued_lists},
            !shared_scratch_batch_->in_scope()&&!shared_scratch_batch_->has_recording(),observation.available))
            throw std::logic_error("Shared attachment conversion was not accepted as one exact FIFO packet");
        p.attachment_token={};
    }catch(...){FailSharedAttachments();throw;}
}
void NativeRetainedOpaqueWriter::CloseSharedAttachmentGeneration(const std::shared_ptr<NativeRetainedOpaqueGeneration>& generation) {
    try {
        if(!shares_attachments()||!generation||generation!=shared_attachment_generation_.lock()||
           !ValidateGeneration(*generation)||!shared_scratch_||!shared_scratch_->canonical||
           shared_scratch_batch_->in_scope()||shared_scratch_batch_->has_recording()||
           !shared_attachment_queue_->ObserveSubmission().available)
            throw std::logic_error("Shared attachment closure lost exact FIFO/generation ownership");
        const auto binding=SharedAttachmentBinding(*shared_scratch_->canonical);
        const auto ticket=generation->authority.ticket();const auto current=AttachmentTicket(generation->authority);
        const bool dirty=generation->authority.dirty();
        if(!shared_attachment_sequence_->CanClose(binding,current,dirty))
            throw std::logic_error("Shared attachment closure lacks its exact accepted restore/export");
        generation->authority.Materialized(ticket,true);
        if(!shared_attachment_sequence_->Close(binding,current,dirty))
            throw std::logic_error("Shared attachment closure failed after authority materialization");
        shared_attachment_generation_.reset();++statistics_.shared_attachment_closes;
        statistics_.shared_attachment_clean_closes+=!dirty;
    }catch(...){FailSharedAttachments();throw;}
}

std::shared_ptr<NativeRetainedOpaqueWriter::ScratchResources> NativeRetainedOpaqueWriter::AcquireScratch(RenderBuffer& canonical) {
    auto resources=scratch_pool_->Take(kWidth,kHeight,0);
    if(resources)++statistics_.scratch_reuses;
    else {
        resources=std::make_unique<ScratchResources>();
        resources->owner=this;
        resources->scratch=device_.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes*2,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        if(!resources->scratch)return {};
        const auto& scratch=static_cast<VulkanBuffer&>(*resources->scratch);
        if(!scratch.vk||!scratch.allocation)return {};
        resources->bytes=scratch.allocationInfo.size;
        if(resources->bytes<kBytes*2||!budget_->TryCharge(resources->bytes,kMaximumRetainedImageBytes)) {++statistics_.budget_refusals;return {};}
        resources->budget=budget_;
        resources->descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(kRanges.data(),kRanges.size()));
        if(!resources->descriptors||!static_cast<VulkanDescriptorSet&>(*resources->descriptors).vk)return {};
        ++statistics_.scratch_allocations;
    }
    if(resources->owner!=this||resources->budget.get()!=budget_.get()||!resources->scratch||!resources->descriptors)return {};
    resources->canonical=&canonical;
    resources->descriptors->setBuffer(0,&canonical);resources->descriptors->setBuffer(1,resources->scratch.get(),kBytes*2);
    const auto bytes=resources->bytes;
    return scratch_pool_->Share(std::move(resources),kWidth,kHeight,bytes,0);
}
bool NativeRetainedOpaqueWriter::SharedScratchMatches(const ScratchResources& resources,const RenderBuffer& canonical)const noexcept {
    const auto& source=static_cast<const VulkanBuffer&>(canonical);
    if(resources.owner!=this||resources.budget||resources.canonical!=&canonical||
       source.device!=static_cast<const VulkanDevice*>(&device_)||!source.vk||!source.allocation||
       source.desc.size!=2048u*5120||source.vk!=resources.canonical_buffer||
       source.allocation!=resources.canonical_allocation||source.allocationInfo.size!=resources.canonical_bytes||
       !resources.scratch||!resources.descriptors)return false;
    const auto& scratch=static_cast<const VulkanBuffer&>(*resources.scratch);
    const auto& descriptors=static_cast<const VulkanDescriptorSet&>(*resources.descriptors);
    return scratch.device==source.device&&scratch.vk&&scratch.allocation&&
        scratch.desc.size==kBytes*2&&scratch.allocationInfo.size==resources.bytes&&
        resources.bytes>=kBytes*2&&resources.bytes<=NativeTransferBatch::kMaxImageBytes&&descriptors.vk;
}
std::shared_ptr<NativeRetainedOpaqueWriter::ScratchResources> NativeRetainedOpaqueWriter::AcquireSharedScratch(RenderBuffer& canonical) {
    if(!shared_scratch_sequence_.CanPrepare(&canonical)) {++statistics_.shared_scratch_refusals;return {};}
    if(shared_scratch_) {
        if(!SharedScratchMatches(*shared_scratch_,canonical)) {++statistics_.shared_scratch_refusals;return {};}
        ++statistics_.shared_scratch_reuses;return shared_scratch_;
    }
    const auto& source=static_cast<const VulkanBuffer&>(canonical);
    if(source.device!=static_cast<const VulkanDevice*>(&device_)||!source.vk||!source.allocation||source.desc.size!=2048u*5120)return {};
    auto resources=std::make_shared<ScratchResources>();resources->owner=this;resources->canonical=&canonical;
    resources->canonical_buffer=source.vk;resources->canonical_allocation=source.allocation;
    resources->canonical_bytes=source.allocationInfo.size;
    resources->scratch=device_.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes*2,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    if(!resources->scratch)return {};
    const auto& scratch=static_cast<const VulkanBuffer&>(*resources->scratch);
    if(!scratch.vk||!scratch.allocation)return {};
    resources->bytes=scratch.allocationInfo.size;
    if(resources->bytes<kBytes*2||resources->bytes>NativeTransferBatch::kMaxImageBytes)return {};
    resources->descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(kRanges.data(),kRanges.size()));
    if(!resources->descriptors||!static_cast<const VulkanDescriptorSet&>(*resources->descriptors).vk)return {};
    // Exactly once: no descriptor update while earlier packets may be in flight.
    resources->descriptors->setBuffer(0,&canonical);resources->descriptors->setBuffer(1,resources->scratch.get(),kBytes*2);
    if(!SharedScratchMatches(*resources,canonical)||!shared_scratch_sequence_.BindCanonical(&canonical))return {};
    shared_scratch_=resources;++statistics_.shared_scratch_allocations;statistics_.shared_scratch_bytes=resources->bytes;
    return resources;
}
std::unique_ptr<NativeRetainedOpaqueWriter::Prepared> NativeRetainedOpaqueWriter::PreparePooledRestore(RenderBuffer& canonical,
        uint64_t generation,uint64_t color_version,uint64_t depth_version) {
    const auto& source=static_cast<VulkanBuffer&>(canonical);
    if(source.device!=static_cast<VulkanDevice*>(&device_)||!source.vk||source.desc.size!=2048u*5120)return {};
    auto attachments=AcquireAttachments();if(!attachments)return {};
    // One immutable conversion lease belongs to this generation. Restore and
    // store use it only through the same accepted FIFO with existing barriers.
    // It cannot return to the cross-generation pool until every owner retires.
    auto restore=AcquireScratch(canonical);if(!restore)return {};
    auto g=std::make_shared<NativeRetainedOpaqueGeneration>();g->conversion_owner=this;
    g->pooled_resources=attachments;g->pooled_framebuffer=attachments->framebuffer.get();
    g->color=std::shared_ptr<RenderTexture>(attachments,attachments->color.get());
    g->depth=std::shared_ptr<RenderTexture>(attachments,attachments->depth.get());
    g->color_bytes=attachments->color_bytes;g->depth_bytes=attachments->depth_bytes;
    g->reserved_store_scratch=restore;
    g->color_before_restore=static_cast<const VulkanTexture&>(*g->color).textureLayout;
    g->depth_before_restore=static_cast<const VulkanTexture&>(*g->depth).textureLayout;
    // New authority object and page/version ticket on EVERY physical reuse.
    g->authority.Admit(generation,kNativeRetainedOpaqueLayout,color_version,depth_version);
    if(!ValidateGeneration(*g))return {};
    auto out=std::make_unique<Prepared>();out->canonical=&canonical;out->generation=std::move(g);
    out->scratch_bytes=restore->bytes;out->pooled_scratch=std::move(restore);return out;
}
std::shared_ptr<RenderTexture> NativeRetainedOpaqueWriter::BudgetImage(std::unique_ptr<RenderTexture> image,uint64_t& bytes) {
    if(!image||!static_cast<VulkanTexture&>(*image).vk||!static_cast<VulkanTexture&>(*image).allocation)return {};
    bytes=static_cast<VulkanTexture&>(*image).allocationInfo.size;
    if(!bytes||bytes>kMaximumRetainedImageBytes){++statistics_.budget_refusals;return {};}
    auto held=budget_->bytes.load(std::memory_order_relaxed);
    do {if(held>kMaximumRetainedImageBytes-bytes){++statistics_.budget_refusals;return {};}}
    while(!budget_->bytes.compare_exchange_weak(held,held+bytes,std::memory_order_relaxed));
    auto peak=budget_->peak.load(std::memory_order_relaxed);
    while(peak<held+bytes&&!budget_->peak.compare_exchange_weak(peak,held+bytes,std::memory_order_relaxed)){}
    auto budget=budget_;return std::shared_ptr<RenderTexture>(image.release(),[budget,bytes](RenderTexture* image)noexcept {delete image;budget->bytes.fetch_sub(bytes,std::memory_order_relaxed);});
}
bool NativeRetainedOpaqueWriter::ValidateGeneration(const NativeRetainedOpaqueGeneration& g)const noexcept {
    if(g.conversion_owner!=this||!g.color||!g.depth||!g.Framebuffer()||!g.authority.present()||g.authority.failed())return false;
    if(bool(g.shared_attachments)!=shares_attachments())return false;
    if(g.shared_attachments) {
        const auto& r=shared_attachments_;
        if(!r||g.shared_attachments!=r||g.shared_attachments.owner_before(r)||r.owner_before(g.shared_attachments)||
           g.color.owner_before(r)||r.owner_before(g.color)||g.depth.owner_before(r)||r.owner_before(g.depth)||
           g.pooled_resources||g.framebuffer||g.readonly_framebuffer||g.shared_framebuffer!=r->framebuffer.get()||
           r->owner!=this||r->budget!=budget_||r->color.get()!=g.color.get()||r->depth.get()!=g.depth.get()||
           r->color_bytes!=g.color_bytes||r->depth_bytes!=g.depth_bytes)return false;
        const auto& color=static_cast<const VulkanTexture&>(*r->color);
        const auto& depth=static_cast<const VulkanTexture&>(*r->depth);
        const auto& framebuffer=static_cast<const VulkanFramebuffer&>(*r->framebuffer);
        if(color.vk!=r->initial_color||depth.vk!=r->initial_depth||
           color.allocation!=r->initial_color_allocation||depth.allocation!=r->initial_depth_allocation||
           framebuffer.vk!=r->initial_framebuffer)return false;
    }
    if(bool(g.pooled_resources)!=reuse_resources_||(g.pooled_resources&&(!g.pooled_framebuffer||g.framebuffer)))return false;
    if(g.pooled_resources) {
        const auto r=std::static_pointer_cast<AttachmentResources>(g.pooled_resources);
        if(r->owner!=this||r->budget.get()!=budget_.get()||r->color.get()!=g.color.get()||r->depth.get()!=g.depth.get()||
           r->framebuffer.get()!=g.pooled_framebuffer||r->color_bytes!=g.color_bytes||r->depth_bytes!=g.depth_bytes)return false;
    }
    const auto* d=static_cast<const VulkanDevice*>(&device_);
    const auto valid=[&](const RenderTexture& image,RenderFormat format,uint64_t bytes) {
        const auto& vk=static_cast<const VulkanTexture&>(image);const auto& desc=vk.desc;
        return vk.device==d&&vk.vk&&vk.allocation&&vk.allocationInfo.size==bytes&&bytes&&bytes<=kMaximumRetainedImageBytes&&
            desc.format==format&&desc.width==kWidth&&desc.height==kHeight&&desc.depth==1&&desc.mipLevels==1&&desc.arraySize==1&&
            desc.dimension==RenderTextureDimension::TEXTURE_2D&&desc.multisampling.sampleCount==1;
    };
    if(!valid(*g.color,RenderFormat::R32_UINT,g.color_bytes)||!valid(*g.depth,RenderFormat::D32_FLOAT,g.depth_bytes)||
       g.color_bytes>kMaximumRetainedImageBytes-g.depth_bytes)return false;
    const auto& framebuffer=static_cast<const VulkanFramebuffer&>(*g.Framebuffer());
    if(!(framebuffer.device==d&&framebuffer.vk&&framebuffer.renderPass&&framebuffer.width==kWidth&&framebuffer.height==kHeight&&
        !framebuffer.depthAttachmentReadOnly&&framebuffer.colorAttachments.size()==1&&
        framebuffer.colorAttachments[0]==g.color.get()&&framebuffer.depthAttachment==g.depth.get()))return false;
    if(g.readonly_framebuffer) {
        const auto& readonly=static_cast<const VulkanFramebuffer&>(*g.readonly_framebuffer);
        if(readonly.device!=d||!readonly.vk||!readonly.renderPass||readonly.width!=kWidth||readonly.height!=kHeight||
           readonly.depthAttachmentReadOnly||!readonly.colorAttachments.empty()||readonly.depthAttachment!=g.depth.get())return false;
    }
    return true;
}
std::unique_ptr<NativeRetainedOpaqueWriter::Prepared> NativeRetainedOpaqueWriter::PrepareScratch(RenderBuffer& canonical,
        const std::shared_ptr<NativeRetainedOpaqueGeneration>& generation,bool store) {
    auto& source=static_cast<VulkanBuffer&>(canonical);
    if(!supported_||!generation||!ValidateGeneration(*generation)||source.device!=static_cast<VulkanDevice*>(&device_)||!source.vk||source.desc.size!=2048u*5120)return {};
    auto out=std::make_unique<Prepared>();out->canonical=&canonical;out->generation=generation;out->store=store;
    if(shared_scratch_batch_) {
        auto scratch=AcquireSharedScratch(canonical);if(!scratch)return {};
        if(scratch->bytes>NativeTransferBatch::kMaxImageBytes-generation->color_bytes-generation->depth_bytes)return {};
        out->scratch_bytes=scratch->bytes;out->shared_scratch=std::move(scratch);return out;
    }
    if(reuse_resources_) {
        if(!store||!generation->reserved_store_scratch)return {};
        const auto scratch=std::static_pointer_cast<ScratchResources>(generation->reserved_store_scratch);
        if(scratch->canonical!=&canonical||!scratch->scratch||!scratch->descriptors)return {};
        out->scratch_bytes=scratch->bytes;out->pooled_scratch=scratch;return out;
    }
    out->scratch=device_.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes*2,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    if(!out->scratch||!static_cast<VulkanBuffer&>(*out->scratch).vk||!static_cast<VulkanBuffer&>(*out->scratch).allocation)return {};
    out->scratch_bytes=static_cast<VulkanBuffer&>(*out->scratch).allocationInfo.size;
    if(out->scratch_bytes<kBytes*2||out->scratch_bytes>NativeTransferBatch::kMaxImageBytes-generation->color_bytes-generation->depth_bytes)return {};
    out->descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(kRanges.data(),kRanges.size()));
    if(!out->descriptors||!static_cast<VulkanDescriptorSet&>(*out->descriptors).vk)return {};
    out->descriptors->setBuffer(0,&canonical);out->descriptors->setBuffer(1,out->scratch.get(),kBytes*2);
    return out;
}
std::unique_ptr<NativeRetainedOpaqueWriter::Prepared> NativeRetainedOpaqueWriter::PrepareRestore(RenderBuffer& canonical,
        uint64_t generation,uint64_t color_version,uint64_t depth_version) {
    if(!supported_||!generation||!color_version||!depth_version)return {};
    if(shares_attachments())return PrepareSharedAttachmentRestore(canonical,generation,color_version,depth_version);
    try {
        if(reuse_resources_) {
            auto out=PreparePooledRestore(canonical,generation,color_version,depth_version);
            if(!out)++statistics_.allocation_refusals;return out;
        }
        auto g=std::make_shared<NativeRetainedOpaqueGeneration>();g->conversion_owner=this;
        g->color=BudgetImage(device_.createTexture(RenderTextureDesc::ColorTarget(kWidth,kHeight,RenderFormat::R32_UINT)),g->color_bytes);
        g->depth=BudgetImage(device_.createTexture(RenderTextureDesc::DepthTarget(kWidth,kHeight,RenderFormat::D32_FLOAT)),g->depth_bytes);
        if(!g->color||!g->depth){++statistics_.allocation_refusals;return {};}
        const RenderTexture* color=g->color.get();RenderFramebufferDesc fd;fd.colorAttachments=&color;fd.colorAttachmentsCount=1;
        fd.depthAttachment=g->depth.get();fd.depthAttachmentReadOnly=false;
        g->framebuffer=device_.createFramebuffer(fd);
        if(!g->framebuffer||!static_cast<VulkanFramebuffer&>(*g->framebuffer).vk||!static_cast<VulkanFramebuffer&>(*g->framebuffer).renderPass) {
            ++statistics_.allocation_refusals;return {};
        }
        // Admission to actual Scene authority occurs after Record succeeds.
        g->authority.Admit(generation,kNativeRetainedOpaqueLayout,color_version,depth_version);
        auto out=PrepareScratch(canonical,g,false);if(!out)++statistics_.allocation_refusals;return out;
    }catch(const std::exception&){++statistics_.allocation_refusals;return {};}
}
std::unique_ptr<NativeRetainedOpaqueWriter::Prepared> NativeRetainedOpaqueWriter::PrepareStore(RenderBuffer& canonical,
        const std::shared_ptr<NativeRetainedOpaqueGeneration>& generation) {
    if(shares_attachments()) {
        if(shared_attachment_sequence_->failed())throw std::logic_error("Shared attachment sequence is poisoned");
        if(generation!=shared_attachment_generation_.lock()||!generation||!generation->authority.dirty())return {};
        auto out=PrepareScratch(canonical,generation,true);
        if(!out){++statistics_.allocation_refusals;return {};}
        out->attachment_sequence=shared_attachment_sequence_;
        out->attachment_token=shared_attachment_sequence_->Prepare(SharedAttachmentBinding(canonical),AttachmentTicket(generation->authority),true);
        if(!out->attachment_token){++statistics_.shared_attachment_refusals;return {};}
        return out;
    }
    try {auto out=PrepareScratch(canonical,generation,true);if(!out)++statistics_.allocation_refusals;return out;}
    catch(const std::exception&){++statistics_.allocation_refusals;return {};}
}
bool NativeRetainedOpaqueWriter::PrepareReadonlyFramebuffer(
        const std::shared_ptr<NativeRetainedOpaqueGeneration>& generation) {
    if(!supported_||shares_attachments()||!generation||!ValidateGeneration(*generation))return false;
    if(generation->readonly_framebuffer)return true;
    try {
        RenderFramebufferDesc desc;desc.depthAttachment=generation->depth.get();
        // readonly=true may select LOAD_OP_NONE on Plume. LOAD+STORE and the
        // unchanged DEPTH_WRITE image dependencies preserve the exact contents.
        desc.depthAttachmentReadOnly=false;
        auto framebuffer=device_.createFramebuffer(desc);
        if(!framebuffer)return false;
        const auto& vk=static_cast<const VulkanFramebuffer&>(*framebuffer);
        if(vk.device!=static_cast<VulkanDevice*>(&device_)||!vk.vk||!vk.renderPass||
           vk.width!=kWidth||vk.height!=kHeight||!vk.colorAttachments.empty()||
           vk.depthAttachment!=generation->depth.get()||vk.depthAttachmentReadOnly)return false;
        generation->readonly_framebuffer=std::move(framebuffer);return true;
    }catch(...){++statistics_.allocation_refusals;return false;}
}
bool NativeRetainedOpaqueWriter::Record(Prepared& p,NativeTransferBatch& batch) {
    if(!supported_||!p.canonical||!p.generation||!ValidateGeneration(*p.generation)||!batch.active())return false;
    if(shares_attachments()) {
        const bool owners=&batch==shared_scratch_batch_&&batch.work_queue_identity()==shared_attachment_queue_&&
            p.attachment_sequence==shared_attachment_sequence_&&p.generation==shared_attachment_generation_.lock()&&
            p.attachment_token.store==p.store&&AttachmentTicket(p.generation->authority)==p.attachment_token.ticket;
        const auto observation=shared_attachment_queue_->ObserveSubmission();
        if(!owners||!shared_attachment_sequence_->CanRecord(SharedAttachmentBinding(*p.canonical),p.attachment_token,
                batch.active(),!batch.has_recording(),observation.available)) {
            ++statistics_.shared_attachment_refusals;return false;
        }
    }
    const auto pooled=std::static_pointer_cast<ScratchResources>(p.pooled_scratch);
    const auto shared=std::static_pointer_cast<ScratchResources>(p.shared_scratch);
    if(shared_scratch_batch_) {
        const bool owns=shared&&shared==shared_scratch_&&!pooled&&!p.scratch&&!p.descriptors&&shared->bytes==p.scratch_bytes;
        if(!shared_scratch_sequence_.CanRecord(&batch,p.canonical,batch.active(),batch.has_work_queue(),
           owns&&SharedScratchMatches(*shared,*p.canonical),owns)) {++statistics_.shared_scratch_refusals;return false;}
    }else if(shared||(reuse_resources_?(!pooled||p.scratch||p.descriptors||pooled->canonical!=p.canonical||
       !pooled->scratch||!pooled->descriptors||pooled->bytes!=p.scratch_bytes):
       (bool(pooled)||!p.scratch||!p.descriptors)))return false;
    if(pooled) {
        const auto& scratch=static_cast<const VulkanBuffer&>(*pooled->scratch);
        const auto& descriptors=static_cast<const VulkanDescriptorSet&>(*pooled->descriptors);
        if(pooled->owner!=this||pooled->budget.get()!=budget_.get()||scratch.device!=static_cast<VulkanDevice*>(&device_)||
           !scratch.vk||!scratch.allocation||scratch.allocationInfo.size!=p.scratch_bytes||scratch.desc.size!=kBytes*2||!descriptors.vk)return false;
    }
    auto& color=static_cast<VulkanTexture&>(*p.generation->color);auto& depth=static_cast<VulkanTexture&>(*p.generation->depth);
    if(!reuse_resources_&&!shares_attachments()&&!p.store&&(color.textureLayout!=RenderTextureLayout::UNKNOWN||depth.textureLayout!=RenderTextureLayout::UNKNOWN))
        throw std::logic_error("Retained opaque restore requires fresh images");
    const bool reused_restore_layout=(reuse_resources_||shares_attachments())&&!p.store&&color.textureLayout!=RenderTextureLayout::UNKNOWN;
    // Any exception while reserving/recording can follow previously accepted
    // FIFO work. Fail closed; never reuse this scratch after an uncertain tail.
    try {
    if(!batch.ReserveUploadBytes(p.scratch_bytes))return false;
    batch.KeepImage(p.generation->color,p.generation->color_bytes);batch.KeepImage(p.generation->depth,p.generation->depth_bytes);
    if(!batch.active())return false;
    if(shares_attachments()) {
        const auto observation=shared_attachment_queue_->ObserveSubmission();const auto& counts=batch.statistics();
        if(!shared_attachment_sequence_->BeginRecord(SharedAttachmentBinding(*p.canonical),p.attachment_token,
            {observation.accepted,counts.stages,counts.queued_lists},batch.active(),!batch.has_recording(),observation.available))
            throw std::logic_error("Shared attachment conversion lost its clean recording scope");
    }
    auto& commands=batch.BeginStage();auto& vk_commands=static_cast<VulkanCommandList&>(commands);
    auto* scratch=shared?shared->scratch.get():pooled?pooled->scratch.get():p.scratch.get();
    auto* descriptors=shared?shared->descriptors.get():pooled?pooled->descriptors.get():p.descriptors.get();
    if(shared)batch.KeepLease(p.shared_scratch);
    else if(pooled)batch.KeepLease(p.pooled_scratch);
    else {batch.Keep(std::move(p.scratch));batch.Keep(std::move(p.descriptors));}
    if(p.store) {
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&color,RenderTextureLayout::COPY_SOURCE));
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&depth,RenderTextureLayout::COPY_SOURCE));
        commands.barriers(RenderBarrierStage::COPY,RenderBufferBarrier(scratch,RenderBufferAccess::WRITE));
        Copy(vk_commands,static_cast<VulkanBuffer&>(*scratch),color,0,VK_IMAGE_ASPECT_COLOR_BIT,true);
        Copy(vk_commands,static_cast<VulkanBuffer&>(*scratch),depth,kBytes,VK_IMAGE_ASPECT_DEPTH_BIT,true);
    }
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(p.canonical,p.store?(RenderBufferAccess::READ|RenderBufferAccess::WRITE):RenderBufferAccess::READ));
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(scratch,p.store?RenderBufferAccess::READ:RenderBufferAccess::WRITE));
    commands.setComputePipelineLayout(layout_.get());commands.setComputeDescriptorSet(descriptors,0);
    commands.setPipeline(p.store?store_pipeline_.get():restore_pipeline_.get());commands.dispatch((kWidth+7)/8,(kHeight+7)/8,1);
    if(!p.store) {
        commands.barriers(RenderBarrierStage::COPY,RenderBufferBarrier(scratch,RenderBufferAccess::READ));
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&color,RenderTextureLayout::COPY_DEST));
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&depth,RenderTextureLayout::COPY_DEST));
        Copy(vk_commands,static_cast<VulkanBuffer&>(*scratch),color,0,VK_IMAGE_ASPECT_COLOR_BIT,false);
        Copy(vk_commands,static_cast<VulkanBuffer&>(*scratch),depth,kBytes,VK_IMAGE_ASPECT_DEPTH_BIT,false);
        commands.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&color,RenderTextureLayout::COLOR_WRITE));
        commands.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&depth,RenderTextureLayout::DEPTH_WRITE));
        ++statistics_.restores;
        if(pooled)++statistics_.pooled_restores;
        if(reused_restore_layout)++statistics_.reused_restore_layouts;
    }else {++statistics_.exports;if(pooled)++statistics_.pooled_exports;}
    if(shares_attachments()&&!shared_attachment_sequence_->Recorded(p.attachment_token,batch.statistics().stages,batch.has_recording()))
        throw std::logic_error("Shared attachment conversion did not record one exact stage");
    // Same single-use Prepared contract as moving the non-pooled uniques.
    // A pressure-only false retains the lease; accepted work consumes it.
    if(pooled)p.pooled_scratch.reset();
    if(shared) {
        if(p.store)++statistics_.shared_scratch_exports;else ++statistics_.shared_scratch_restores;
        p.shared_scratch.reset();
    }
    return true;
    }catch(...) {
        if(shares_attachments())FailSharedAttachments();
        if(shared_scratch_batch_) {shared_scratch_sequence_.FailRecording();++statistics_.shared_scratch_recording_failures;}
        throw;
    }
}
}
