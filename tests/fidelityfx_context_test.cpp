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

namespace {
struct Image {VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkFormat format; uint32_t w, h;};
Image MakeImage(VkDevice device, VkPhysicalDevice physical, VkFormat format, uint32_t w, uint32_t h) {
    Image out{VK_NULL_HANDLE, VK_NULL_HANDLE, format, w, h};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {w, h, 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(vkCreateImage(device, &info, nullptr, &out.image) == VK_SUCCESS);
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, out.image, &req);
    VkPhysicalDeviceMemoryProperties mem; vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    uint32_t type = 0;
    for (; type < mem.memoryTypeCount; ++type)
        if ((req.memoryTypeBits & (1u << type)) && (mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
    assert(vkAllocateMemory(device, &alloc, nullptr, &out.memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, out.image, out.memory, 0) == VK_SUCCESS);
    return out;
}
FfxApiResource Resource(const Image& i, uint32_t usage, uint32_t state) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = i.format; info.extent = {i.w, i.h, 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(i.image), ffxApiGetImageResourceDescriptionVK(i.image, info, usage), state);
}
}  // namespace

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
    // As the presenter configures it: no FidelityFX swapchain.
    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.frameGenerationEnabled = true;
    configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    assert(ffxConfigure(&generator, &configure.header) == FFX_API_RETURN_OK);
    std::puts("Frame generation configured");

    // One upscale and one frame generation dispatch, as the presenter records them.
    {
        constexpr uint32_t rw = 320, rh = 180, dw = 960, dh = 540;
        Image color = MakeImage(device, physical, VK_FORMAT_A2B10G10R10_UNORM_PACK32, rw, rh);
        Image depth = MakeImage(device, physical, VK_FORMAT_R32_SFLOAT, rw, rh);
        Image motion = MakeImage(device, physical, VK_FORMAT_R16G16_SFLOAT, rw, rh);
        Image reactive = MakeImage(device, physical, VK_FORMAT_R8_UNORM, rw, rh);
        Image real = MakeImage(device, physical, VK_FORMAT_A2B10G10R10_UNORM_PACK32, dw, dh);
        Image generated = MakeImage(device, physical, VK_FORMAT_A2B10G10R10_UNORM_PACK32, dw, dh);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        VkCommandPool pool; assert(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb_info.commandPool = pool; cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cb_info.commandBufferCount = 1;
        VkCommandBuffer cb; assert(vkAllocateCommandBuffers(device, &cb_info, &cb) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        assert(vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS);
        std::vector<VkImageMemoryBarrier> barriers;
        for (const Image* i : {&color, &depth, &motion, &reactive, &real, &generated}) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.newLayout = (i == &real || i == &generated) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = i->image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers.push_back(b);
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             uint32_t(barriers.size()), barriers.data());
        constexpr uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        ffxDispatchDescUpscale up{};
        up.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        up.commandList = cb;
        up.color = Resource(color, FFX_API_RESOURCE_USAGE_READ_ONLY, kRead);
        up.depth = Resource(depth, FFX_API_RESOURCE_USAGE_READ_ONLY, kRead);
        up.motionVectors = Resource(motion, FFX_API_RESOURCE_USAGE_READ_ONLY, kRead);
        up.reactive = Resource(reactive, FFX_API_RESOURCE_USAGE_READ_ONLY, kRead);
        up.output = Resource(real, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        up.motionVectorScale = {1, 1}; up.renderSize = {rw, rh}; up.upscaleSize = {dw, dh};
        up.reset = true; up.frameTimeDelta = 16.6f; up.preExposure = 1; up.cameraNear = 10; up.cameraFar = 1e9f;
        up.cameraFovAngleVertical = 0.5f; up.viewSpaceToMetersFactor = 0.01f;
        assert(ffxDispatch(&upscaler, &up.header) == FFX_API_RETURN_OK);
        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.frameID = 1; prepare.commandList = cb; prepare.renderSize = {rw, rh};
        prepare.motionVectorScale = {1, 1}; prepare.frameTimeDelta = 16.6f; prepare.cameraNear = 10; prepare.cameraFar = 1e9f;
        prepare.cameraFovAngleVertical = 0.5f; prepare.viewSpaceToMetersFactor = 0.01f;
        prepare.depth = up.depth; prepare.motionVectors = up.motionVectors;
        assert(ffxDispatch(&generator, &prepare.header) == FFX_API_RETURN_OK);
        // The presenter moves the real frame to SHADER_READ before generation.
        VkImageMemoryBarrier to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL; to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcQueueFamilyIndex = to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.image = real.image; to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_read);
        ffxDispatchDescFrameGeneration fg{};
        fg.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        fg.commandList = cb;
        fg.presentColor = Resource(real, FFX_API_RESOURCE_USAGE_READ_ONLY, kRead);
        fg.outputs[0] = Resource(generated, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        fg.numGeneratedFrames = 1; fg.reset = true; fg.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        fg.minMaxLuminance[1] = 1; fg.frameID = 1;
        assert(ffxDispatch(&generator, &fg.header) == FFX_API_RETURN_OK);
        assert(vkEndCommandBuffer(cb) == VK_SUCCESS);
        VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
        assert(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
        assert(vkQueueWaitIdle(queue) == VK_SUCCESS);
        std::puts("Upscale and frame generation dispatched");
        vkDestroyCommandPool(device, pool, nullptr);
        for (const Image* i : {&color, &depth, &motion, &reactive, &real, &generated}) {
            vkDestroyImage(device, i->image, nullptr); vkFreeMemory(device, i->memory, nullptr);
        }
    }

    assert(ffxDestroyContext(&generator, nullptr) == FFX_API_RETURN_OK);
    assert(ffxDestroyContext(&upscaler, nullptr) == FFX_API_RETURN_OK);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts("FidelityFX contexts PASS");
}
