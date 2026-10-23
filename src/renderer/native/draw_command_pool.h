#pragma once
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace gow3::renderer {
struct NativeDrawRecordingStatistics {
    uint64_t begins=0,pool_resets=0,ends=0;
    double begin_ms=0,pool_reset_ms=0,end_ms=0,enqueue_submit_ms=0,enqueue_wait_ms=0;
};

// The caller owns one command buffer in this private pool and proves that its
// previous submission fence completed before entering. Reset no other pool:
// a transfer, marker, shared pool or accepted pending draw must never use this
// shortcut. The flags retain driver allocations just like the old buffer reset.
inline void BeginRetiredNativeDrawPool(VkDevice device,VkCommandPool pool,VkCommandBuffer commands,
    PFN_vkResetCommandPool reset,PFN_vkBeginCommandBuffer begin,
    NativeDrawRecordingStatistics* statistics=nullptr,bool time_reset=false) {
    if(!device||!pool||!commands||!reset||!begin)
        throw std::invalid_argument("Native retired draw pool has invalid handles/functions");
    const auto start=time_reset?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    const auto reset_result=reset(device,pool,0);
    if(statistics) {
        if(time_reset)statistics->pool_reset_ms+=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-start).count();
        if(reset_result==VK_SUCCESS)++statistics->pool_resets;
    }
    if(reset_result!=VK_SUCCESS)
        throw std::runtime_error("Native retired draw pool reset failed: "+std::to_string(reset_result));
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const auto begin_result=begin(commands,&info);
    if(begin_result!=VK_SUCCESS)
        throw std::runtime_error("Native retired draw command begin failed: "+std::to_string(begin_result));
}
} // namespace gow3::renderer
