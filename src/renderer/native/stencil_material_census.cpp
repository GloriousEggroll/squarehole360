#include "stencil_material_census.h"
#include "plume_vulkan.h"
#include <cstring>
namespace gow3::renderer {
using namespace plume;
namespace {
void Require(bool v,const char* text){if(!v)throw std::runtime_error(text);}
}
NativeStencilMaterialCounterSlot::NativeStencilMaterialCounterSlot(RenderDevice& device):device_(&device) {
    gpu_=device.createBuffer(RenderBufferDesc::DefaultBuffer(16,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    readback_=device.createBuffer(RenderBufferDesc::ReadbackBuffer(16));
    Require(gpu_&&readback_,"Stencil census allocation failed");
}
void NativeStencilMaterialCounterSlot::Prepare(uint64_t token,uint64_t event,uint64_t frame) {
    owner_.Prepare(token);token_=token;event_=event;frame_=frame;
}
void NativeStencilMaterialCounterSlot::Begin(RenderCommandList& commands) {
    auto& vk=dynamic_cast<VulkanCommandList&>(commands);
    Require(owner_.phase()==NativeStencilMaterialCounterOwnership::Phase::Preparing&&vk.queue->device==device_&&
        vk.activeRenderPass==VK_NULL_HANDLE,"Stencil census clear must precede its exact standalone pass");
    auto& gpu=static_cast<VulkanBuffer&>(*gpu_);
    vkCmdFillBuffer(vk.vk,gpu.vk,0,16,0);
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.buffer=gpu.vk;b.size=16;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(vk.vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr);
    owner_.Begin();
}
void NativeStencilMaterialCounterSlot::End(RenderCommandList& commands) {
    auto& vk=dynamic_cast<VulkanCommandList&>(commands);
    Require(owner_.phase()==NativeStencilMaterialCounterOwnership::Phase::Begun&&vk.queue->device==device_,"Stencil census unmatched draw end");
    vk.endActiveRenderPass();
    auto& gpu=static_cast<VulkanBuffer&>(*gpu_);auto& readback=static_cast<VulkanBuffer&>(*readback_);
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.buffer=gpu.vk;b.size=16;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(vk.vk,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
    const VkBufferCopy copy{0,0,16};vkCmdCopyBuffer(vk.vk,gpu.vk,readback.vk,1,&copy);
    b.buffer=readback.vk;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(vk.vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);
    owner_.End();
}
void NativeStencilMaterialCounterSlot::Accept(uint64_t token) {
    owner_.Accept(token);
}
nlohmann::json NativeStencilMaterialCounterSlot::Retire(uint64_t token,uint64_t submission) {
    owner_.ValidateRetirement(token,submission);
    auto& vk=static_cast<VulkanBuffer&>(*readback_);const auto* mapped=readback_->map();
    Require(mapped,"Stencil census map failed");
    const auto invalidated=vmaInvalidateAllocation(vk.device->allocator,vk.allocation,0,VK_WHOLE_SIZE);
    NativeStencilMaterialCounts counts;std::memcpy(&counts,mapped,sizeof(counts));readback_->unmap();
    Require(invalidated==VK_SUCCESS&&counts.Valid(),"Stencil census overflow or invalid fenced counts");
    owner_.Retire(token,submission);
    return {{"token",token_},{"event",event_},{"frame",frame_},{"submission",submission},
        {"entered",counts.entered},{"rejected",counts.rejected},{"survived",counts.survived},{"overflow",counts.overflow}};
}
} // namespace gow3::renderer
