#pragma once
#include "plume_vulkan.h"
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace gow3::renderer {
// Address and byte count, not an allocation extent. Bytes outside these ranges
// belong to the recipient and must survive a renderer transition unchanged.
using NativeStateRanges=std::vector<std::pair<uint32_t,uint32_t>>;
struct NativeMemoryState {
    VkBuffer buffer=VK_NULL_HANDLE;
    uint32_t capacity=0;
    NativeStateRanges ranges;
};
struct NativeRendererState { NativeMemoryState physical,edram; };

// Same-device, same-queue handback. Sources and destinations must remain alive
// until Copy returns. Sub-word ranges are merged on the GPU, preserving adjacent
// bytes. No guest CPU memory is read or published by this operation.
class NativeStateTransfer {
public:
    NativeStateTransfer(plume::RenderDevice&,plume::RenderCommandQueue&,uint32_t descriptor_window=0);
    ~NativeStateTransfer();
    void Copy(const NativeMemoryState&,VkBuffer destination);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
