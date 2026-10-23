#pragma once
#include "plume_render_interface_types.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace gow3::renderer {
// Host rasterization is deliberately still single-sample. Guest sample/EDRAM
// behavior must not change when removing the unobserved host color image.
inline bool NativeAttachmentlessFsiEligible(bool native_fsi,uint32_t width,uint32_t height,
        const VkPhysicalDeviceLimits& limits,
        VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT) noexcept {
    return native_fsi&&width&&height&&width<=8192&&height<=8192&&
        width<=limits.maxFramebufferWidth&&height<=limits.maxFramebufferHeight&&
        limits.maxFramebufferLayers>=1&&samples==VK_SAMPLE_COUNT_1_BIT&&
        (limits.framebufferNoAttachmentsSampleCounts&samples);
}
inline plume::RenderFramebufferDesc NativeAttachmentlessFsiFramebuffer(uint32_t width,uint32_t height) noexcept {
    plume::RenderFramebufferDesc result;
    result.attachmentlessWidth=width;result.attachmentlessHeight=height;
    return result;
}
}
