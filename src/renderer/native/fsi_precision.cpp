#include "fsi_precision.h"
#include "fsi_state.h"
#include "plume_vulkan.h"
#include <cstring>
#include <stdexcept>
namespace gow3::renderer {
using namespace plume;
namespace {
const std::array<RenderDescriptorRange,2> ranges{{
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
bool Factor(uint32_t v) {return v<=1||(v>=4&&v<=16);}
void Flush(RenderBuffer& buffer) {
    auto& b=static_cast<VulkanBuffer&>(buffer);
    if(vmaFlushAllocation(b.device->allocator,b.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
        throw std::runtime_error("Cannot flush interlock input");
}
}
FsiPrecision::FsiPrecision(RenderDevice& device,RenderCommandQueue& queue,
    std::span<const uint32_t> vertex,std::span<const uint32_t> fragment,bool enabled,bool production)
    :production_abi_(production),device_(device),queue_(queue) {
    if(!enabled)throw std::runtime_error("Ordered pixel interlock was not enabled on the device");
    storage_=device_.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    commands_=queue_.createCommandList();fence_=device_.createCommandFence();
    query_=std::make_unique<NativeQueryTracker>(device_);
    vertex_=device_.createShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
    fragment_=device_.createShader(fragment.data(),fragment.size_bytes(),"main",RenderShaderFormat::SPIRV);
    const RenderPushConstantRange push(0,0,0,production?sizeof(ShaderPushConstants):sizeof(FsiRasterState),RenderShaderStageFlag::PIXEL);
    const RenderDescriptorSetDesc descriptors(ranges.data(),ranges.size());
    const std::array<RenderDescriptorSetDesc,5> production_descriptors{descriptors,{}, {}, {},{ranges.data(),1}};
    layout_=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,production?production_descriptors.data():&descriptors,production?5:1));
    RenderGraphicsPipelineDesc p;
    p.pipelineLayout=layout_.get();p.vertexShader=vertex_.get();p.pixelShader=fragment_.get();
    p.renderTargetCount=1;p.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
    p.renderTargetBlend[0]=RenderBlendDesc::Copy();p.renderTargetBlend[0].renderTargetWriteMask=0;
    p.depthClipEnabled=true;p.cullMode=RenderCullMode::NONE;
    pipeline_=device_.createGraphicsPipeline(p);
    if(!pipeline_||!static_cast<VulkanGraphicsPipeline&>(*pipeline_).vk)
        throw std::runtime_error("Cannot create packed EDRAM interlock pipeline");
}
void FsiPrecision::Submit() {commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());queue_.waitForCommandFence(fence_.get());}
void FsiPrecision::Initialize(std::span<const uint32_t> bytes) {
    if(bytes.size_bytes()!=kBytes)throw std::runtime_error("Interlock initial EDRAM must contain all tiles");
    auto upload=device_.createBuffer(RenderBufferDesc::UploadBuffer(kBytes));
    std::memcpy(upload->map(),bytes.data(),kBytes);Flush(*upload);upload->unmap();
    commands_->begin();commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(storage_.get(),RenderBufferAccess::WRITE));
    commands_->copyBuffer(storage_.get(),upload.get());Submit();
}
void FsiPrecision::Draw(const FsiRasterState& s,std::span<const FsiFragment> fragments,uint32_t width,uint32_t height,std::array<float,4> polygon_bias) {
    if(!width||!height||width>8192||height>8192||s.pitch<width||s.pitch>16383||s.color_base>=2048||s.depth_base>=2048||
       s.depth_format>1||s.color_mask>15||s.blend_enabled>1||s.reserved||fragments.empty()||fragments.size()>4096)
        throw std::runtime_error("Unsupported interlock raster extent/state");
    if(s.color_format!=0&&s.color_format!=2&&s.color_format!=3&&s.color_format!=4&&s.color_format!=5&&
       s.color_format!=6&&s.color_format!=7&&s.color_format!=14&&s.color_format!=15)
        throw std::runtime_error("Unsupported interlock color format");
    if(s.blend_enabled&&(!Factor(s.blend_control&31)||!Factor((s.blend_control>>8)&31)||
       !Factor((s.blend_control>>16)&31)||!Factor((s.blend_control>>24)&31)||
       ((s.blend_control>>5)&7)>4||((s.blend_control>>21)&7)>4))
        throw std::runtime_error("Unsupported interlock blend state");
    auto inputs=device_.createBuffer(RenderBufferDesc::UploadBuffer(fragments.size_bytes(),RenderBufferFlag::STORAGE));
    std::memcpy(inputs->map(),fragments.data(),fragments.size_bytes());Flush(*inputs);inputs->unmap();
    auto descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),ranges.size()));
    descriptors->setBuffer(0,storage_.get());descriptors->setBuffer(1,inputs.get());
    std::unique_ptr<RenderDescriptorSet> production_descriptors;
    std::unique_ptr<RenderBuffer> production_constants;
    ShaderPushConstants addresses{};
    if(production_abi_) {
        NativeFsiShaderConstants constants;
        auto& r=constants.raster;r.pitch=s.pitch;r.depth_base=s.depth_base;r.depth_control=s.depth_control;r.depth_format=s.depth_format;
        r.stencil_front=s.stencil_front;r.stencil_back=s.stencil_back;r.blend_constant=s.blend_constant;r.blend_enabled=s.blend_enabled;
        r.color[0]={s.color_base,s.color_format,s.color_mask,s.blend_control};
        r.front_slope_bias=polygon_bias[0];r.front_constant_bias=polygon_bias[1];
        r.back_slope_bias=polygon_bias[2];r.back_constant_bias=polygon_bias[3];
        production_constants=device_.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(constants),RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
        std::memcpy(production_constants->map(),&constants,sizeof(constants));Flush(*production_constants);production_constants->unmap();
        addresses.shared_constants=production_constants->getDeviceAddress();
        production_descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),1));
        production_descriptors->setBuffer(0,storage_.get());
    }
    auto target=device_.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R8G8B8A8_UNORM));
    const RenderTexture* attachment=target.get();RenderFramebufferDesc fd;fd.colorAttachments=&attachment;fd.colorAttachmentsCount=1;
    auto framebuffer=device_.createFramebuffer(fd);
    commands_->begin();
    commands_->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(storage_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
    commands_->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(inputs.get(),RenderBufferAccess::READ));
    commands_->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target.get(),RenderTextureLayout::COLOR_WRITE));
    commands_->setFramebuffer(framebuffer.get());commands_->setViewports(RenderViewport(0,0,width,height));
    commands_->setScissors(RenderRect(0,0,width,height));commands_->setGraphicsPipelineLayout(layout_.get());
    commands_->setGraphicsPushConstants(0,production_abi_?static_cast<const void*>(&addresses):static_cast<const void*>(&s));
    commands_->setGraphicsDescriptorSet(descriptors.get(),0);if(production_abi_)commands_->setGraphicsDescriptorSet(production_descriptors.get(),4);
    commands_->setPipeline(pipeline_.get());
    query_->Begin(1,32);query_->ResetDraw(*commands_);query_->BeginDraw(*commands_);
    commands_->drawInstanced(3,fragments.size(),0,0);query_->EndDraw(*commands_);Submit();
    query_->CompleteDraw();passing_samples_=query_->End(1,0).samples;
}
std::vector<uint32_t> FsiPrecision::Read() {
    auto readback=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
    commands_->begin();commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(storage_.get(),RenderBufferAccess::READ));
    commands_->copyBuffer(readback.get(),storage_.get());Submit();
    const auto* mapped=readback->map();auto& b=static_cast<VulkanBuffer&>(*readback);
    if(vmaInvalidateAllocation(b.device->allocator,b.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
        throw std::runtime_error("Cannot invalidate interlock readback");
    std::vector<uint32_t> result(kBytes/4);std::memcpy(result.data(),mapped,kBytes);readback->unmap();return result;
}
}
