#include "fsi_device.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }
namespace gow3::renderer {
using namespace plume;
namespace {
struct Lease final:LiveQueueLease {std::unique_lock<std::mutex> lock;explicit Lease(std::mutex& mutex):lock(mutex){}};
}
struct FsiDevice::State {
    std::unique_ptr<RenderInterface> interface=CreateVulkanInterface();
    VkDevice device=VK_NULL_HANDLE;
    std::mutex mutex;
    LiveVulkanDeviceDesc desc;
    bool pipeline_executable_info=false;
    std::unique_ptr<LiveVulkanDevice> borrowed;
    explicit State(bool request_stencil_export, bool request_pipeline_executable_info, bool request_store_op_none) {
        if(!interface)throw std::runtime_error("Vulkan instance unavailable");
        auto& vk=static_cast<VulkanInterface&>(*interface);desc.instance=vk.instance;desc.api_version=vk.appInfo.apiVersion;
        uint32_t count=0;vkEnumeratePhysicalDevices(desc.instance,&count,nullptr);
        std::vector<VkPhysicalDevice> physical(count);vkEnumeratePhysicalDevices(desc.instance,&count,physical.data());
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        for(auto candidate:physical) {
            VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(candidate,&properties);
            if(properties.apiVersion<VK_API_VERSION_1_2)continue;
            desc.features12={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};desc.features12.pNext=&interlock;
            VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&desc.features12;
            vkGetPhysicalDeviceFeatures2(candidate,&f);
            if(!interlock.fragmentShaderPixelInterlock||!f.features.fragmentStoresAndAtomics||!desc.features12.bufferDeviceAddress)continue;
            uint32_t n=0;vkEnumerateDeviceExtensionProperties(candidate,nullptr,&n,nullptr);
            std::vector<VkExtensionProperties> extensions(n);vkEnumerateDeviceExtensionProperties(candidate,nullptr,&n,extensions.data());
            if(std::none_of(extensions.begin(),extensions.end(),[](auto& e){return !std::strcmp(e.extensionName,VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);}))continue;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,families.data());
            for(uint32_t i=0;i<n;++i)if((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) {
                desc.physical_device=candidate;desc.features=f.features;desc.queue_family=i;break;
            }
            if(desc.physical_device)break;
        }
        if(!desc.physical_device)throw std::runtime_error("No device supports ordered pixel interlock");
        // Capability is published only for extensions placed in vkCreateDevice.
        // No feature struct exists for VK_EXT_shader_stencil_export.
        std::vector<const char*> enabled_extensions{VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME};
        if(request_store_op_none) {
            uint32_t n=0;
            if(vkEnumerateDeviceExtensionProperties(desc.physical_device,nullptr,&n,nullptr)==VK_SUCCESS) {
                std::vector<VkExtensionProperties> extensions(n);
                if(vkEnumerateDeviceExtensionProperties(desc.physical_device,nullptr,&n,extensions.data())==VK_SUCCESS&&
                   std::any_of(extensions.begin(),extensions.begin()+n,[](const auto& e) {
                       return !std::strcmp(e.extensionName,VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
                   })) {
                    enabled_extensions.push_back(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
                    desc.store_op_none=true;
                }
            }
        }
        if(request_stencil_export) {
            uint32_t count=0;
            if(vkEnumerateDeviceExtensionProperties(desc.physical_device,nullptr,&count,nullptr)==VK_SUCCESS) {
                std::vector<VkExtensionProperties> extensions(count);
                if(vkEnumerateDeviceExtensionProperties(desc.physical_device,nullptr,&count,extensions.data())==VK_SUCCESS&&
                   std::any_of(extensions.begin(),extensions.begin()+count,[](const auto& extension) {
                       return !std::strcmp(extension.extensionName,VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
                   })) {
                    enabled_extensions.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
                    desc.shader_stencil_export=true;
                }
            }
        }
        // Standalone diagnostic only. Publish enabled capability after both
        // extension presence and queried feature support, never from a PFN.
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
        if(request_pipeline_executable_info) {
            uint32_t n=0;
            if(vkEnumerateDeviceExtensionProperties(desc.physical_device,nullptr,&n,nullptr)==VK_SUCCESS) {
                std::vector<VkExtensionProperties> extensions(n);
                if(vkEnumerateDeviceExtensionProperties(desc.physical_device,nullptr,&n,extensions.data())==VK_SUCCESS&&
                   std::any_of(extensions.begin(),extensions.begin()+n,[](const auto& e) {
                       return !std::strcmp(e.extensionName,VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
                   })) {
                    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                    supported.pNext=&executable;vkGetPhysicalDeviceFeatures2(desc.physical_device,&supported);
                    if(executable.pipelineExecutableInfo) {
                        enabled_extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
                        interlock.pNext=&executable;pipeline_executable_info=true;
                    }
                }
            }
        }
        interlock.fragmentShaderSampleInterlock=false;interlock.fragmentShaderShadingRateInterlock=false;
        const float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex=desc.queue_family;queue.queueCount=1;queue.pQueuePriorities=&priority;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};create.pNext=&desc.features12;create.pEnabledFeatures=&desc.features;
        create.queueCreateInfoCount=1;create.pQueueCreateInfos=&queue;create.enabledExtensionCount=uint32_t(enabled_extensions.size());create.ppEnabledExtensionNames=enabled_extensions.data();
        if(vkCreateDevice(desc.physical_device,&create,nullptr,&device)!=VK_SUCCESS)throw std::runtime_error("Cannot enable interlock Vulkan device");
        volkLoadDevice(device);desc.device=device;desc.ordered_pixel_interlock=true;vkGetDeviceQueue(device,desc.queue_family,0,&desc.queue);
        desc.features12.pNext=nullptr;desc.acquire_queue=[&]{return std::make_unique<Lease>(mutex);};
        borrowed=std::make_unique<LiveVulkanDevice>(desc);
    }
    ~State() {borrowed.reset();if(device)vkDestroyDevice(device,nullptr);}
};
FsiDevice::FsiDevice(bool request_stencil_export, bool request_pipeline_executable_info, bool request_store_op_none)
    :state_(std::make_unique<State>(request_stencil_export,request_pipeline_executable_info,request_store_op_none)) {}
FsiDevice::~FsiDevice()=default;
plume::RenderDevice& FsiDevice::device() {return state_->borrowed->device();}
bool FsiDevice::stencil_export_enabled() const {return state_->desc.shader_stencil_export;}
bool FsiDevice::pipeline_statistics_enabled() const {return state_->desc.features.pipelineStatisticsQuery;}
}

namespace gow3::renderer {
bool FsiDevice::pipeline_executable_info_enabled() const {return state_->pipeline_executable_info;}
}
