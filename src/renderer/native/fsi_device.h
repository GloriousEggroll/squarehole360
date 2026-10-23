#pragma once
#include "live_device.h"
namespace gow3::renderer {
// Standalone diagnostics need an explicitly interlock-enabled device. Gameplay
// borrows its existing enabled Vulkan device through LiveVulkanDevice instead.
class FsiDevice {
public:
    explicit FsiDevice(bool request_stencil_export=false, bool request_pipeline_executable_info=false,
                       bool request_store_op_none=false);
    ~FsiDevice();
    FsiDevice(const FsiDevice&)=delete;
    FsiDevice& operator=(const FsiDevice&)=delete;
    plume::RenderDevice& device();
    bool stencil_export_enabled() const;
    bool pipeline_executable_info_enabled() const;
    bool pipeline_statistics_enabled() const; // Actual core feature enabled in vkCreateDevice.
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
