#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace gow3::renderer {
// Byte layout consumed by the Gears XenosRecomp adapter. Addresses refer to
// native upload buffers, never guest pointers. Keep buffers alive until the
// submission fence completes. Raw vertex bytes retain their guest endianness.
struct VertexStream {
  uint64_t address = 0;
  uint32_t byte_size = 0;
  uint32_t endian = 0;
};
struct alignas(16) SharedShaderConstants {
  std::array<uint32_t, 32> textures_2d{}, textures_3d{}, textures_cube{}, samplers{};
  std::array<uint32_t, 8> booleans{};
  uint32_t reserved_texcoord_swaps = 0;
  std::array<float, 2> half_pixel{};
  float alpha_threshold = 0;
  std::array<float, 4> color_scale{1,1,1,1};
  std::array<uint32_t, 32> loops{};
  std::array<VertexStream, 96> vertex_streams{};
  // Per-binding post-sample signedness (after view swizzle) and power-of-two
  // exponent adjustment. Signed normalization itself uses native SNORM views.
  struct TextureProperties { uint32_t signs = 0; int32_t exponent = 0; };
  std::array<TextureProperties, 32> texture_properties{};
};
struct ShaderPushConstants {
  uint64_t vertex_constants, pixel_constants, shared_constants;
};
static_assert(sizeof(VertexStream) == 16);
static_assert(offsetof(SharedShaderConstants, booleans) == 512);
static_assert(offsetof(SharedShaderConstants, half_pixel) == 548);
static_assert(offsetof(SharedShaderConstants, alpha_threshold) == 556);
static_assert(offsetof(SharedShaderConstants, color_scale) == 560);
static_assert(offsetof(SharedShaderConstants, loops) == 576);
static_assert(offsetof(SharedShaderConstants, vertex_streams) == 704);
static_assert(offsetof(SharedShaderConstants, texture_properties) == 2240);
static_assert(sizeof(SharedShaderConstants) == 2496);
static_assert(sizeof(ShaderPushConstants) == 24);
}  // namespace gow3::renderer
