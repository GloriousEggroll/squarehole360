// Windows: loads libxess.dll, creates a Vulkan device with XeSS's required
// extensions and features, then initializes XeSS for 720p -> 4K and runs one
// upscale on cleared images, as the presenter does. No game, no window.
#undef NDEBUG
#include <xess/xess_vk.h>
#include <vulkan/vulkan.h>
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
template <typename F>
F Load(HMODULE module, const char* name) {
    auto f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    assert(f);
    return f;
}
struct Image {VkImage image; VkImageView view; VkDeviceMemory memory; VkFormat format; uint32_t w, h;};
Image Make(VkDevice device, VkPhysicalDevice physical, VkFormat format, uint32_t w, uint32_t h) {
    Image out{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, format, w, h};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {w, h, 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(vkCreateImage(device, &info, nullptr, &out.image) == VK_SUCCESS);
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, out.image, &req);
    VkPhysicalDeviceMemoryProperties mem; vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    uint32_t type = 0;
    while (!((req.memoryTypeBits >> type) & 1) ||
           !(mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ++type;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
    assert(vkAllocateMemory(device, &alloc, nullptr, &out.memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, out.image, out.memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    assert(vkCreateImageView(device, &view, nullptr, &out.view) == VK_SUCCESS);
    return out;
}
xess_vk_image_view_info Info(const Image& i) {
    return {i.view, i.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, i.format, i.w, i.h};
}
}  // namespace

int main() {
    HMODULE xess = LoadLibraryW(L"libxess.dll");
    if (!xess) { std::puts("libxess.dll not found; skipped"); return 0; }
    auto get_instance_extensions = Load<decltype(&xessVKGetRequiredInstanceExtensions)>(xess, "xessVKGetRequiredInstanceExtensions");
    auto get_device_extensions = Load<decltype(&xessVKGetRequiredDeviceExtensions)>(xess, "xessVKGetRequiredDeviceExtensions");
    auto get_device_features = Load<decltype(&xessVKGetRequiredDeviceFeatures)>(xess, "xessVKGetRequiredDeviceFeatures");
    auto create_context = Load<decltype(&xessVKCreateContext)>(xess, "xessVKCreateContext");
    auto init = Load<decltype(&xessVKInit)>(xess, "xessVKInit");
    auto execute = Load<decltype(&xessVKExecute)>(xess, "xessVKExecute");
    auto destroy = Load<decltype(&xessDestroyContext)>(xess, "xessDestroyContext");

    uint32_t count = 0, min_api = 0;
    const char* const* names = nullptr;
    assert(get_instance_extensions(&count, &names, &min_api) == XESS_RESULT_SUCCESS);
    std::vector<const char*> instance_extensions(names, names + count);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = uint32_t(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    VkInstance instance;
    assert(vkCreateInstance(&instance_info, nullptr, &instance) == VK_SUCCESS);
    uint32_t devices = 1;
    VkPhysicalDevice physical;
    vkEnumeratePhysicalDevices(instance, &devices, &physical);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);

    assert(get_device_extensions(instance, physical, &count, &names) == XESS_RESULT_SUCCESS);
    std::vector<const char*> device_extensions(names, names + count);
    for (const char* name : device_extensions) std::printf("XeSS device extension: %s\n", name);
    void* features = nullptr;
    assert(get_device_features(instance, physical, &features) == XESS_RESULT_SUCCESS);
    for (auto* node = static_cast<const VkBaseInStructure*>(features); node; node = node->pNext)
        std::printf("XeSS feature structure: %u\n", uint32_t(node->sType));

    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueCount = 1; queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = features;
    device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue;
    device_info.enabledExtensionCount = uint32_t(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    VkDevice device;
    assert(vkCreateDevice(physical, &device_info, nullptr, &device) == VK_SUCCESS);

    xess_context_handle_t context = nullptr;
    assert(create_context(instance, physical, device, &context) == XESS_RESULT_SUCCESS);
    xess_vk_init_params_t params{};
    params.outputResolution = {3840, 2160};
    params.qualitySetting = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    params.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_LDR_INPUT_COLOR |
                       XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    params.creationNodeMask = params.visibleNodeMask = 1;
    const xess_result_t init_result = init(context, &params);
    std::printf("XeSS init on %s: %d\n", properties.deviceName, int(init_result));
    assert(init_result == XESS_RESULT_SUCCESS);

    Image color = Make(device, physical, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1280, 720);
    Image depth = Make(device, physical, VK_FORMAT_R32_SFLOAT, 1280, 720);
    Image motion = Make(device, physical, VK_FORMAT_R16G16_SFLOAT, 1280, 720);
    Image reactive = Make(device, physical, VK_FORMAT_R8_UNORM, 1280, 720);
    Image output = Make(device, physical, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 3840, 2160);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool; assert(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_info.commandPool = pool; cb_info.commandBufferCount = 1;
    VkCommandBuffer cb; assert(vkAllocateCommandBuffers(device, &cb_info, &cb) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin);
    std::vector<VkImageMemoryBarrier> barriers;
    for (const Image* i : {&color, &depth, &motion, &reactive, &output}) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.newLayout = i == &output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = i->image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers.push_back(b);
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
    xess_vk_execute_params_t run{};
    run.colorTexture = Info(color); run.velocityTexture = Info(motion); run.depthTexture = Info(depth);
    run.responsivePixelMaskTexture = Info(reactive); run.outputTexture = Info(output);
    run.exposureScale = 1; run.resetHistory = 1; run.inputWidth = 1280; run.inputHeight = 720;
    const xess_result_t execute_result = execute(context, cb, &run);
    std::printf("XeSS execute: %d\n", int(execute_result));
    assert(execute_result == XESS_RESULT_SUCCESS);
    vkEndCommandBuffer(cb);
    VkQueue q; vkGetDeviceQueue(device, 0, 0, &q);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
    assert(vkQueueSubmit(q, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(q) == VK_SUCCESS);
    destroy(context);
    std::puts("XeSS PASS");
    return 0;
}
