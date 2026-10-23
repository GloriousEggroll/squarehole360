#pragma once
#include "fsi_state_specialization.h"
#include "plume_render_interface_types.h"
namespace gow3::renderer {
// Owned for the complete synchronous createGraphicsPipeline call. Add this to
// NativeFsiPipelineDescriptor; the currently passed Draw descriptor is replaced
// by Pipeline() and cannot supply these constants directly.
struct NativeFsiSpecializationConstants {
    std::array<plume::RenderSpecConstant,17> values{};
    NativeFsiSpecializationConstants(uint32_t alpha, const NativeFsiStateSpecialization& state) {
        values[0]=plume::RenderSpecConstant(0,alpha);
        values[1]=plume::RenderSpecConstant(32,1);
        for (uint32_t i=0;i<state.words.size();++i)
            values[i+2]=plume::RenderSpecConstant(i+33,state.words[i]);
    }
};
}
