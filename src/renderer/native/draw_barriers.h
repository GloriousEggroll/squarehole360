#pragma once
#include "plume_render_interface_types.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace gow3::renderer {
struct NativeDrawBarrierStatistics {
    uint64_t sampled_emitted=0,sampled_skipped=0;
    uint64_t precise_fsi=0,precise_fallback=0,merged=0,separate=0;
};
inline bool NativeSampleBarrierRequired(plume::RenderTextureLayout layout,
                                        plume::RenderBarrierStages stages) noexcept {
    return layout!=plume::RenderTextureLayout::SHADER_READ ||
        stages!=plume::RenderBarrierStage::GRAPHICS;
}
struct NativeFsiBarrierState {
    bool native_fsi=false,active_render_pass=false;
    plume::RenderBarrierStages edram_stages=plume::RenderBarrierStage::NONE;
    plume::RenderBarrierStages target_stages=plume::RenderBarrierStage::NONE;
    plume::RenderTextureLayout target_layout=plume::RenderTextureLayout::UNKNOWN;
};
inline bool NativePreciseFsiBarrierAllowed(const NativeFsiBarrierState& state) noexcept {
    return state.native_fsi&&!state.active_render_pass&&
        state.edram_stages==plume::RenderBarrierStage::GRAPHICS&&
        state.target_stages==plume::RenderBarrierStage::GRAPHICS&&
        state.target_layout==plume::RenderTextureLayout::COLOR_WRITE;
}
struct NativeFsiBarrierPlan {
    VkPipelineStageFlags stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkDependencyFlags dependencies=0; // Global EDRAM aliases may cross framebuffer coordinates.
    VkBufferMemoryBarrier edram{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    VkImageMemoryBarrier target{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    NativeFsiBarrierPlan(VkBuffer buffer,uint64_t bytes,VkImage image,uint32_t levels,uint32_t layers) {
        edram.srcAccessMask=edram.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        edram.srcQueueFamilyIndex=edram.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        edram.buffer=buffer;edram.offset=0;edram.size=bytes;
        target.srcAccessMask=target.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        target.oldLayout=target.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        target.srcQueueFamilyIndex=target.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        target.image=image;target.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        target.subresourceRange.baseMipLevel=target.subresourceRange.baseArrayLayer=0;
        target.subresourceRange.levelCount=levels;target.subresourceRange.layerCount=layers;
    }
};
} // namespace gow3::renderer
