// Creates and destroys the FidelityFX FSR 3.1 upscaling and frame generation
// contexts on the first Vulkan GPU, with the presenter's arguments. No game,
// no rendering: catches backend crashes and missing shaders in isolation.
#undef NDEBUG
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        std::puts("No Vulkan instance; skipped");
        return 0;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count) {
        std::puts("No Vulkan device; skipped");
        return 0;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical = devices[0];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    // FidelityFX uses these optional 1.1-1.3 features when present.
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue;
    VkDevice device;
    assert(vkCreateDevice(physical, &device_info, nullptr, &device) == VK_SUCCESS);

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = device;
    backend.vkPhysicalDevice = physical;
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale upscale{};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.header.pNext = &backend.header;
    upscale.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    upscale.maxRenderSize = {1280, 720};
    upscale.maxUpscaleSize = {3840, 2160};
    ffxContext upscaler = nullptr;
    assert(ffxCreateContext(&upscaler, &upscale.header, nullptr) == FFX_API_RETURN_OK);
    ffxQueryGetProviderVersion version{};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    assert(ffxQuery(&upscaler, &version.header) == FFX_API_RETURN_OK);
    std::printf("Upscaler: %s on %s\n", version.versionName, properties.deviceName);

    ffxCreateContextDescFrameGeneration generation{};
    generation.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    generation.header.pNext = &backend.header;
    generation.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    generation.displaySize = {3840, 2160};
    generation.maxRenderSize = {1280, 720};
    generation.backBufferFormat = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    ffxContext generator = nullptr;
    assert(ffxCreateContext(&generator, &generation.header, nullptr) == FFX_API_RETURN_OK);
    std::puts("Frame generation context created");

    assert(ffxDestroyContext(&generator, nullptr) == FFX_API_RETURN_OK);
    assert(ffxDestroyContext(&upscaler, nullptr) == FFX_API_RETURN_OK);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts("FidelityFX contexts PASS");
}
