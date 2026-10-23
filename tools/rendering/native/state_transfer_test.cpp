#include "gpu_helpers.h"
#include "renderer/native/state_transfer.h"
#include <cstring>
#include <iostream>

using namespace plume;
using namespace gow3::renderer;
int main() try {
    auto api=CreateVulkanInterface();auto device=api?api->createDevice():nullptr;
    if(!device)throw std::runtime_error("Vulkan unavailable");
    auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
    constexpr uint32_t capacity=8192;
    std::vector<uint8_t> input(capacity),expected(capacity),initial(capacity);
    for(uint32_t i=0;i<capacity;++i){input[i]=uint8_t(i*73+19);initial[i]=uint8_t(i*17+203);}
    auto source=device->createBuffer(RenderBufferDesc::DefaultBuffer(capacity,RenderBufferFlag::STORAGE));
    auto target=device->createBuffer(RenderBufferDesc::DefaultBuffer(capacity,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    auto upload=device->createBuffer(RenderBufferDesc::UploadBuffer(capacity));
    auto readback=device->createBuffer(RenderBufferDesc::ReadbackBuffer(capacity));
    auto command=queue->createCommandList();auto fence=device->createCommandFence();
    const auto submit=[&](){command->end();queue->executeCommandLists(command.get(),fence.get());queue->waitForCommandFence(fence.get());};
    const auto write=[&](RenderBuffer& buffer,const std::vector<uint8_t>& bytes) {
        std::memcpy(upload->map(),bytes.data(),capacity);validation::Flush(*upload);upload->unmap();
        command->begin();command->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(&buffer,RenderBufferAccess::WRITE));
        command->copyBuffer(&buffer,upload.get());submit();
    };
    const auto verify=[&](){
        command->begin();
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=static_cast<VulkanBuffer&>(*target).vk;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*command).vk,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
        command->copyBuffer(readback.get(),target.get());
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*command).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        submit();
        auto data=readback->map();auto& buffer=static_cast<VulkanBuffer&>(*readback);
        if(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
            throw std::runtime_error("Cannot read native handback test");
        const bool equal=std::memcmp(data,expected.data(),capacity)==0;readback->unmap();
        if(!equal)throw std::runtime_error("Native handback changed an owned byte or an untouched neighbor");
    };
    write(*source,input);write(*target,initial);expected=initial;
    NativeMemoryState state{static_cast<VulkanBuffer&>(*source).vk,capacity,
        {{0,1},{2,1},{3,3},{15,2},{63,2},{255,2},{511,518},{4093,6},{8189,3}}};
    // Deliberately narrow descriptor windows exercise cross-window edges.
    NativeStateTransfer copy(*device,*queue,256);
    copy.Copy(state,static_cast<VulkanBuffer&>(*target).vk);
    for(auto [at,size]:state.ranges)std::copy_n(input.begin()+at,size,expected.begin()+at);
    verify();
    state.ranges={{6,9},{17,46},{65,190},{257,254},{1029,3064},{4099,4090}};
    copy.Copy(state,static_cast<VulkanBuffer&>(*target).vk);
    for(auto [at,size]:state.ranges)std::copy_n(input.begin()+at,size,expected.begin()+at);
    verify();
    unsigned rejected=0;
    for(auto ranges:std::vector<NativeStateRanges>{{{0,0}},{{8191,2}},{{4,8},{8,4}},{{8,1},{4,1}},{{UINT32_MAX,4}}}) {
        state.ranges=std::move(ranges);
        try{copy.Copy(state,static_cast<VulkanBuffer&>(*target).vk);}
        catch(const std::runtime_error&){++rejected;}
    }
    if(rejected!=5)throw std::runtime_error("Invalid native handback ranges accepted");
    verify();
    state.ranges.clear();copy.Copy(state,static_cast<VulkanBuffer&>(*target).vk);verify();
    std::cout<<"PASS: exact-byte GPU state handback, shared edge words, descriptor windows, untouched neighbors; "
             <<capacity*4<<" byte comparisons and "<<rejected<<" invalid transfers rejected\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
