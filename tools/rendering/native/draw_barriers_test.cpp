#include "renderer/native/draw_barriers.h"
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
using namespace plume;
namespace {
void Require(bool value) {if(!value)throw std::runtime_error("Draw barrier policy mismatch");}
}
int main() try {
    uint64_t cases=0;
    for(uint32_t native=0;native<2;++native)for(uint32_t pass=0;pass<2;++pass)
        for(uint32_t buffer=0;buffer<16;++buffer)for(uint32_t image=0;image<16;++image)
            for(uint32_t layout=0;layout<16;++layout) {
                const auto texture_layout=static_cast<RenderTextureLayout>(layout);
                const bool expected=native&&!pass&&buffer==RenderBarrierStage::GRAPHICS&&
                    image==RenderBarrierStage::GRAPHICS&&texture_layout==RenderTextureLayout::COLOR_WRITE;
                Require(NativePreciseFsiBarrierAllowed({bool(native),bool(pass),buffer,image,texture_layout})==expected);
                Require(NativeSampleBarrierRequired(texture_layout,image)==
                    !(texture_layout==RenderTextureLayout::SHADER_READ&&image==RenderBarrierStage::GRAPHICS));
                ++cases;
            }
    NativeFsiBarrierPlan plan(VK_NULL_HANDLE,10u*1024*1024,VK_NULL_HANDLE,3,6);
    Require(plan.dependencies==0&&plan.stages==(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
    Require(plan.edram.srcAccessMask==(VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT)&&
        plan.edram.dstAccessMask==plan.edram.srcAccessMask&&plan.edram.offset==0&&plan.edram.size==10u*1024*1024&&
        plan.edram.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&plan.edram.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
    Require(plan.target.srcAccessMask==(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)&&
        plan.target.dstAccessMask==plan.target.srcAccessMask&&plan.target.oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL&&
        plan.target.newLayout==plan.target.oldLayout&&plan.target.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&
        plan.target.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&plan.target.subresourceRange.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&
        plan.target.subresourceRange.baseMipLevel==0&&plan.target.subresourceRange.baseArrayLayer==0&&
        plan.target.subresourceRange.levelCount==3&&plan.target.subresourceRange.layerCount==6);
    std::cout<<"PASS "<<cases<<" exact draw barrier admission/sample-read cases and global full-range dependency masks\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
