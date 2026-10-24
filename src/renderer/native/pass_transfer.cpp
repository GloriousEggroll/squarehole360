#include "pass_transfer.h"
#include "synchronous_gpu_timing.h"
#include "transfer_batch.h"
#include "plume_vulkan.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace gow3::renderer {
using namespace plume;
namespace {
RenderDescriptorRange ImageRange() { return {RenderDescriptorRangeType::TEXTURE, 0, 1}; }
void CheckRect(const RenderTexture& image, const RenderRect& r) {
    const auto& d = static_cast<const VulkanTexture&>(image).desc;
    if (r.left < 0 || r.top < 0 || r.right > int32_t(d.width) || r.bottom > int32_t(d.height) || r.isEmpty())
        throw std::runtime_error("Native pass rectangle exceeds its image: (" + std::to_string(r.left) + "," +
            std::to_string(r.top) + ")-(" + std::to_string(r.right) + "," + std::to_string(r.bottom) + ") in " +
            std::to_string(d.width) + "x" + std::to_string(d.height));
}
}
PassTransfer::PassTransfer(RenderDevice& device, RenderCommandQueue& queue,
                           std::span<const uint32_t> vertex, std::span<const uint32_t> pixel,
                           std::span<const uint32_t> depth_pixel, NativeTransferBatch* batch,
                           std::span<const uint32_t> stencil_export_pixel)
    : batch_(batch),device_(device), queue_(queue) {
    commands_ = queue_.createCommandList(); fence_ = device_.createCommandFence();
    vertex_ = device_.createShader(vertex.data(), vertex.size_bytes(), "main", RenderShaderFormat::SPIRV);
    pixel_ = device_.createShader(pixel.data(), pixel.size_bytes(), "main", RenderShaderFormat::SPIRV);
    if(!depth_pixel.empty())depth_pixel_=device_.createShader(depth_pixel.data(),depth_pixel.size_bytes(),"main",RenderShaderFormat::SPIRV);
    if(!stencil_export_pixel.empty())try {
        auto shader=device_.createShader(stencil_export_pixel.data(),stencil_export_pixel.size_bytes(),
            "main",RenderShaderFormat::SPIRV);
        if(!shader||!static_cast<VulkanShader*>(shader.get())->vk)
            throw std::runtime_error("Cannot create optional stencil export shader");
        stencil_export_pixel_=std::move(shader);
    } catch(...) {stencil_export_failed_=true;++restore_statistics_.export_failures;}
    const RenderPushConstantRange push(0, 0, 0, 20, RenderShaderStageFlag::PIXEL);
    const auto range = ImageRange(); const RenderDescriptorSetDesc descriptor(&range, 1);
    layout_ = device_.createPipelineLayout(RenderPipelineLayoutDesc(&push, 1, &descriptor, 1));
}
PassTransfer::~PassTransfer()=default;
void PassTransfer::SetRenderPassCacheEnabled(bool enabled) {
    if(render_pass_cache_configured_||render_pass_dispatch_started_)
        throw std::logic_error("Pass transfer render-pass cache must be configured once before dispatch");
    auto prepared=enabled?std::make_unique<VulkanFramebufferRenderPassCache>(static_cast<VulkanDevice&>(device_).vk):nullptr;
    render_pass_cache_=std::move(prepared);render_pass_cache_configured_=true;
}
PassTransfer::RenderPassStatistics PassTransfer::RenderPassCacheStatistics() const {
    RenderPassStatistics out;
    if(render_pass_cache_) {
        const auto s=render_pass_cache_->statistics();out.enabled=true;
        out.requests=s.requests;out.hits=s.hits;out.misses=s.misses;out.creations=s.creations;
        out.unsupported_bypasses=s.unsupported_bypasses;out.capacity_bypasses=s.capacity_bypasses;
        out.create_failures=s.create_failures;out.cached_entries=s.cached_entries;out.maximum_entries=s.maximum_entries;
    }
    return out;
}
std::unique_ptr<RenderFramebuffer> PassTransfer::CreateFramebuffer(const RenderFramebufferDesc& desc) {
    render_pass_dispatch_started_=true;
    if(!render_pass_cache_)return device_.createFramebuffer(desc);
    return std::make_unique<VulkanFramebuffer>(&static_cast<VulkanDevice&>(device_),desc,render_pass_cache_.get());
}
void PassTransfer::SetDescriptorPoolEnabled(bool enabled) {
    if(descriptor_pool_configured_||descriptor_dispatch_started_)
        throw std::logic_error("Pass transfer descriptor pool must be configured once before dispatch");
    auto prepared=enabled?std::make_unique<DescriptorPool>():nullptr;
    descriptor_pool_=std::move(prepared);descriptor_pool_configured_=true;
}
PassTransfer::DescriptorPool::Statistics PassTransfer::DescriptorPoolStatistics() const {
    auto stats=descriptor_pool_?descriptor_pool_->statistics():DescriptorPool::Statistics{};
    stats.synchronous_bypasses=descriptor_synchronous_bypasses_;return stats;
}
std::unique_ptr<RenderDescriptorSet> PassTransfer::AcquireDescriptor(
        std::shared_ptr<RenderDescriptorSet>& lease) {
    descriptor_dispatch_started_=true;
    const auto range=ImageRange();
    if(descriptor_pool_&&batch_&&batch_->active()&&batch_->has_work_queue()) {
        lease=descriptor_pool_->Acquire(0,[&] {
            auto created=device_.createDescriptorSet(RenderDescriptorSetDesc(&range,1));
            const auto* native=created?static_cast<const VulkanDescriptorSet*>(created.get()):nullptr;
            if(!native||!native->vk||!native->descriptorPool||!native->setLayout||!native->setLayout->vk)
                throw std::runtime_error("Pass transfer pooled descriptor allocation failed");
            return created;
        });
        return {};
    }
    // Preserve standalone and suspended-scope submission/lifetime behavior.
    if(descriptor_pool_)++descriptor_synchronous_bypasses_;
    return device_.createDescriptorSet(RenderDescriptorSetDesc(&range,1));
}
void PassTransfer::Submit(NativeSynchronousGpuTimingScope* presentation_scope) {
    if(presentation_scope)presentation_scope->End(*commands_);
    commands_->end();if(presentation_scope)presentation_scope->BeforeSubmit();
    queue_.executeCommandLists(commands_.get(),fence_.get());
    if(presentation_scope)presentation_scope->BeforeFenceWait();
    queue_.waitForCommandFence(fence_.get());
    if(presentation_scope)presentation_scope->FenceCompleted();
}
void PassTransfer::Clear(RenderTexture* color, RenderTexture* depth, bool clear_color,
                         bool clear_depth, bool clear_stencil, const RenderRect& rectangle,
                         const RenderColor& value, float depth_value, uint32_t stencil_value) {
    if (rectangle.isEmpty()) return;
    if (clear_color && !color) throw std::runtime_error("Clear requires a color attachment");
    if ((clear_depth || clear_stencil) && !depth) throw std::runtime_error("Clear requires a depth attachment");
    if (clear_color) CheckRect(*color, rectangle);
    if (clear_depth || clear_stencil) CheckRect(*depth, rectangle);
    if (!std::isfinite(depth_value) || depth_value < 0 || depth_value > 1)
        throw std::runtime_error("Invalid native clear depth");
    const RenderTexture* attachment = color;
    RenderFramebufferDesc fd;
    fd.colorAttachments = clear_color ? &attachment : nullptr; fd.colorAttachmentsCount = clear_color ? 1 : 0;
    fd.depthAttachment = clear_depth || clear_stencil ? depth : nullptr;
    if (!fd.colorAttachmentsCount && !fd.depthAttachment) return;
    auto framebuffer = CreateFramebuffer(fd);
    const bool batched=batch_&&batch_->active();
    auto* commands=batched?&batch_->BeginStage():commands_.get();
    auto* framebuffer_ptr=framebuffer.get();
    if(batched)batch_->Keep(std::move(framebuffer));
    if(!batched){if(batch_)batch_->Flush();commands->begin();}
    if (clear_color) commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color, RenderTextureLayout::COLOR_WRITE));
    if (fd.depthAttachment) commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth, RenderTextureLayout::DEPTH_WRITE));
    commands->setFramebuffer(framebuffer_ptr);
    if (clear_color) commands->clearColor(0, value, &rectangle, 1);
    if (fd.depthAttachment) commands->clearDepthStencil(clear_depth, clear_stencil, depth_value, stencil_value, &rectangle, 1);
    if(!batched)Submit();
}
void PassTransfer::Resolve(RenderTexture& source, RenderTexture& destination,
                           const RenderRect& src, const RenderRect& dst, int32_t exponent_bias, Conversion conversion,
                           uint32_t channel_swizzle,const NativeSynchronousGpuTimingContext* presentation_timing) {
    if (&source == &destination) throw std::runtime_error("Resolve source aliases its destination image");
    CheckRect(source, src); CheckRect(destination, dst);
    const int32_t factor = conversion == Conversion::Average2x2 ? 2 : 1;
    if (src.right - src.left != (dst.right - dst.left) * factor || src.bottom - src.top != (dst.bottom - dst.top) * factor ||
        (factor == 2 && ((src.left | src.top) & 1)) ||
        exponent_bias < -32 || exponent_bias > 31) throw std::runtime_error("Invalid native resolve extent/exponent");
    if(channel_swizzle>>12)throw std::runtime_error("Invalid native channel swizzle");
    for(unsigned c=0;c<4;++c)if(((channel_swizzle>>(c*3))&7)>5)
        throw std::runtime_error("Invalid native channel swizzle");
    const auto& source_desc = static_cast<VulkanTexture&>(source).desc;
    const auto& dest_desc = static_cast<VulkanTexture&>(destination).desc;
    if (source_desc.multisampling.sampleCount != 1 || dest_desc.multisampling.sampleCount != 1 ||
        (dest_desc.flags & RenderTextureFlag::DEPTH_TARGET)) throw std::runtime_error("Unsupported native resolve image type");
    std::shared_ptr<RenderDescriptorSet> descriptor_lease;
    auto descriptor=AcquireDescriptor(descriptor_lease);
    auto* descriptor_ptr=descriptor_lease?descriptor_lease.get():descriptor.get();
    auto view = source.createTextureView(RenderTextureViewDesc::Texture2D(source_desc.format));
    // The sole binding is always replaced before recording while the lease is exclusive.
    descriptor_ptr->setTexture(0, &source, RenderTextureLayout::SHADER_READ, view.get());
    const RenderTexture* attachment = &destination;
    RenderFramebufferDesc fd; fd.colorAttachments = &attachment; fd.colorAttachmentsCount = 1;
    auto framebuffer = CreateFramebuffer(fd);
    auto& pipeline = pipelines_[dest_desc.format];
    if (!pipeline) {
        RenderGraphicsPipelineDesc p;
        p.pipelineLayout = layout_.get(); p.vertexShader = vertex_.get(); p.pixelShader = pixel_.get();
        p.renderTargetCount = 1; p.renderTargetFormat[0] = dest_desc.format; p.renderTargetBlend[0] = RenderBlendDesc::Copy();
        p.depthClipEnabled = true;
        pipeline = device_.createGraphicsPipeline(p);
        if (!pipeline || !static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk)
            throw std::runtime_error("Native resolve pipeline creation failed");
    }
    struct Push { int32_t x, y; float scale; uint32_t conversion,swizzle; };
    const Push push{src.left / factor - dst.left, src.top / factor - dst.top, std::ldexp(1.0f, exponent_bias), uint32_t(conversion),channel_swizzle};
    const bool batched=batch_&&batch_->active();
    auto* commands=batched?&batch_->BeginStage():commands_.get();
    auto* framebuffer_ptr=framebuffer.get();
    if(batched)batch_->Keep(std::move(framebuffer));
    if(batched) {
        if(descriptor_lease)batch_->KeepLease(std::move(descriptor_lease));
        else batch_->Keep(std::move(descriptor));
        batch_->Keep(std::move(view));
    }
    if(!batched){if(batch_)batch_->Flush();commands->begin();}
    NativeSynchronousGpuTimingScope presentation_scope(!batched?presentation_timing:nullptr,*commands,
        NativeSynchronousGpuPhase::FrontbufferSwizzle,dst.right-dst.left,dst.bottom-dst.top);
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(&source, RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(&destination, RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(framebuffer_ptr);
    commands->setViewports(RenderViewport(0, 0, dest_desc.width, dest_desc.height)); commands->setScissors(dst);
    commands->setGraphicsPipelineLayout(layout_.get()); commands->setGraphicsPushConstants(0, &push);
    commands->setGraphicsDescriptorSet(descriptor_ptr, 0); commands->setPipeline(pipeline.get());
    commands->drawInstanced(3, 1, 0, 0);
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(&destination, RenderTextureLayout::SHADER_READ));
    if(!batched)Submit(&presentation_scope);
}
bool PassTransfer::PrepareStencilExport() {
    if(!stencil_export_pixel_||stencil_export_failed_)return false;
    if(stencil_export_pipeline_)return true;
    try {
        RenderGraphicsPipelineDesc p;
        p.pipelineLayout=layout_.get();p.vertexShader=vertex_.get();p.pixelShader=stencil_export_pixel_.get();
        p.renderTargetCount=0;p.depthTargetFormat=RenderFormat::D32_FLOAT_S8_UINT;
        p.depthEnabled=p.depthWriteEnabled=true;p.depthFunction=RenderComparisonFunction::ALWAYS;
        p.stencilEnabled=true;p.stencilReference=0;p.stencilReadMask=p.stencilWriteMask=255;
        p.stencilFrontFace.compareFunction=RenderComparisonFunction::ALWAYS;
        p.stencilFrontFace.passOp=p.stencilFrontFace.depthFailOp=RenderStencilOp::REPLACE;
        p.stencilBackFace=p.stencilFrontFace;
        auto pipeline=device_.createGraphicsPipeline(p);
        if(!pipeline||!static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk)
            throw std::runtime_error("Cannot create optional stencil export pipeline");
        stencil_export_pipeline_=std::move(pipeline);return true;
    }catch(...) {
        // Before any commands or attachment writes. This optional acceleration
        // may safely retry the unchanged Native stencil-mask path instead.
        stencil_export_failed_=true;++restore_statistics_.export_failures;return false;
    }
}
void PassTransfer::RestoreDepth(RenderTexture& source,RenderTexture& destination,const RenderRect* region) {
    const auto& src=static_cast<VulkanTexture&>(source).desc;
    const auto& dst=static_cast<VulkanTexture&>(destination).desc;
    if(!depth_pixel_||&source==&destination||src.width!=dst.width||src.height!=dst.height||
       dst.format!=RenderFormat::D32_FLOAT_S8_UINT||dst.multisampling.sampleCount!=1)
        throw std::runtime_error("Invalid depth/stencil restoration");
    if(region) {CheckRect(source,*region);CheckRect(destination,*region);}
    const bool export_stencil=PrepareStencilExport();
    std::shared_ptr<RenderDescriptorSet> descriptor_lease;
    auto descriptor=AcquireDescriptor(descriptor_lease);
    auto* descriptor_ptr=descriptor_lease?descriptor_lease.get():descriptor.get();
    auto view=source.createTextureView(RenderTextureViewDesc::Texture2D(src.format));
    descriptor_ptr->setTexture(0,&source,RenderTextureLayout::SHADER_READ,view.get());
    RenderFramebufferDesc fd;fd.depthAttachment=&destination;auto framebuffer=CreateFramebuffer(fd);
    const bool batched=batch_&&batch_->active();
    auto* commands=batched?&batch_->BeginStage():commands_.get();
    auto* framebuffer_ptr=framebuffer.get();
    if(batched)batch_->Keep(std::move(framebuffer));
    if(batched) {
        if(descriptor_lease)batch_->KeepLease(std::move(descriptor_lease));
        else batch_->Keep(std::move(descriptor));
        batch_->Keep(std::move(view));
    }
    if(!batched){if(batch_)batch_->Flush();commands->begin();}
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&source,RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&destination,RenderTextureLayout::DEPTH_WRITE));
    commands->setFramebuffer(framebuffer_ptr);
    if(!export_stencil) {
        commands->clearDepthStencil(false,true,0,0,region,region?1:0);++restore_statistics_.stencil_clears;
    }
    commands->setViewports(RenderViewport(0,0,dst.width,dst.height));
    commands->setScissors(region?*region:RenderRect(0,0,dst.width,dst.height));
    commands->setGraphicsPipelineLayout(layout_.get());commands->setGraphicsDescriptorSet(descriptor_ptr,0);
    ++restore_statistics_.calls;
    if(export_stencil) {
        commands->setPipeline(stencil_export_pipeline_.get());commands->drawInstanced(3,1,0,0);
        ++restore_statistics_.export_calls;++restore_statistics_.draws;
    } else {
    ++restore_statistics_.legacy_calls;
    for(unsigned i=0;i<9;++i) {
        const uint32_t bit=i?1u<<(i-1):0;
        auto& pipeline=depth_pipelines_[i];
        if(!pipeline) {
            RenderGraphicsPipelineDesc p;
            p.pipelineLayout=layout_.get();p.vertexShader=vertex_.get();p.pixelShader=depth_pixel_.get();
            p.renderTargetCount=0;p.depthTargetFormat=dst.format;
            p.depthEnabled=!i;p.depthWriteEnabled=!i;p.depthFunction=RenderComparisonFunction::ALWAYS;
            p.stencilEnabled=i!=0;p.stencilReference=bit;p.stencilReadMask=255;p.stencilWriteMask=bit;
            p.stencilFrontFace.compareFunction=RenderComparisonFunction::ALWAYS;
            p.stencilFrontFace.passOp=RenderStencilOp::REPLACE;
            p.stencilBackFace=p.stencilFrontFace;
            pipeline=device_.createGraphicsPipeline(p);
            if(!pipeline||!static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk)
                throw std::runtime_error("Cannot create depth restoration pipeline");
        }
        const std::array<uint32_t,5> push{0,0,0,bit,0};
        commands->setGraphicsPushConstants(0,push.data());commands->setPipeline(pipeline.get());
        commands->drawInstanced(3,1,0,0);++restore_statistics_.draws;
    }
    }
    if(!batched)Submit();
}
}
