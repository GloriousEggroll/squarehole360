#include "renderer/native/dummy_fsi_render_pass.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool condition,const char* text) {++checks;if(!condition)throw std::runtime_error(text);}
template<class T>T Handle(uintptr_t value) {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);
    else return T(value);
}
const auto device=Handle<VkDevice>(1);
const auto original=Handle<VkRenderPass>(10),replacement=Handle<VkRenderPass>(20);
VkResult create_result=VK_SUCCESS;
VkRenderPass create_handle=replacement;
uint64_t creates=0;
std::vector<VkRenderPass> destroyed;
VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice d,const VkRenderPassCreateInfo* info,
        const VkAllocationCallbacks* allocator,VkRenderPass* output) {
    ++creates;Require(d==device&&!allocator,"Create lost borrowed device or allocator pairing");
    Require(info&&info->sType==VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO&&!info->pNext&&!info->flags&&
        info->attachmentCount==1&&info->subpassCount==1&&!info->dependencyCount&&!info->pDependencies,
        "Discard pass added or omitted structural render-pass fields");
    const auto& a=*info->pAttachments;
    Require(!a.flags&&a.format==VK_FORMAT_R8G8B8A8_UNORM&&a.samples==VK_SAMPLE_COUNT_1_BIT&&
        a.loadOp==VK_ATTACHMENT_LOAD_OP_DONT_CARE&&a.storeOp==VK_ATTACHMENT_STORE_OP_DONT_CARE&&
        a.stencilLoadOp==VK_ATTACHMENT_LOAD_OP_DONT_CARE&&a.stencilStoreOp==VK_ATTACHMENT_STORE_OP_DONT_CARE&&
        a.initialLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL&&a.finalLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "Discard pass changed format/sample/layout or retained unnecessary attachment operations");
    const auto& s=*info->pSubpasses;
    Require(!s.flags&&s.pipelineBindPoint==VK_PIPELINE_BIND_POINT_GRAPHICS&&!s.inputAttachmentCount&&
        !s.pInputAttachments&&s.colorAttachmentCount==1&&!s.pResolveAttachments&&!s.pDepthStencilAttachment&&
        !s.preserveAttachmentCount&&!s.pPreserveAttachments&&s.pColorAttachments&&
        s.pColorAttachments->attachment==0&&s.pColorAttachments->layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "Discard pass changed the exact compatible subpass");
    *output=create_handle;return create_result;
}
VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice d,VkRenderPass handle,const VkAllocationCallbacks* allocator) {
    Require(d==device&&!allocator,"Destroy lost borrowed device/allocator pairing");destroyed.push_back(handle);
}
NativeDummyFsiAttachmentShape Shape() {
    return {true,true,false,1,1280,720,1,1,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT};
}
void Refuse(const NativeDummyFsiAttachmentShape& shape,VkDevice d=device,
        PFN_vkCreateRenderPass create=Create,PFN_vkDestroyRenderPass destroy=Destroy) {
    auto owner=original;const auto before_creates=creates;const auto before_destroyed=destroyed.size();
    Require(ReplaceFreshNativeDummyFsiRenderPass(d,owner,shape,create,destroy)!=VK_SUCCESS,
        "Invalid candidate was installed");
    Require(owner==original&&creates==before_creates&&destroyed.size()==before_destroyed,
        "Eligibility refusal mutated existing ownership or called Vulkan");
}
}
int main() try {
    Require(NativeDummyFsiAttachmentEligible(Shape()),"Exact dummy framebuffer refused");
    for(unsigned field=0;field<13;++field) {
        auto s=Shape();
        if(field==0)s.native_fsi=false;
        if(field==1)s.dedicated=false;
        if(field==2)s.has_depth=true;
        if(field==3)s.colors=0;
        if(field==4)s.colors=2;
        if(field==5)s.width=0;
        if(field==6)s.height=0;
        if(field==7)s.width=8193;
        if(field==8)s.height=8193;
        if(field==9)s.levels=2;
        if(field==10)s.layers=2;
        if(field==11)s.format=VK_FORMAT_B8G8R8A8_UNORM;
        if(field==12)s.samples=VK_SAMPLE_COUNT_2_BIT;
        Refuse(s);
    }
    Refuse(Shape(),VK_NULL_HANDLE);Refuse(Shape(),device,nullptr);Refuse(Shape(),device,Create,nullptr);
    auto missing=VkRenderPass(VK_NULL_HANDLE);
    Require(ReplaceFreshNativeDummyFsiRenderPass(device,missing,Shape(),Create,Destroy)!=VK_SUCCESS&&
        creates==0&&destroyed.empty(),"Missing old owner created a pass");
    for(const auto failure:std::array{VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY,VK_ERROR_INITIALIZATION_FAILED}) {
        auto owner=original;create_result=failure;create_handle=VK_NULL_HANDLE;const auto count=destroyed.size();
        Require(ReplaceFreshNativeDummyFsiRenderPass(device,owner,Shape(),Create,Destroy)==failure&&
            owner==original&&destroyed.size()==count,"Create failure released or replaced accepted original owner");
    }
    create_result=VK_SUCCESS;
    for(const auto malformed:std::array{VkRenderPass(VK_NULL_HANDLE),original}) {
        auto owner=original;create_handle=malformed;const auto count=destroyed.size();
        Require(ReplaceFreshNativeDummyFsiRenderPass(device,owner,Shape(),Create,Destroy)!=VK_SUCCESS&&
            owner==original&&destroyed.size()==count,"Invalid success handle released original owner");
    }
    auto owner=original;create_handle=replacement;const auto count=destroyed.size();
    Require(ReplaceFreshNativeDummyFsiRenderPass(device,owner,Shape(),Create,Destroy)==VK_SUCCESS&&
        owner==replacement&&destroyed.size()==count+1&&destroyed.back()==original,
        "Successful creation did not replace ownership and destroy exactly original pass");
    // Model the actual unchanged VulkanFramebuffer destructor after a fully
    // fenced lifetime. It must own only the replacement, never the old handle.
    Destroy(device,owner,nullptr);owner=VK_NULL_HANDLE;
    Require(destroyed==std::vector<VkRenderPass>{original,replacement},"Fresh-owner transaction double-destroyed or leaked a valid pass");
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"creates\":"<<creates<<"}\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
