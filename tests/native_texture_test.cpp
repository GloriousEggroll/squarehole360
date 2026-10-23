#include "renderer/native/texture_data.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <iostream>
#include <stdexcept>

int main() try {
  namespace native = gow3::renderer;
  namespace reference = rex::graphics::texture_util;
  namespace xenos = rex::graphics::xenos;
  size_t comparisons = 0, cases = 0;
  for (uint32_t format : {2u, 6u, 7u, 10u, 18u, 19u, 20u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 36u, 37u, 38u, 49u, 54u, 59u}) {
    for (auto size : {std::array{2u, 2u}, std::array{8u, 8u}, std::array{32u, 32u},
                      std::array{80u, 260u}, std::array{128u, 64u}}) {
      for (bool tiled : {false, true}) for (bool packed : {false, true})
      for (uint32_t dimension : {1u, 2u, 3u}) for (uint32_t endian : {0u, 1u, 2u, 3u}) {
        const uint32_t w = size[0], h = size[1], depth = dimension == 2 ? 8 : 1;
        const auto block = native::TextureFormat(format);
        const uint32_t pitch_texels = (std::max(w, 256u / block.bytes * block.block_width) + 31) & ~31u;
        std::array<uint32_t, 6> words{2u | ((pitch_texels / 32) << 22) | (uint32_t(tiled) << 31),
            0x1000u | (endian << 6) | format,
            dimension == 2 ? ((w - 1) | ((h - 1) << 11) | ((depth - 1) << 22))
                           : ((w - 1) | ((h - 1) << 13) | (dimension == 3 ? 5u << 26 : 0)),
            0, 15 << 6, 0x2000u | (dimension << 9) | (uint32_t(packed) << 11)};
        const auto actual = native::DescribeTexture(words);
        xenos::xe_gpu_texture_fetch_t fetch;
        std::memcpy(&fetch, words.data(), sizeof(fetch));
        uint32_t wm, hm, dm, base, mips, min, max;
        reference::GetSubresourcesFromFetchConstant(fetch, &wm, &hm, &dm, &base, &mips, &min, &max);
        const auto expected = reference::GetGuestTextureLayout(fetch.dimension, fetch.pitch, wm + 1, hm + 1, dm + 1,
            fetch.tiled, fetch.format, fetch.packed_mips, base != 0, max);
        for (const auto& level : actual.levels) {
          const auto stored = std::min(level.mip, expected.packed_level);
          const auto& reference_level = level.mip ? expected.mips[stored] : expected.base;
          uint32_t x = 0, y = 0, z = 0;
          if (packed) reference::GetPackedMipOffset(w, h, depth, fetch.format, level.mip, x, y, z);
          if (dimension != 2) z = 0;
          const uint64_t offset = (level.mip ? expected.mip_offsets_bytes[stored] : 0) +
                                   uint64_t(level.layer) * reference_level.array_slice_stride_bytes;
          if (level.pitch_blocks * block.bytes != reference_level.row_pitch_bytes ||
              level.slice_rows != reference_level.z_slice_stride_block_rows || level.offset != offset ||
              level.x != x || level.y != y || level.z != z)
            throw std::runtime_error("Layout mismatch format=" + std::to_string(format) + " mip=" + std::to_string(level.mip));
          const uint32_t bw = (level.width + block.block_width - 1) / block.block_width;
          const uint32_t bh = (level.height + block.block_height - 1) / block.block_height;
          for (uint32_t bz : {0u, level.depth - 1}) for (uint32_t by : {0u, bh / 2, bh - 1}) for (uint32_t bx : {0u, bw / 2, bw - 1}) {
            uint64_t address;
            const uint32_t pitch = reference_level.row_pitch_bytes / block.bytes;
            if (tiled) address = dimension == 2
                ? reference::GetTiledOffset3D(x + bx, y + by, z + bz, pitch,
                    reference_level.z_slice_stride_block_rows, std::countr_zero(block.bytes))
                : reference::GetTiledOffset2D(x + bx, y + by, pitch, std::countr_zero(block.bytes));
            else address = ((uint64_t(z + bz) * reference_level.z_slice_stride_block_rows + y + by) * pitch + x + bx) * block.bytes;
            address += offset;
            if (native::TextureBlockOffset(actual, level, bx, by, bz) != address)
              throw std::runtime_error("Tiled address mismatch");
            ++comparisons;
          }
        }
        ++cases;
      }
    }
  }
  // Literal 2x2 pixels, with a padded row and all four endian conventions.
  for (uint32_t endian = 0; endian < 4; ++endian) {
    std::array<uint32_t, 6> words{2 | (1 << 22), 0x1000 | (endian << 6) | 6,
        1 | (1 << 13), 0, 0, 1 << 9};
    std::vector<uint8_t> source(136);
    constexpr uint32_t swap[]{0, 1, 3, 2};
    for (uint32_t pixel = 0; pixel < 4; ++pixel) for (uint32_t c = 0; c < 4; ++c)
      source[((pixel / 2) * 128 + (pixel % 2) * 4 + c) ^ swap[endian]] = pixel * 4 + c;
    const auto output = native::DecodeTexture(words, source, {});
    for (uint32_t byte = 0; byte < 16; ++byte) {
      if (output.levels[0].bytes[byte] != byte) throw std::runtime_error("Byte-order conversion mismatch");
      ++comparisons;
    }
    source.resize(132);
    bool rejected = false;
    try { native::DecodeTexture(words, source, {}); } catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Truncated source was accepted");
  }
  for (uint32_t bits = 0; bits < (1u << 24); ++bits) {
    const uint32_t packed = bits << 8 | 0xa5;
    if (native::DecodeDepthTexel(packed, true) != xenos::Float20e4To32(bits))
      throw std::runtime_error("Float24 texture conversion mismatch");
    const float normalized = float(double(bits) / double(0xffffff));
    if (std::abs(native::DecodeDepthTexel(packed, false) - normalized) > 1.0f / 16777216.0f)
      throw std::runtime_error("UNORM24 texture conversion mismatch");
    comparisons += 2;
  }
  std::cout << "PASS: " << cases << " texture layouts, " << comparisons << " comparisons against reference and literal pixels\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
