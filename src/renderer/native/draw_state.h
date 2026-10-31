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
// A draw's viewport registers (0x2908..0x291c), non-finite values included.
struct DrawViewport {
    float x_scale = 0, x_offset = 0, y_scale = 0, y_offset = 0, z_scale = 0, z_offset = 0;
    // A finite D3D viewport, top-left origin, depth range within [0, 1]:
    // the only orientation the native pipeline represents.
    bool Representable() const noexcept;
};
DrawViewport DecodeDrawViewport(std::span<const uint32_t, kDrawStateWords> words) noexcept;
// Rejects states not yet represented by this native pipeline. Callers must
// report these explicitly, rather than silently producing a wrong scene.
DrawState DecodeDrawState(std::span<const uint32_t, kDrawStateWords> words,
                          uint32_t width, uint32_t height, uint32_t primitive,
                          bool programmable_blending = false);
}
