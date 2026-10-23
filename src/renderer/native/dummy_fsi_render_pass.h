#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace gow3::renderer {
// Only Scene's private, never-read FSI color image qualifies. The caller must
// additionally prove that this framebuffer has not yet been used by commands.
struct NativeDummyFsiAttachmentShape {
    bool native_fsi=false,dedicated=false,has_depth=false;
    uint32_t colors=0,width=0,height=0,levels=0,layers=0;
    VkFormat format=VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;
};
inline bool NativeDummyFsiAttachmentEligible(const NativeDummyFsiAttachmentShape& s) noexcept {
    return s.native_fsi&&s.dedicated&&!s.has_depth&&s.colors==1&&
        s.width&&s.height&&s.width<=8192&&s.height<=8192&&s.levels==1&&s.layers==1&&
        s.format==VK_FORMAT_R8G8B8A8_UNORM&&s.samples==VK_SAMPLE_COUNT_1_BIT;
}
// Identical to Plume's one-color/no-depth framebuffer pass except load/store.
// Vulkan render-pass compatibility ignores those two attachment operations.
struct NativeDummyFsiRenderPassDescription {
    VkAttachmentDescription attachment{};
    VkAttachmentReference color{};
    VkSubpassDescription subpass{};
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    NativeDummyFsiRenderPassDescription() noexcept {
        attachment.format=VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.attachment=0;color.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
        info.attachmentCount=1;info.pAttachments=&attachment;
        info.subpassCount=1;info.pSubpasses=&subpass;
    }
    NativeDummyFsiRenderPassDescription(const NativeDummyFsiRenderPassDescription&)=delete;
    NativeDummyFsiRenderPassDescription& operator=(const NativeDummyFsiRenderPassDescription&)=delete;
};
// Fresh-owner-only: create before replacement, then give the new handle to the
// framebuffer's existing destructor. Failure leaves its ordinary Native pass
// untouched. No submission, wait, image transition or pipeline change occurs.
inline VkResult ReplaceFreshNativeDummyFsiRenderPass(VkDevice device,VkRenderPass& owner,
        const NativeDummyFsiAttachmentShape& shape,PFN_vkCreateRenderPass create,
        PFN_vkDestroyRenderPass destroy) noexcept {
    if(!device||!owner||!create||!destroy||!NativeDummyFsiAttachmentEligible(shape))
        return VK_ERROR_INITIALIZATION_FAILED;
    NativeDummyFsiRenderPassDescription description;
    VkRenderPass candidate=VK_NULL_HANDLE;
    const auto result=create(device,&description.info,nullptr,&candidate);
    if(result!=VK_SUCCESS)return result;
    if(!candidate||candidate==owner)return VK_ERROR_INITIALIZATION_FAILED;
    const auto previous=owner;owner=candidate;
    destroy(device,previous,nullptr);
    return VK_SUCCESS;
}
}
