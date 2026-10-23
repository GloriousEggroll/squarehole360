#pragma once

// Consumer-side Plume integration. The window system and presentation remain
// owned by the host; this adapter borrows its Vulkan device and graphics queue.
#include "plume_vulkan.h"
#include <functional>
#include <memory>
#include <string>

namespace gow3::renderer {

// Private native capability: submit and wait/reset MUST throw on any backend
// failure. A void return from ordinary Plume queues is not a fence receipt.
// Only the live checked queue implements this in production; focused fixtures
// may supply the same checked semantics. No user config can attest this.
class NativeCheckedQueueCompletion {
protected:
    NativeCheckedQueueCompletion() = default;
public:
    virtual ~NativeCheckedQueueCompletion() = default;
};

// Implement using the host's queue mutex, not a second unrelated mutex. The
// lease must remain locked until destruction and may be acquired recursively.
struct LiveQueueLease {
    virtual ~LiveQueueLease() = default;
};

struct LiveVulkanDeviceDesc {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    uint32_t api_version = VK_API_VERSION_1_0;
    // Enabled features, not merely physically supported features.
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    bool null_descriptor = false;
    bool ordered_pixel_interlock = false;
    bool shader_stencil_export = false; // Actually enabled device extension.
    bool store_op_none = false; // Actually enabled EXT_load_store_op_none (core 1.3 checked separately).
    std::function<std::unique_ptr<LiveQueueLease>()> acquire_queue;
};

// Returns an explanation if the host cannot run the current native shaders.
// Unsupported hosts should retain the reference renderer instead of attempting
// to create pipelines using features that the host did not enable.
std::string CheckLiveVulkanDevice(const LiveVulkanDeviceDesc& desc);

class LiveVulkanDevice {
public:
    explicit LiveVulkanDevice(const LiveVulkanDeviceDesc& desc);
    ~LiveVulkanDevice();
    LiveVulkanDevice(const LiveVulkanDevice&) = delete;
    LiveVulkanDevice& operator=(const LiveVulkanDevice&) = delete;

    plume::RenderDevice& device();

    // Waits only for the borrowed graphics queue. All resources and command
    // queues allocated through this adapter must be destroyed before it is.
    void WaitIdle();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace gow3::renderer
