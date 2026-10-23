#pragma once
// Offscreen validation helpers. Resource contents are only mapped after a fence.
#include "plume_vulkan.h"
#include <cstring>
#include <stdexcept>
#include <vector>
namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }
namespace validation {
inline void Flush(plume::RenderBuffer& buffer) {
    auto& vk = static_cast<plume::VulkanBuffer&>(buffer);
    if (vmaFlushAllocation(vk.device->allocator, vk.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("Upload flush failed");
}
inline std::vector<uint8_t> ReadImage(plume::RenderDevice& device, plume::RenderCommandQueue& queue,
                                    plume::RenderTexture& image, uint32_t pixel_bytes) {
    using namespace plume;
    auto& vk_image = static_cast<VulkanTexture&>(image);
    const auto& d = vk_image.desc;
    auto buffer = device.createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(d.width) * d.height * d.depth * pixel_bytes));
    auto commands = queue.createCommandList(); auto fence = device.createCommandFence();
    commands->begin();
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(&image, RenderTextureLayout::COPY_SOURCE));
    auto& vk_buffer = static_cast<VulkanBuffer&>(*buffer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {d.width, d.height, d.depth};
    const auto vk_cmd = static_cast<VulkanCommandList&>(*commands).vk;
    vkCmdCopyImageToBuffer(vk_cmd, vk_image.vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buffer.vk, 1, &copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    commands->end(); queue.executeCommandLists(commands.get(), fence.get()); queue.waitForCommandFence(fence.get());
    const void* mapped = buffer->map();
    if (vmaInvalidateAllocation(vk_buffer.device->allocator, vk_buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("Readback invalidate failed");
    std::vector<uint8_t> result(size_t(d.width) * d.height * d.depth * pixel_bytes);
    std::memcpy(result.data(), mapped, result.size()); buffer->unmap();
    return result;
}
}
