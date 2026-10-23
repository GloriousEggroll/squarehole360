#include "retained_fp24_depth_writer.h"
#include "transfer_batch.h"
#include "plume_vulkan.h"
#include <array>
#include <stdexcept>

namespace gow3::renderer {
using namespace plume;
namespace {
constexpr uint32_t kWidth=1280,kHeight=720,kBytes=kWidth*kHeight*4;
const std::array<RenderDescriptorRange,2> kRanges{{
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
void CopyDepth(VulkanCommandList& commands,VulkanBuffer& scratch,VulkanTexture& depth,bool store) {
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.imageExtent={kWidth,kHeight,1};
    if(store)vkCmdCopyImageToBuffer(commands.vk,depth.vk,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scratch.vk,1,&copy);
    else vkCmdCopyBufferToImage(commands.vk,scratch.vk,depth.vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
}
}
struct NativeRetainedFp24DepthScratch {
    const NativeRetainedFp24DepthWriter* owner=nullptr;
    std::shared_ptr<NativeRetainedOpaqueResourceBudget> budget;
    std::unique_ptr<RenderBuffer> buffer;
    std::unique_ptr<RenderDescriptorSet> descriptors;
    RenderBuffer* canonical=nullptr;
    uint64_t bytes=0;
    ~NativeRetainedFp24DepthScratch(){descriptors.reset();buffer.reset();if(budget)budget->Release(bytes);}
};
NativeRetainedFp24DepthWriter::NativeRetainedFp24DepthWriter(RenderDevice& device,
        std::span<const uint32_t> restore,std::span<const uint32_t> store):device_(device) {
    auto& vk=static_cast<VulkanDevice&>(device);VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(vk.physicalDevice,VK_FORMAT_D32_SFLOAT,&properties);
    constexpr auto needed=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkImageFormatProperties image{};
    constexpr auto usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
        VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if((properties.optimalTilingFeatures&needed)!=needed||
       vkGetPhysicalDeviceImageFormatProperties(vk.physicalDevice,VK_FORMAT_D32_SFLOAT,VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,usage,0,&image)!=VK_SUCCESS||image.maxExtent.width<kWidth||image.maxExtent.height<kHeight||
       !(image.sampleCounts&VK_SAMPLE_COUNT_1_BIT)||!image.maxArrayLayers||
       restore.size()<5||store.size()<5||restore.front()!=0x07230203||store.front()!=0x07230203)return;
    const RenderDescriptorSetDesc descriptor(kRanges.data(),kRanges.size());
    layout_=device.createPipelineLayout(RenderPipelineLayoutDesc(nullptr,0,&descriptor,1));
    restore_=device.createShader(restore.data(),restore.size_bytes(),"main",RenderShaderFormat::SPIRV);
    store_=device.createShader(store.data(),store.size_bytes(),"main",RenderShaderFormat::SPIRV);
    if(!layout_||!restore_||!store_||!static_cast<VulkanPipelineLayout&>(*layout_).vk||
       !static_cast<VulkanShader&>(*restore_).vk||!static_cast<VulkanShader&>(*store_).vk)return;
    restore_pipeline_=device.createComputePipeline(RenderComputePipelineDesc(layout_.get(),restore_.get(),8,8,1));
    store_pipeline_=device.createComputePipeline(RenderComputePipelineDesc(layout_.get(),store_.get(),8,8,1));
    supported_=restore_pipeline_&&store_pipeline_&&static_cast<VulkanComputePipeline&>(*restore_pipeline_).vk&&
        static_cast<VulkanComputePipeline&>(*store_pipeline_).vk;
}
bool NativeRetainedFp24DepthWriter::ValidateGeneration(const NativeRetainedFp24DepthGeneration& g)const noexcept {
    if(g.conversion_owner!=this||!g.depth||!g.scratch||!g.framebuffer||!g.authority.present()||g.authority.failed())return false;
    const auto* device=static_cast<const VulkanDevice*>(&device_);
    const auto& image=static_cast<const VulkanTexture&>(*g.depth);const auto& desc=image.desc;
    const auto& framebuffer=static_cast<const VulkanFramebuffer&>(*g.framebuffer);
    if(image.device!=device||!image.vk||!image.allocation||!g.depth_bytes||image.allocationInfo.size!=g.depth_bytes||
       g.depth_bytes>kMaximumRetainedBytes||desc.format!=RenderFormat::D32_FLOAT||desc.width!=kWidth||desc.height!=kHeight||
       desc.depth!=1||desc.mipLevels!=1||desc.arraySize!=1||desc.dimension!=RenderTextureDimension::TEXTURE_2D||
       desc.multisampling.sampleCount!=1||framebuffer.device!=device||!framebuffer.vk||!framebuffer.renderPass||
       framebuffer.width!=kWidth||framebuffer.height!=kHeight||!framebuffer.colorAttachments.empty()||
       framebuffer.depthAttachment!=g.depth.get()||framebuffer.depthAttachmentReadOnly)return false;
    const auto scratch=g.scratch;
    if(scratch->owner!=this||scratch->budget.get()!=budget_.get()||!scratch->buffer||!scratch->descriptors||
       !scratch->canonical||scratch->bytes>kMaximumRetainedBytes-g.depth_bytes)return false;
    const auto& buffer=static_cast<const VulkanBuffer&>(*scratch->buffer);
    const auto& canonical=static_cast<const VulkanBuffer&>(*scratch->canonical);
    return buffer.device==device&&buffer.vk&&buffer.allocation&&buffer.desc.size==kBytes&&
        buffer.allocationInfo.size==scratch->bytes&&scratch->bytes>=kBytes&&
        static_cast<const VulkanDescriptorSet&>(*scratch->descriptors).vk&&
        canonical.device==device&&canonical.vk&&canonical.desc.size==2048u*5120;
}
std::unique_ptr<NativeRetainedFp24DepthWriter::Prepared> NativeRetainedFp24DepthWriter::PrepareRestore(
        RenderBuffer& canonical,uint64_t generation,uint64_t depth_version) {
    const auto& src=static_cast<const VulkanBuffer&>(canonical);
    if(!supported_||!generation||!depth_version||src.device!=static_cast<VulkanDevice*>(&device_)||
       !src.vk||src.desc.size!=2048u*5120)return {};
    try {
        auto g=std::make_shared<NativeRetainedFp24DepthGeneration>();g->conversion_owner=this;
        auto image=device_.createTexture(RenderTextureDesc::DepthTarget(kWidth,kHeight,RenderFormat::D32_FLOAT));
        if(!image||!static_cast<const VulkanTexture&>(*image).vk||!static_cast<const VulkanTexture&>(*image).allocation)
            {++statistics_.allocation_refusals;return {};}
        g->depth_bytes=static_cast<const VulkanTexture&>(*image).allocationInfo.size;
        if(!budget_->TryCharge(g->depth_bytes,kMaximumRetainedBytes)){++statistics_.budget_refusals;return {};}
        auto budget=budget_;const auto image_bytes=g->depth_bytes;
        g->depth=std::shared_ptr<RenderTexture>(image.release(),[budget,image_bytes](RenderTexture* p)noexcept{
            delete p;budget->Release(image_bytes);
        });
        auto scratch=std::make_shared<NativeRetainedFp24DepthScratch>();scratch->owner=this;scratch->canonical=&canonical;
        scratch->buffer=device_.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        if(!scratch->buffer||!static_cast<const VulkanBuffer&>(*scratch->buffer).vk||
           !static_cast<const VulkanBuffer&>(*scratch->buffer).allocation){++statistics_.allocation_refusals;return {};}
        scratch->bytes=static_cast<const VulkanBuffer&>(*scratch->buffer).allocationInfo.size;
        if(scratch->bytes<kBytes||!budget_->TryCharge(scratch->bytes,kMaximumRetainedBytes))
            {++statistics_.budget_refusals;return {};}
        scratch->budget=budget_;
        scratch->descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(kRanges.data(),kRanges.size()));
        if(!scratch->descriptors||!static_cast<const VulkanDescriptorSet&>(*scratch->descriptors).vk)
            {++statistics_.allocation_refusals;return {};}
        scratch->descriptors->setBuffer(0,&canonical);scratch->descriptors->setBuffer(1,scratch->buffer.get(),kBytes);
        g->scratch=scratch;
        RenderFramebufferDesc fd;fd.depthAttachment=g->depth.get();fd.depthAttachmentReadOnly=false; // Actual Plume LOAD+STORE.
        g->framebuffer=device_.createFramebuffer(fd);
        if(!g->framebuffer){++statistics_.allocation_refusals;return {};}
        g->authority.Admit(generation,kNativeRetainedFp24DepthLayout,depth_version);
        if(!ValidateGeneration(*g)){++statistics_.allocation_refusals;return {};}
        auto out=std::make_unique<Prepared>();out->canonical=&canonical;out->generation=std::move(g);
        ++statistics_.preparations;return out;
    }catch(const std::exception&){++statistics_.allocation_refusals;return {};}
}
std::unique_ptr<NativeRetainedFp24DepthWriter::Prepared> NativeRetainedFp24DepthWriter::PrepareStore(
        RenderBuffer& canonical,const std::shared_ptr<NativeRetainedFp24DepthGeneration>& generation) {
    try {
        if(!supported_||!generation||!ValidateGeneration(*generation))return {};
        const auto scratch=generation->scratch;
        if(scratch->canonical!=&canonical)return {};
        auto out=std::make_unique<Prepared>();out->canonical=&canonical;out->generation=generation;out->store=true;return out;
    }catch(const std::exception&){++statistics_.allocation_refusals;return {};}
}
bool NativeRetainedFp24DepthWriter::Record(Prepared& p,NativeTransferBatch& batch) {
    if(!supported_||p.consumed||!p.canonical||!p.generation||!ValidateGeneration(*p.generation)||!batch.active())return false;
    const auto scratch=p.generation->scratch;
    if(scratch->canonical!=p.canonical)return false;
    auto& image=static_cast<VulkanTexture&>(*p.generation->depth);
    if(!p.store&&(image.textureLayout!=RenderTextureLayout::UNKNOWN||image.barrierStages!=RenderBarrierStage::NONE))
        throw std::logic_error("Retained FP24 depth restore requires a fresh image");
    if(!batch.ReserveUploadBytes(scratch->bytes))return false;
    batch.KeepImage(p.generation->depth,p.generation->depth_bytes);if(!batch.active())return false;
    auto& commands=batch.BeginStage();
    batch.KeepLease(p.generation->scratch); // Requires this recorded stage; precedes every GPU command.
    auto& vk=static_cast<VulkanCommandList&>(commands);
    if(p.store) {
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&image,RenderTextureLayout::COPY_SOURCE));
        commands.barriers(RenderBarrierStage::COPY,RenderBufferBarrier(scratch->buffer.get(),RenderBufferAccess::WRITE));
        CopyDepth(vk,static_cast<VulkanBuffer&>(*scratch->buffer),image,true);
    }
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(p.canonical,p.store?
        RenderBufferAccess::READ|RenderBufferAccess::WRITE:RenderBufferAccess::READ));
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(scratch->buffer.get(),p.store?RenderBufferAccess::READ:RenderBufferAccess::WRITE));
    commands.setComputePipelineLayout(layout_.get());commands.setComputeDescriptorSet(scratch->descriptors.get(),0);
    commands.setPipeline(p.store?store_pipeline_.get():restore_pipeline_.get());commands.dispatch((kWidth+7)/8,(kHeight+7)/8,1);
    if(!p.store) {
        commands.barriers(RenderBarrierStage::COPY,RenderBufferBarrier(scratch->buffer.get(),RenderBufferAccess::READ));
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&image,RenderTextureLayout::COPY_DEST));
        CopyDepth(vk,static_cast<VulkanBuffer&>(*scratch->buffer),image,false);
        commands.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&image,RenderTextureLayout::DEPTH_WRITE));
        ++statistics_.restores;
    }else ++statistics_.exports;
    p.consumed=true;return true;
}
}
