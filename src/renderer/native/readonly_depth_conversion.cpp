#include "edram_extent.h"
#include "readonly_depth_conversion.h"
#include "resolve_memory.h"
#include "transfer_batch.h"
#include "plume_vulkan.h"
#include <algorithm>
#include <array>
#include <stdexcept>
namespace gow3::renderer {
using namespace plume;
namespace {
const std::array<RenderDescriptorRange,2> ranges{{
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
}
NativeReadonlyDepthConversion::NativeReadonlyDepthConversion(RenderDevice& device,std::span<const uint32_t> compute,bool read_only_pass):device_(device) {
    auto& vk=static_cast<VulkanDevice&>(device);VkFormatProperties properties{};
    read_only_pass_=read_only_pass&&vk.storeOpNoneSupported;
    vkGetPhysicalDeviceFormatProperties(vk.physicalDevice,VK_FORMAT_D32_SFLOAT,&properties);
    constexpr auto needed=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((properties.optimalTilingFeatures&needed)!=needed)return;
    const RenderDescriptorSetDesc descriptors(ranges.data(),ranges.size());
    const RenderPushConstantRange push(0,0,0,16,RenderShaderStageFlag::COMPUTE);
    layout_=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&descriptors,1));
    compute_=device.createShader(compute.data(),compute.size_bytes(),"main",RenderShaderFormat::SPIRV);
    if(!layout_||!static_cast<VulkanPipelineLayout&>(*layout_).vk||!compute_||!static_cast<VulkanShader&>(*compute_).vk)return;
    pipeline_=device.createComputePipeline(RenderComputePipelineDesc(layout_.get(),compute_.get(),8,8,1));
    supported_=pipeline_&&static_cast<VulkanComputePipeline&>(*pipeline_).vk;
}
std::unique_ptr<NativeReadonlyDepthConversion::Prepared> NativeReadonlyDepthConversion::Prepare(
        RenderBuffer& source,const SurfaceLayout& s,uint64_t version,uint64_t generation) {
    if(!supported_||!version||!generation||!s.depth||s.format!=1||!s.width||!s.height||s.width>8192||s.height>8192||
       !s.pitch||s.pitch>16383||s.base>=2048)return {};
    const uint64_t columns=(uint64_t(s.width)+79)/80,rows=(uint64_t(s.height)+15)/16,pitch=(uint64_t(s.pitch)+79)/80;
    if(pitch<columns||(rows-1)*pitch+columns-1>=2048||!NativeEdramLegacyWrapSafe(s.base,(rows-1)*pitch+columns-1))return {};
    const uint64_t logical=uint64_t(s.width)*s.height*4;
    // Conservative candidate-size admission before any image or scratch
    // allocation. Actual retained accounting below still uses VMA size.
    if(logical>kMaximumRetainedImageBytes) {++statistics_.budget_refusals;return {};}
    auto& src=static_cast<VulkanBuffer&>(source);
    if(src.device!=static_cast<VulkanDevice*>(&device_)||!src.vk||src.desc.size!=2048u*5120)return {};
    try {
        auto out=std::make_unique<Prepared>();out->source=&source;
        out->generation=std::make_shared<NativeReadonlyDepthGeneration>(generation,
            NativeReadonlyDepthLayoutKey{s.width,s.height,s.pitch,s.base,s.format,uint32_t(s.depth)},version);
        auto image=device_.createTexture(RenderTextureDesc::DepthTarget(s.width,s.height,RenderFormat::D32_FLOAT));
        if(!image||!static_cast<VulkanTexture&>(*image).vk||!static_cast<VulkanTexture&>(*image).allocation) {
            ++statistics_.allocation_refusals;return {};
        }
        out->image_bytes=static_cast<const VulkanTexture&>(*image).allocationInfo.size;
        if(!out->image_bytes||out->image_bytes>kMaximumRetainedImageBytes) {
            ++statistics_.budget_refusals;return {};
        }
        // Shared-pointer construction invokes this deleter if its control-block
        // allocation fails. The image budget includes every transfer/draw owner,
        // not merely the current cache entry; no hidden uncharged old images.
        const auto bytes=out->image_bytes;auto budget=budget_;
        auto held=budget->bytes.load(std::memory_order_relaxed);
        do {
            if(held>kMaximumRetainedImageBytes-bytes) {++statistics_.budget_refusals;return {};}
        }while(!budget->bytes.compare_exchange_weak(held,held+bytes,std::memory_order_relaxed));
        auto peak=budget->peak.load(std::memory_order_relaxed);
        while(peak<held+bytes&&!budget->peak.compare_exchange_weak(peak,held+bytes,std::memory_order_relaxed)) {}
        // This bounds retained accepted owners. The just-created local image
        // existed before its actual VMA size was known; rejected local
        // allocation peaks are deliberately not claimed by this counter.
        out->generation->image=std::shared_ptr<RenderTexture>(image.release(),[budget,bytes](RenderTexture* value)noexcept {
            delete value;budget->bytes.fetch_sub(bytes,std::memory_order_relaxed);
        });
        out->generation->read_only_pass=read_only_pass_;
        RenderFramebufferDesc framebuffer;framebuffer.depthAttachment=out->generation->image.get();
        // Explicit LOAD retains the converted values. STORE_NONE and a
        // read-only layout remove attachment writes, including implicit stores.
        // Unsupported devices keep the existing LOAD+STORE Native D32 path.
        framebuffer.depthAttachmentReadOnly=read_only_pass_;
        framebuffer.depthAttachmentPreserveReadOnly=read_only_pass_;
        out->generation->framebuffer=device_.createFramebuffer(framebuffer);
        if(!out->generation->framebuffer||!static_cast<VulkanFramebuffer&>(*out->generation->framebuffer).vk||
           !static_cast<VulkanFramebuffer&>(*out->generation->framebuffer).renderPass) {
            ++statistics_.allocation_refusals;return {};
        }
        out->scratch=device_.createBuffer(RenderBufferDesc::DefaultBuffer(logical,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        if(!out->scratch||!static_cast<VulkanBuffer&>(*out->scratch).vk||!static_cast<VulkanBuffer&>(*out->scratch).allocation) {
            ++statistics_.allocation_refusals;return {};
        }
        out->scratch_bytes=static_cast<const VulkanBuffer&>(*out->scratch).allocationInfo.size;
        if(out->scratch_bytes<logical||out->scratch_bytes>NativeTransferBatch::kMaxImageBytes-out->image_bytes) {
            ++statistics_.budget_refusals;return {};
        }
        out->descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),ranges.size()));
        if(!out->descriptors||!static_cast<VulkanDescriptorSet&>(*out->descriptors).vk) {
            ++statistics_.allocation_refusals;return {};
        }
        out->descriptors->setBuffer(0,&source);out->descriptors->setBuffer(1,out->scratch.get(),logical);
        ++statistics_.preparations;return out;
    }catch(const std::exception&) {++statistics_.allocation_refusals;return {};}
}
bool NativeReadonlyDepthConversion::Record(Prepared& prepared,NativeTransferBatch& batch) {
    if(!supported_||!prepared.source||!prepared.generation||!prepared.scratch||!prepared.descriptors||!batch.active())return false;
    auto& image=static_cast<VulkanTexture&>(*prepared.generation->image);
    if(image.textureLayout!=RenderTextureLayout::UNKNOWN||image.barrierStages!=RenderBarrierStage::NONE)
        throw std::logic_error("Readonly depth conversion destination is not a fresh generation");
    if(!batch.ReserveUploadBytes(prepared.scratch_bytes))return false;
    batch.KeepImage(prepared.generation->image,prepared.image_bytes);if(!batch.active())return false;
    auto& commands=batch.BeginStage();auto* scratch=prepared.scratch.get();auto* descriptors=prepared.descriptors.get();
    batch.Keep(std::move(prepared.scratch));batch.Keep(std::move(prepared.descriptors));
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(prepared.source,RenderBufferAccess::READ));
    const auto& key=prepared.generation->version.layout();
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(scratch,RenderBufferAccess::WRITE));
    const std::array<uint32_t,4> push{key[0],key[1],key[2],key[3]};
    commands.setComputePipelineLayout(layout_.get());commands.setComputePushConstants(0,push.data());
    commands.setComputeDescriptorSet(descriptors,0);commands.setPipeline(pipeline_.get());commands.dispatch((key[0]+7)/8,(key[1]+7)/8,1);
    commands.barriers(RenderBarrierStage::COPY,RenderBufferBarrier(scratch,RenderBufferAccess::READ));
    commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&image,RenderTextureLayout::COPY_DEST));
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.imageExtent={key[0],key[1],1};
    vkCmdCopyBufferToImage(static_cast<VulkanCommandList&>(commands).vk,static_cast<VulkanBuffer&>(*scratch).vk,
        image.vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    commands.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&image,
        prepared.generation->read_only_pass?RenderTextureLayout::DEPTH_READ:RenderTextureLayout::DEPTH_WRITE));
    ++statistics_.conversions;statistics_.scratch_bytes+=prepared.scratch_bytes;return true;
}
}
