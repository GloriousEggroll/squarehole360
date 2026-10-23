#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace gow3::renderer {
// D24S8 / D24FS8 resolved texture texel; stencil is not exposed by sampling.
float DecodeDepthTexel(uint32_t packed, bool floating);
struct TextureFormatInfo { uint32_t block_width, block_height, bytes, components; };
TextureFormatInfo TextureFormat(uint32_t format);

// The layout and byte conversion are independent of the graphics API. Inputs
// come from the user's loaded assets; these objects never own guest pointers.
struct TextureLevel {
  uint32_t mip, layer, width, height, depth;
  uint32_t pitch_blocks, slice_rows, x, y, z;
  uint64_t offset;
  bool mips;
};
struct TextureLayout {
  std::array<uint32_t, 6> fetch;
  uint32_t width, height, depth, layers, dimension, format, endian, min_mip, max_mip;
  uint32_t base_address, mip_address;
  bool tiled;
  TextureFormatInfo block;
  std::vector<TextureLevel> levels;
};
struct TextureData {
  TextureLayout layout;
  struct Level { TextureLevel layout; std::vector<uint8_t> bytes; };
  std::vector<Level> levels;
};
TextureLayout DescribeTexture(const std::array<uint32_t, 6>& fetch);
uint64_t TextureBlockOffset(const TextureLayout& texture, const TextureLevel& level,
                            uint32_t x, uint32_t y, uint32_t z);
// Physical address and byte count for actual sampled blocks, excluding
// allocation padding. Base and mip regions retain their distinct provenance.
std::vector<std::pair<uint32_t,uint32_t>> TextureMemoryRanges(const TextureLayout&,bool mips,
                                                          bool coalesce_blocks=false);
TextureData DecodeTexture(const std::array<uint32_t, 6>& fetch,
                          std::span<const uint8_t> base, std::span<const uint8_t> mips);
}  // namespace gow3::renderer
