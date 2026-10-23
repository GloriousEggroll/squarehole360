#include "fused_packed_atlas_resolve.h"
#include "transfer_batch.h"
#include "plume_vulkan.h"
#include <stdexcept>
namespace gow3::renderer {
using namespace plume;
namespace {
constexpr uint32_t width=1120,height=688,packed_bytes=2048*5120;
RenderDescriptorRange PackedRange(){return {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1};}
bool ShaderValid(std::span<const uint32_t> code){return code.size()>=5&&code.front()==0x07230203&&code.size_bytes()<=1024*1024;}
}
NativeFusedPackedAtlasResolve::NativeFusedPackedAtlasResolve(RenderDevice& device,RenderCommandQueue& queue,
        std::span<const uint32_t> vertex,std::span<const uint32_t> pixel,NativeTransferBatch* batch)
    :device_(device),batch_(batch) {
    if(!ShaderValid(vertex)||!ShaderValid(pixel))return;
    try {
        auto& vk=static_cast<VulkanDevice&>(device_);VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(vk.physicalDevice,&properties);
        if(properties.limits.maxColorAttachments<2||properties.limits.maxStorageBufferRange<packed_bytes)return;
        vertex_=device_.createShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
        pixel_=device_.createShader(pixel.data(),pixel.size_bytes(),"main",RenderShaderFormat::SPIRV);
        const auto range=PackedRange();const RenderDescriptorSetDesc descriptor(&range,1);
        layout_=device_.createPipelineLayout(RenderPipelineLayoutDesc(nullptr,0,&descriptor,1));
        if(!vertex_||!pixel_||!layout_||
           !static_cast<VulkanShader&>(*vertex_).vk||!static_cast<VulkanShader&>(*pixel_).vk||
           !static_cast<VulkanPipelineLayout&>(*layout_).vk)return;
        RenderGraphicsPipelineDesc p;
        p.pipelineLayout=layout_.get();p.vertexShader=vertex_.get();p.pixelShader=pixel_.get();
        p.renderTargetCount=2;p.renderTargetFormat[0]=RenderFormat::R32_FLOAT;
        p.renderTargetFormat[1]=RenderFormat::R32G32B32A32_FLOAT;
        p.renderTargetBlend[0]=p.renderTargetBlend[1]=RenderBlendDesc::Copy();p.depthClipEnabled=true;
        auto pipeline=device_.createGraphicsPipeline(p);
        if(pipeline&&static_cast<VulkanGraphicsPipeline&>(*pipeline).vk)pipeline_=std::move(pipeline);
    }catch(...) {pipeline_.reset();} // Optional preparation, no accepted commands.
}
bool NativeFusedPackedAtlasResolve::Resolve(RenderBuffer& packed,RenderTexture& destination,
        const RenderRect& r,std::shared_ptr<RenderTexture>& stencil) {
    // The full exact layout is also admitted by this low-level primitive for
    // exhaustive codec validation. Scene retains its five captured rectangles.
    const bool full=!r.left&&!r.top&&r.right==int32_t(width)&&r.bottom==int32_t(height);
    if(!supported()||!batch_||!batch_->active()||stencil||(!full&&!NativeFusedPackedAtlasRectangleAllowed(
        {r.left,r.top,r.right,r.bottom},{r.left,r.top,r.right,r.bottom})))return false;
    const auto& source=static_cast<const VulkanBuffer&>(packed);
    const auto& output=static_cast<const VulkanTexture&>(destination);const auto& d=output.desc;
    const auto* device=static_cast<const VulkanDevice*>(&device_);
    if(source.device!=device||!source.vk||!source.allocation||source.desc.size!=packed_bytes||!(source.desc.flags&RenderBufferFlag::STORAGE)||
       output.device!=device||!output.vk||!output.imageView||!output.ownership||!output.allocation||
       output.imageFormat!=VK_FORMAT_R32_SFLOAT||d.format!=RenderFormat::R32_FLOAT||
       d.width!=width||d.height!=height||d.dimension!=RenderTextureDimension::TEXTURE_2D||
       d.depth!=1||d.mipLevels!=1||d.arraySize!=1||
       d.multisampling.sampleCount!=RenderSampleCount::COUNT_1||d.multisampling.sampleLocationsEnabled||
       !(d.flags&RenderTextureFlag::RENDER_TARGET))return false;
    std::shared_ptr<RenderTexture> scratch;
    std::unique_ptr<RenderDescriptorSet> descriptor;std::unique_ptr<RenderFramebuffer> framebuffer;
    // All optional failure exits precede KeepImage/BeginStage. After this block
    // allocation, recording or queue failures propagate through original fatal
    // batch handling; they must not replay already accepted destination writes.
    try {
        scratch=device_.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R32G32B32A32_FLOAT));
        if(!scratch)return false;
        const auto& image=static_cast<const VulkanTexture&>(*scratch);
        if(!image.vk||!image.imageView||!image.ownership||!image.allocation||!image.allocationInfo.size||
           image.allocationInfo.size>NativeTransferBatch::kMaxImageBytes)return false;
        const auto range=PackedRange();descriptor=device_.createDescriptorSet(RenderDescriptorSetDesc(&range,1));
        if(!descriptor||!static_cast<const VulkanDescriptorSet&>(*descriptor).vk)return false;
        descriptor->setBuffer(0,&packed,packed_bytes);
        std::array<const RenderTexture*,2> attachments{&destination,scratch.get()};
        RenderFramebufferDesc fd;fd.colorAttachments=attachments.data();fd.colorAttachmentsCount=2;
        framebuffer=device_.createFramebuffer(fd);
        if(!framebuffer||!static_cast<const VulkanFramebuffer&>(*framebuffer).vk||
           !static_cast<const VulkanFramebuffer&>(*framebuffer).renderPass)return false;
    }catch(...) {return false;}
    // KeepImage can fence a prefix and switch this scope to synchronous mode.
    // In that case refuse BEFORE new MRT commands; the unchanged Native path
    // handles the pressure. Every accepted MRT has real packet-owned resources.
    batch_->KeepImage(scratch,static_cast<const VulkanTexture&>(*scratch).allocationInfo.size);
    if(!batch_->active())return false;
    auto* commands=&batch_->BeginStage();
    auto* fd=batch_->Keep(std::move(framebuffer));auto* ds=batch_->Keep(std::move(descriptor));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(&packed,RenderBufferAccess::READ));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&destination,RenderTextureLayout::COLOR_WRITE));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(scratch.get(),RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(fd);commands->setViewports(RenderViewport(0,0,width,height));commands->setScissors(r);
    commands->setGraphicsPipelineLayout(layout_.get());commands->setGraphicsDescriptorSet(ds,0);commands->setPipeline(pipeline_.get());
    commands->drawInstanced(3,1,0,0);
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&destination,RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(scratch.get(),RenderTextureLayout::SHADER_READ));
    stencil=std::move(scratch);return true;
}
}
