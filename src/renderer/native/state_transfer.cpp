#include "state_transfer.h"
#include "state_transfer_spv.h"
#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>

namespace gow3::renderer {
using namespace plume;
namespace {
void Check(VkResult result,const char* reason) {if(result!=VK_SUCCESS)throw std::runtime_error(reason);}
}
struct NativeStateTransfer::State {
    RenderCommandQueue& queue;
    VkDevice device;
    uint32_t window;
    VkShaderModule shader=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    std::unique_ptr<RenderCommandList> commands;
    std::unique_ptr<RenderCommandFence> fence;
    explicit State(RenderDevice& owner,RenderCommandQueue& queue_,uint32_t requested):queue(queue_) {
        auto& vulkan=static_cast<VulkanDevice&>(owner);device=vulkan.vk;
        const auto& limits=vulkan.physicalDeviceProperties.limits;
        const uint32_t alignment=std::max(4u,uint32_t(limits.minStorageBufferOffsetAlignment));
        uint64_t maximum=std::min<uint64_t>(limits.maxStorageBufferRange,uint64_t(limits.maxComputeWorkGroupCount[0])*64*4);
        maximum=std::min<uint64_t>(maximum,8*1024*1024);
        if(requested)maximum=std::min<uint64_t>(maximum,requested);
        window=uint32_t(maximum)/alignment*alignment;
        if(!window)throw std::runtime_error("Native handback descriptor window is too small");
        commands=queue.createCommandList();fence=owner.createCommandFence();
    }
    void Initialize() {
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize=sizeof(state_transfer_spv);module.pCode=state_transfer_spv;
        Check(vkCreateShaderModule(device,&module,nullptr,&shader),"Cannot create native handback shader");
        const std::array<VkDescriptorSetLayoutBinding,2> bindings{{
            {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount=bindings.size();set.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(device,&set,nullptr,&descriptors),"Cannot create native handback descriptors");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&descriptors;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        Check(vkCreatePipelineLayout(device,&pl,nullptr,&layout),"Cannot create native handback layout");
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.layout=layout;cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=shader;cp.stage.pName="main";
        Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline),"Cannot create native handback pipeline");
    }
    ~State() {
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptors)vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
        if(shader)vkDestroyShaderModule(device,shader,nullptr);
    }
    void Copy(const NativeMemoryState& source,VkBuffer destination) {
        if(!source.buffer||!destination||source.buffer==destination||!source.capacity||source.capacity%4||source.capacity>512u*1024*1024)
            throw std::runtime_error("Invalid native handback buffers");
        uint64_t previous=0;
        std::map<uint32_t,VkDescriptorSet> sets;
        for(const auto& [address,count]:source.ranges) {
            if(!count||address<previous||uint64_t(address)+count>source.capacity)
                throw std::runtime_error("Invalid or overlapping native handback range");
            previous=uint64_t(address)+count;
            for(uint32_t base=address/window*window;base<previous;base+=window)sets.emplace(base,VK_NULL_HANDLE);
        }
        if(sets.empty())return;
        struct Pool {
            VkDevice device;VkDescriptorPool handle=VK_NULL_HANDLE;
            ~Pool(){if(handle)vkDestroyDescriptorPool(device,handle,nullptr);}
        } pool{device};
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(sets.size()*2)};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets=sets.size();pi.poolSizeCount=1;pi.pPoolSizes=&size;
        Check(vkCreateDescriptorPool(device,&pi,nullptr,&pool.handle),"Cannot allocate native handback descriptors");
        for(auto& [base,set]:sets) {
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocation.descriptorPool=pool.handle;allocation.descriptorSetCount=1;allocation.pSetLayouts=&descriptors;
            Check(vkAllocateDescriptorSets(device,&allocation,&set),"Cannot allocate native handback set");
            const uint32_t extent=std::min(window,source.capacity-base);
            const std::array<VkDescriptorBufferInfo,2> buffers{{{source.buffer,base,extent},{destination,base,extent}}};
            std::array<VkWriteDescriptorSet,2> writes{};
            for(unsigned i=0;i<2;++i) {
                writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;
                writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i];
            }
            vkUpdateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);
        }
        commands->begin();auto command=static_cast<VulkanCommandList&>(*commands).vk;
        std::array<VkBufferMemoryBarrier,2> barriers{};
        for(unsigned i=0;i<2;++i) {
            auto& b=barriers[i];b={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
            b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|(i?VK_ACCESS_SHADER_WRITE_BIT:0);
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.buffer=i?destination:source.buffer;b.size=VK_WHOLE_SIZE;
        }
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,2,barriers.data(),0,nullptr);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        for(const auto& [address,count]:source.ranges) {
            uint32_t at=address,end=address+count;
            while(at<end) {
                const uint32_t base=at/window*window,last=std::min(end,base+window);
                auto set=sets.at(base);
                vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
                const uint32_t first_word=(at-base)/4,word_count=(last-base+3)/4-first_word;
                const std::array<uint32_t,4> push{at-base,last-base,first_word,word_count};
                vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push.data());
                vkCmdDispatch(command,(word_count+63)/64,1,1);
                // Separate byte ranges can share a word; order masked merges.
                auto b=barriers[1];b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr);
                at=last;
            }
        }
        auto b=barriers[1];b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,1,&b,0,nullptr);
        commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    }
};
NativeStateTransfer::NativeStateTransfer(RenderDevice& device,RenderCommandQueue& queue,uint32_t window)
    :state_(std::make_unique<State>(device,queue,window)){state_->Initialize();}
NativeStateTransfer::~NativeStateTransfer()=default;
void NativeStateTransfer::Copy(const NativeMemoryState& source,VkBuffer destination){state_->Copy(source,destination);}
}
