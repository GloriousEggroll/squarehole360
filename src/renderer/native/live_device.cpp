#include "live_device.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace gow3::renderer {
namespace {
void Check(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(action) + ": Vulkan result " + std::to_string(result));
}

// Plume's queue uses a private mutex. Vulkan queues are externally synchronized,
// so every submit must also use the queue owner's lock. Recording and fence
// waiting require no queue lock. Submit errors must not become an infinite
// fence wait; propagate them here instead of Plume's stderr-only handling.
class SharedQueue final : public plume::VulkanCommandQueue, public NativeCheckedQueueCompletion {
    std::function<std::unique_ptr<LiveQueueLease>()> acquire_;
public:
    SharedQueue(plume::VulkanDevice* device, plume::RenderCommandListType type,
                std::function<std::unique_ptr<LiveQueueLease>()> acquire)
        : VulkanCommandQueue(device, type), acquire_(std::move(acquire)) {}

    std::unique_ptr<plume::RenderSwapChain> createSwapChain(const plume::RenderSwapChainDesc&) override {
        throw std::runtime_error("The live renderer uses the host presenter");
    }

    void executeCommandLists(const plume::RenderCommandList** lists, uint32_t count,
            plume::RenderCommandSemaphore** waits, uint32_t wait_count,
            plume::RenderCommandSemaphore** signals, uint32_t signal_count,
            plume::RenderCommandFence* fence) override {
        if (!lists || !count) throw std::invalid_argument("Empty live submission");
        // The bounded Native queue submits at most128 lists plus two optional
        // timestamp markers. Keep their temporary handles on this invocation's
        // stack; larger external callers retain the dynamically sized path.
        // Per-call storage also preserves concurrent/reentrant submissions.
        std::array<VkCommandBuffer,130> inline_commands;
        std::vector<VkCommandBuffer> overflow_commands;
        auto* commands=inline_commands.data();
        if(count>inline_commands.size()) {
            overflow_commands.resize(count);
            commands=overflow_commands.data();
        }
        std::vector<VkSemaphore> wait_handles, signal_handles;
        // A wait may protect storage/compute/transfer work as well as color
        // writes. Each semaphore needs its own stage-mask array entry.
        std::vector<VkPipelineStageFlags> wait_stages(wait_count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        for (uint32_t i = 0; i < count; ++i) {
            if (!lists[i]) throw std::invalid_argument("Null live command list");
            const auto* list = static_cast<const plume::VulkanCommandList*>(lists[i]);
            if (list->queue->device != device) throw std::invalid_argument("Foreign live command list");
            commands[i]=list->vk;
        }
        for (uint32_t i = 0; i < wait_count; ++i) {
            if (!waits || !waits[i]) throw std::invalid_argument("Null live wait semaphore");
            const auto* semaphore = static_cast<const plume::VulkanCommandSemaphore*>(waits[i]);
            if (semaphore->device != device) throw std::invalid_argument("Foreign live wait semaphore");
            wait_handles.push_back(semaphore->vk);
        }
        for (uint32_t i = 0; i < signal_count; ++i) {
            if (!signals || !signals[i]) throw std::invalid_argument("Null live signal semaphore");
            const auto* semaphore = static_cast<const plume::VulkanCommandSemaphore*>(signals[i]);
            if (semaphore->device != device) throw std::invalid_argument("Foreign live signal semaphore");
            signal_handles.push_back(semaphore->vk);
        }
        VkFence signal_fence = VK_NULL_HANDLE;
        if (fence) {
            const auto* vk_fence = static_cast<const plume::VulkanCommandFence*>(fence);
            if (vk_fence->device != device) throw std::invalid_argument("Foreign live fence");
            signal_fence = vk_fence->vk;
        }
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = count;
        info.pCommandBuffers = commands;
        info.waitSemaphoreCount = wait_count;
        info.pWaitSemaphores = wait_handles.data();
        info.pWaitDstStageMask = wait_stages.data();
        info.signalSemaphoreCount = signal_count;
        info.pSignalSemaphores = signal_handles.data();
        auto lease = acquire_();
        if (!lease) throw std::runtime_error("Host did not lock its graphics queue");
        Check(vkQueueSubmit(queue->vk, 1, &info, signal_fence), "Native queue submission failed");
    }

    void waitForCommandFence(plume::RenderCommandFence* fence) override {
        const auto* vk_fence = dynamic_cast<const plume::VulkanCommandFence*>(fence);
        if (!vk_fence || vk_fence->device != device) throw std::invalid_argument("Foreign live fence");
        Check(vkWaitForFences(device->vk, 1, &vk_fence->vk, VK_TRUE, UINT64_MAX), "Native fence wait failed");
        Check(vkResetFences(device->vk, 1, &vk_fence->vk), "Native fence reset failed");
    }
};

class BorrowedDevice final : public plume::VulkanDevice {
    std::function<std::unique_ptr<LiveQueueLease>()> acquire_;
    bool borrowed_ = false;
public:
    BorrowedDevice(plume::VulkanInterface* interface)
        : VulkanDevice(interface, "") {}
    ~BorrowedDevice() override {
        if (borrowed_) {
            nullBuffer.reset();
            if (allocator) vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
            // The base destructor may release only its own bootstrap device.
            vk = VK_NULL_HANDLE;
        }
    }

    void Adopt(const LiveVulkanDeviceDesc& d) {
        // Plume has no public import constructor. Initialize its concrete C++
        // object normally, then release that temporary logical device before
        // replacing its public backend state. No vendor sources are changed.
        release();
        queueFamilies.clear();
        physicalDevice = d.physical_device;
        vk = d.device;
        borrowed_ = true;
        acquire_ = d.acquire_queue;
        vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &family_count, families.data());
        if (d.queue_family >= family_count ||
            (families[d.queue_family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            throw std::invalid_argument("Native renderer requires a graphics/compute queue");
        queueFamilies.resize(family_count);
        auto& q = queueFamilies[d.queue_family].queues.emplace_back();
        q.vk = d.queue;
        q.mutex = std::make_unique<std::mutex>();
        std::fill(std::begin(queueFamilyIndices), std::end(queueFamilyIndices), d.queue_family);

        capabilities = {};
        capabilities.geometryShader = d.features.geometryShader;
        capabilities.descriptorIndexing = true;
        capabilities.scalarBlockLayout = true;
        capabilities.bufferDeviceAddress = true;
        capabilities.samplerMirrorClampToEdge = d.features12.samplerMirrorClampToEdge;
        capabilities.resolveRegion = true;
        capabilities.maxTextureSize = physicalDeviceProperties.limits.maxImageDimension2D;
        capabilities.dynamicDepthBias = true;
        capabilities.queryPools = true;
        capabilities.triangleFan = true;
        loadStoreOpNoneSupported = false;
        hostQueryResetEnabled = d.features12.hostQueryReset;
        storeOpNoneSupported = d.store_op_none ||
            std::min(d.api_version, physicalDeviceProperties.apiVersion) >= VK_API_VERSION_1_3;
        nullDescriptorSupported = d.null_descriptor;
        rtPipelineProperties = {};
        sampleLocationProperties = {};
        description = {};
        description.name = physicalDeviceProperties.deviceName;
        description.driverVersion = physicalDeviceProperties.driverVersion;
        description.vendor = plume::RenderDeviceVendor(physicalDeviceProperties.vendorID);

        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo info{};
        info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        info.instance = d.instance;
        info.physicalDevice = d.physical_device;
        info.device = d.device;
        info.vulkanApiVersion = std::min(d.api_version, physicalDeviceProperties.apiVersion);
        info.pVulkanFunctions = &functions;
        Check(vmaCreateAllocator(&info, &allocator), "Native shared allocator creation failed");
        if (!nullDescriptorSupported)
            nullBuffer = createBuffer(plume::RenderBufferDesc::DefaultBuffer(16, plume::RenderBufferFlag::VERTEX));
    }

    std::unique_ptr<plume::RenderCommandQueue> createCommandQueue(plume::RenderCommandListType type) override {
        return std::make_unique<SharedQueue>(this, type, acquire_);
    }
};
} // namespace

std::string CheckLiveVulkanDevice(const LiveVulkanDeviceDesc& d) {
    if (!d.instance || !d.physical_device || !d.device || !d.queue || !d.acquire_queue)
        return "Native renderer requires live device/queue handles and the host queue lock";
    if (d.api_version < VK_API_VERSION_1_2)
        return "Native renderer requires Vulkan 1.2";
    if (!d.features.shaderInt64 || !d.features.shaderSampledImageArrayDynamicIndexing)
        return "Native shader integer/addressing features were not enabled";
    const auto& f = d.features12;
    if (!f.bufferDeviceAddress || !f.scalarBlockLayout || !f.runtimeDescriptorArray ||
        !f.descriptorBindingPartiallyBound || !f.descriptorBindingVariableDescriptorCount ||
        !f.descriptorBindingSampledImageUpdateAfterBind || !f.shaderSampledImageArrayNonUniformIndexing)
        return "Native buffer-address or descriptor-indexing features were not enabled";
    return {};
}

struct LiveVulkanDevice::State {
    // Declaration order matters: the device is destroyed before its interface.
    std::unique_ptr<plume::VulkanInterface> interface;
    std::unique_ptr<BorrowedDevice> device;
    LiveVulkanDeviceDesc desc;
    bool borrowed_instance = false;
    ~State() {
        device.reset();
        if (borrowed_instance) interface->instance = VK_NULL_HANDLE;
    }
};

LiveVulkanDevice::LiveVulkanDevice(const LiveVulkanDeviceDesc& desc) {
    const auto unsupported = CheckLiveVulkanDevice(desc);
    if (!unsupported.empty()) throw std::invalid_argument(unsupported);
    auto state = std::make_unique<State>();
    state->desc = desc;
    state->interface = std::make_unique<plume::VulkanInterface>();
    if (!state->interface->isValid()) throw std::runtime_error("Cannot initialize Vulkan adapter");
    state->device = std::make_unique<BorrowedDevice>(state->interface.get());
    state->device->release();
    vkDestroyInstance(state->interface->instance, nullptr);
    state->interface->instance = desc.instance;
    state->interface->appInfo.apiVersion = desc.api_version;
    state->borrowed_instance = true;
    volkLoadInstance(desc.instance);
    state->device->Adopt(desc);
    state_ = std::move(state);
}

LiveVulkanDevice::~LiveVulkanDevice() = default;
plume::RenderDevice& LiveVulkanDevice::device() { return *state_->device; }
void LiveVulkanDevice::WaitIdle() {
    auto lease = state_->desc.acquire_queue();
    if (!lease) throw std::runtime_error("Host did not lock its graphics queue");
    Check(vkQueueWaitIdle(state_->desc.queue), "Native queue idle wait failed");
}
} // namespace gow3::renderer
