#include "gpu_helpers.h"
#include "renderer/native/live_device.h"
#include <array>
#include <iostream>
#include <vector>

using namespace plume;
using namespace gow3::renderer;

namespace {
struct Lease final : LiveQueueLease {
    std::unique_lock<std::mutex> lock;
    explicit Lease(std::mutex& mutex) : lock(mutex) {}
};
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void TestCopies(RenderDevice& device, RenderCommandQueue& queue, bool semaphore_wait) {
    std::array<uint32_t, 1024> input{};
    for (size_t i = 0; i < input.size(); ++i) input[i] = uint32_t(i * 0x1234567u) ^ 0xa55ac33cu;
    auto upload = device.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(input)));
    auto readback = device.createBuffer(RenderBufferDesc::ReadbackBuffer(sizeof(input)));
    std::memcpy(upload->map(), input.data(), sizeof(input)); validation::Flush(*upload); upload->unmap();
    auto commands = queue.createCommandList();
    auto fence = device.createCommandFence();
    auto signal0 = device.createCommandSemaphore(), signal1 = device.createCommandSemaphore();
    commands->begin(); commands->copyBuffer(readback.get(), upload.get()); commands->end();
    const RenderCommandList* lists[] = {commands.get()};
    RenderCommandSemaphore* signals[] = {signal0.get(), signal1.get()};
    queue.executeCommandLists(lists, 1, nullptr, 0, semaphore_wait ? signals : nullptr,
                              semaphore_wait ? 2 : 0, fence.get());
    queue.waitForCommandFence(fence.get());
    if (semaphore_wait) {
        commands->begin(); commands->copyBuffer(readback.get(), upload.get()); commands->end();
        queue.executeCommandLists(lists, 1, signals, 2, nullptr, 0, fence.get());
        queue.waitForCommandFence(fence.get());
    }
    const auto* mapped = readback->map();
    auto& buffer = static_cast<VulkanBuffer&>(*readback);
    Require(vmaInvalidateAllocation(buffer.device->allocator, buffer.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
            "Failed to invalidate readback");
    Require(std::memcmp(mapped, input.data(), sizeof(input)) == 0, "Borrowed-device buffer submission changed bytes");
    readback->unmap();
}
void TestSubmissionExtent(RenderDevice& device, RenderCommandQueue& queue, uint32_t count) {
    std::vector<uint32_t> input(count);
    for(uint32_t i=0;i<count;++i)input[i]=0xa5310000u^(i*0x12345u);
    const size_t bytes=input.size()*sizeof(uint32_t);
    auto upload=device.createBuffer(RenderBufferDesc::UploadBuffer(bytes));
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    auto* upload_data=upload->map();Require(upload_data!=nullptr,"Submission extent upload mapping failed");
    std::memcpy(upload_data,input.data(),bytes);validation::Flush(*upload);upload->unmap();
    std::vector<std::unique_ptr<RenderCommandList>> owners;
    std::vector<const RenderCommandList*> lists;
    for(uint32_t i=0;i<count;++i) {
        auto command=queue.createCommandList();command->begin();
        VkBufferCopy region{uint64_t(i)*4,uint64_t(i)*4,4};
        vkCmdCopyBuffer(static_cast<VulkanCommandList&>(*command).vk,
            static_cast<VulkanBuffer&>(*upload).vk,static_cast<VulkanBuffer&>(*readback).vk,1,&region);
        command->end();lists.push_back(command.get());owners.push_back(std::move(command));
    }
    auto fence=device.createCommandFence();
    queue.executeCommandLists(lists.data(),count,nullptr,0,nullptr,0,fence.get());
    queue.waitForCommandFence(fence.get());
    auto* mapped=readback->map();auto& buffer=static_cast<VulkanBuffer&>(*readback);
    Require(mapped!=nullptr,"Submission extent readback mapping failed");
    Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Submission extent readback invalidation failed");
    Require(!std::memcmp(mapped,input.data(),bytes),"Submission extent dropped or changed command handles");
    readback->unmap();
}
}

int main() {
    try {
        auto host_interface = CreateVulkanInterface();
        auto host_device = host_interface ? host_interface->createDevice() : nullptr;
        Require(bool(host_device), "Vulkan unavailable");
        auto host_queue = host_device->createCommandQueue(RenderCommandListType::DIRECT);
        auto& device = static_cast<VulkanDevice&>(*host_device);
        auto& interface = static_cast<VulkanInterface&>(*host_interface);
        auto& queue = static_cast<VulkanCommandQueue&>(*host_queue);
        LiveVulkanDeviceDesc desc;
        desc.instance = interface.instance;
        desc.physical_device = device.physicalDevice;
        desc.device = device.vk;
        desc.queue = queue.queue->vk;
        desc.queue_family = queue.familyIndex;
        desc.api_version = interface.appInfo.apiVersion;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &desc.features12;
        vkGetPhysicalDeviceFeatures2(desc.physical_device, &features);
        // Plume's ordinary constructor does not enable host query reset.
        desc.features12.hostQueryReset = device.hostQueryResetEnabled;
        // The standalone Plume constructor enables supported core features and
        // the full queried indexing/scalar/address feature extension structs.
        desc.features = features.features;
        desc.null_descriptor = device.nullDescriptorSupported;
        unsigned lock_count = 0;
        desc.acquire_queue = [&]() { ++lock_count; return std::make_unique<Lease>(*queue.queue->mutex); };
        Require(CheckLiveVulkanDevice(desc).empty(), "Host lacks native renderer features");
        auto missing = desc; missing.features12.bufferDeviceAddress = false;
        Require(!CheckLiveVulkanDevice(missing).empty(), "Feature gate accepted disabled buffer addresses");
        bool rejected = false;
        try { LiveVulkanDevice bad(missing); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Invalid device was not rejected before allocation");
        TestCopies(*host_device, *host_queue, false);
        {
            LiveVulkanDevice native(desc);
            Require(static_cast<VulkanDevice&>(native.device()).vk == device.vk, "Adapter created a private live device");
            auto native_queue = native.device().createCommandQueue(RenderCommandListType::DIRECT);
            TestCopies(native.device(), *native_queue, true);
            TestSubmissionExtent(native.device(), *native_queue,130);
            TestSubmissionExtent(native.device(), *native_queue,131);
            auto target = native.device().createTexture(RenderTextureDesc::ColorTarget(16, 16, RenderFormat::R8G8B8A8_UNORM));
            auto commands = native_queue->createCommandList(); auto fence = native.device().createCommandFence();
            commands->begin();
            commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_DEST));
            auto& image = static_cast<VulkanTexture&>(*target);
            VkClearColorValue color{{1.f, 0.f, 0.f, 1.f}};
            vkCmdClearColorImage(static_cast<VulkanCommandList&>(*commands).vk, image.vk,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &image.imageSubresourceRange);
            commands->end(); native_queue->executeCommandLists(commands.get(), fence.get()); native_queue->waitForCommandFence(fence.get());
            auto pixels = validation::ReadImage(native.device(), *native_queue, *target, 4);
            for (size_t i = 0; i < pixels.size(); i += 4)
                Require(pixels[i] == 255 && pixels[i+1] == 0 && pixels[i+2] == 0 && pixels[i+3] == 255,
                        "Borrowed image submission mismatch");
            native.WaitIdle();
        }
        // Destruction must not destroy the owner's device or instance.
        TestCopies(*host_device, *host_queue, false);
        Require(lock_count >= 5, "Native submissions did not acquire the host queue lock");
        std::cout << "Shared Vulkan device: feature rejection, two semaphore waits, image/buffer submission, "
                  << "130/131-list submission extents, host queue locking and host survival passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
