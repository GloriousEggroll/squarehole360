#pragma once
#include "plume_render_interface.h"
#include <array>
#include <cstdint>
#include <span>

namespace gow3::renderer {
// Device register window 0x2880..0x2a5f, in host word order. This is the
// supported Gears executable's D3D state ABI, not a GPU command packet parser.
inline constexpr uint32_t kDrawStateOffset = 0x2880;
inline constexpr uint32_t kDrawStateWords = (0x2a60 - kDrawStateOffset) / 4;
struct DrawState {
    plume::RenderViewport viewport;
    plume::RenderRect scissor;
    plume::RenderGraphicsPipelineDesc pipeline;
    uint32_t specialization = 0;
    float alpha_reference = 0;
    std::array<float, 4> blend_constant{};
    float depth_bias = 0, slope_depth_bias = 0;
    std::array<float, 2> half_pixel{};
    int32_t index_offset = 0;
    bool discard = false;
};
// Rejects states not yet represented by this native pipeline. Callers must
// report these explicitly, rather than silently producing a wrong scene.
DrawState DecodeDrawState(std::span<const uint32_t, kDrawStateWords> words,
                          uint32_t width, uint32_t height, uint32_t primitive,
                          bool programmable_blending = false);
}
