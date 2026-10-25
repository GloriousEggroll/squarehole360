// Xenos addressing/mip layout follows the BSD-licensed Xenia/ReXGlue texture
// utilities. Copyright 2022 Ben Vanik. See texture_data.LICENSE.
#include "texture_data.h"
#include "footprint_ranges.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace gow3::renderer {
namespace {
uint32_t Align(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
uint32_t CeilLog(uint32_t value) { return std::bit_width(value - 1); }

std::array<uint32_t, 3> PackedOffset(uint32_t w, uint32_t h, uint32_t d, uint32_t mip) {
  const uint32_t lw = CeilLog(w), lh = CeilLog(h), shorter = std::min(lw, lh);
  if (shorter > 4 + mip) return {};
  const uint32_t first = shorter > 4 ? shorter - 4 : 0, packed = mip - first;
  uint32_t x = 0, y = 0, z = 0;
  if (packed < 3) {
    if (lw > lh) y = 16 >> packed;
    else x = 16 >> packed;
  } else {
    const uint32_t offset = (1u << (std::max(lw, lh) - first)) >> (packed - 2);
    if (lw > lh) x = offset;
    else y = offset;
    if (offset < 4) {
      const uint32_t ld = CeilLog(d);
      z = ld > 1 + packed ? (ld - packed) * 4 : 4;
    }
  }
  return {x, y, z};
}

uint64_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log_bytes) {
  const uint64_t macro = (uint64_t(x >> 5) + uint64_t(y >> 5) * (Align(pitch, 32) >> 5)) << (log_bytes + 7);
  const uint64_t micro = ((x & 7) + ((y & 14) << 2)) << log_bytes;
  const uint64_t offset = macro + ((micro & ~15ull) << 1) + (micro & 15) + ((y & 1) << 4);
  return ((offset & ~511ull) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}

uint64_t Tiled3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height, uint32_t log_bytes) {
  pitch = Align(pitch, 32); height = Align(height, 32);
  const uint64_t outer = (uint64_t(y >> 4) + uint64_t(z >> 2) * (height >> 4)) * (pitch >> 5);
  const uint64_t macro = ((((x >> 5) + outer) << (log_bytes + 6)) & 0xFFFFFFF) << 1;
  const uint64_t micro = ((x & 7) + ((y & 6) << 2)) << log_bytes;
  const uint32_t o = ((y >> 3) + (z >> 2)) & 1;
  const uint32_t a = o + ((((x >> 3) + (o << 1)) & 3) << 1);
  const uint64_t b = ((macro + (micro & ~15ull)) << 1) + (micro & 15) +
                     ((z & 3) << (log_bytes + 6)) + ((y & 1) << 4);
  return (((((uint64_t(a & 1) << 3) + ((b >> 6) & 7)) << 3) + (a & ~1u)) << 5) +
         ((b & ~511ull) << 3) + (b & 63);
}

template <uint32_t Bytes, bool Swap>
void DecodeNarrowTextureBlocks(const TextureLayout& t, const TextureLevel& level,
                               uint32_t columns, uint32_t rows,
                               std::span<const uint8_t> data, std::span<uint8_t> output) {
  static_assert(Bytes == 1 || Bytes == 2);
  constexpr uint32_t swaps[]{0, 1, 3, 2};
  const uint32_t mask = swaps[t.endian];
  size_t target = 0;
  for (uint32_t z = 0; z < level.depth; ++z)
    for (uint32_t y = 0; y < rows; ++y)
      for (uint32_t x = 0; x < columns; ++x, target += Bytes) {
        const uint64_t offset = TextureBlockOffset(t, level, x, y, z);
        if constexpr (Bytes == 1) {
          const uint64_t source = offset ^ mask;
          if (source >= data.size())
            throw std::runtime_error("Texture payload is missing or truncated at mip " + std::to_string(level.mip));
          output[target] = data[source];
        } else {
          // DescribeTexture's level offsets are page aligned. Linear and both
          // tiled address functions preserve bit 0 == 0 for two-byte blocks.
          // XOR bit 1 selects the halfword; XOR bit 0 only swaps its two bytes.
          const uint64_t source = offset ^ (mask & 2);
          if (source > data.size() || sizeof(uint16_t) > data.size() - source)
            throw std::runtime_error("Texture payload is missing or truncated at mip " + std::to_string(level.mip));
          uint16_t value;
          std::memcpy(&value, data.data() + source, sizeof(value));
          if constexpr (Swap) value = std::byteswap(value);
          // memcpy permits unaligned provider spans and either host byte order.
          std::memcpy(output.data() + target, &value, sizeof(value));
        }
      }
}

template <uint32_t Bytes>
void DecodeNarrowTextureBlocks(const TextureLayout& t, const TextureLevel& level,
                               uint32_t columns, uint32_t rows,
                               std::span<const uint8_t> data, std::span<uint8_t> output) {
  // Byte XOR needs no endian-specific copy of the address calculation. Only
  // halfword byte swapping changes the loop body, so specialize that choice.
  if constexpr (Bytes == 1) {
    return DecodeNarrowTextureBlocks<1, false>(t, level, columns, rows, data, output);
  } else {
    if (t.endian == 1 || t.endian == 2)
      return DecodeNarrowTextureBlocks<2, true>(t, level, columns, rows, data, output);
    return DecodeNarrowTextureBlocks<2, false>(t, level, columns, rows, data, output);
  }
}

// Keep the narrow format loops out of the wide decoder's register allocation.
// This is called once per mip/layer, never once per texel.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void DecodeNarrowTexture(const TextureLayout& t, const TextureLevel& level,
                         uint32_t columns, uint32_t rows,
                         std::span<const uint8_t> data, std::span<uint8_t> output) {
  if (t.block.bytes == 1) DecodeNarrowTextureBlocks<1>(t, level, columns, rows, data, output);
  else DecodeNarrowTextureBlocks<2>(t, level, columns, rows, data, output);
}

template <uint32_t Bytes, uint32_t Endian>
void DecodeTextureBlocks(const TextureLayout& t, const TextureLevel& level,
                         uint32_t columns, uint32_t rows,
                         std::span<const uint8_t> data, std::span<uint8_t> output) {
  static_assert(Bytes == 4 || Bytes == 8 || Bytes == 16);
  size_t target = 0;
  for (uint32_t z = 0; z < level.depth; ++z)
    for (uint32_t y = 0; y < rows; ++y)
      for (uint32_t x = 0; x < columns; ++x, target += Bytes) {
        const uint64_t offset = TextureBlockOffset(t, level, x, y, z);
        // DescribeTexture's >=4-byte blocks start on a word boundary. Each
        // endian mode only permutes bytes within those words, so one whole
        // block bounds check covers exactly the original byte accesses.
        if (offset > data.size() || Bytes > data.size() - offset)
          throw std::runtime_error("Texture payload is missing or truncated at mip " + std::to_string(level.mip));
        if constexpr (Endian == 0) {
          std::memcpy(output.data() + target, data.data() + offset, Bytes);
        } else {
          for (uint32_t word = 0; word < Bytes; word += 4) {
            uint32_t value;
            // memcpy also supports unaligned provider spans without aliasing
            // assumptions. These permutations work on either host byte order.
            std::memcpy(&value, data.data() + offset + word, sizeof(value));
            if constexpr (Endian == 1)
              value = ((value & 0x00ff00ffu) << 8) | ((value & 0xff00ff00u) >> 8);
            else if constexpr (Endian == 2) value = std::byteswap(value);
            else value = std::rotl(value, 16);
            std::memcpy(output.data() + target + word, &value, sizeof(value));
          }
        }
      }
}

template <uint32_t Bytes>
void DecodeTextureBlocks(const TextureLayout& t, const TextureLevel& level,
                         uint32_t columns, uint32_t rows,
                         std::span<const uint8_t> data, std::span<uint8_t> output) {
  switch (t.endian) {
    case 0: return DecodeTextureBlocks<Bytes, 0>(t, level, columns, rows, data, output);
    case 1: return DecodeTextureBlocks<Bytes, 1>(t, level, columns, rows, data, output);
    case 2: return DecodeTextureBlocks<Bytes, 2>(t, level, columns, rows, data, output);
    case 3: return DecodeTextureBlocks<Bytes, 3>(t, level, columns, rows, data, output);
  }
}
} // namespace

TextureFormatInfo TextureFormat(uint32_t format) {
  switch (format) {
    case 2: case 8: case 9: return {1, 1, 1, 1};
    case 6: case 7: case 14: case 50: case 54: return {1, 1, 4, 4};
    case 10: return {1, 1, 2, 2};
    case 18: case 51: return {4, 4, 8, 4};
    case 19: case 20: case 52: case 53: return {4, 4, 16, 4};
    case 22: case 23: return {1, 1, 4, 1};
    case 24: case 27: case 30: return {1, 1, 2, 1};
    case 25: case 28: case 31: return {1, 1, 4, 2};
    case 26: case 29: case 32: return {1, 1, 8, 4};
    case 36: return {1, 1, 4, 1};
    case 37: return {1, 1, 8, 2};
    case 38: return {1, 1, 16, 4};
    case 49: return {4, 4, 16, 2};
    case 59: return {4, 4, 8, 1};
    default: throw std::runtime_error("Unsupported native texture format " + std::to_string(format));
  }
}

float DecodeDepthTexel(uint32_t packed, bool floating) {
  const uint32_t depth = packed >> 8;
  if (!floating) return float(depth + (depth >> 23)) * (1.0f / 16777216.0f);
  const uint32_t exponent = depth >> 20, mantissa = depth & 0xfffff;
  // Unsigned 20-bit mantissa, 4-bit exponent with bias 15; all exponents
  // represent finite values, including 15. Subnormals extend down to 2^-34.
  return std::ldexp(float(exponent ? mantissa | 0x100000 : mantissa),
                    exponent ? int(exponent) - 35 : -34);
}

TextureLayout DescribeTexture(const std::array<uint32_t, 6>& f) {
  if ((f[0] & 3) != 2) throw std::runtime_error("Fetch constant is not a texture");
  TextureLayout t{};
  t.fetch = f; t.dimension = (f[5] >> 9) & 3; t.format = f[1] & 63;
  t.block = TextureFormat(t.format); t.endian = (f[1] >> 6) & 3; t.tiled = (f[0] >> 31) != 0;
  const bool stacked = (f[1] & 1024) != 0, packed = (f[5] & 2048) != 0;
  t.width = t.dimension == 0 ? (f[2] & 0xFFFFFF) + 1 : t.dimension == 2 ? (f[2] & 2047) + 1 : (f[2] & 8191) + 1;
  t.height = t.dimension == 0 ? 1 : t.dimension == 2 ? ((f[2] >> 11) & 2047) + 1 : ((f[2] >> 13) & 8191) + 1;
  t.depth = t.dimension == 2 ? (f[2] >> 22) + 1 : 1;
  t.layers = t.dimension == 3 ? 6 : stacked ? (f[2] >> 26) + 1 : 1;
  if ((t.dimension == 0 && (stacked || packed || t.tiled)) ||
      (t.dimension != 1 && stacked) || (t.dimension == 3 && (f[2] >> 26) != 5) ||
      t.width > 8192 || t.height > 8192 || uint64_t(t.width) * t.height * t.depth * t.layers > 128 * 1024 * 1024)
    throw std::runtime_error("Unsupported or oversized texture dimensions");
  t.base_address = f[1] & 0x1FFFF000; t.mip_address = f[5] & 0x1FFFF000;
  const uint32_t size_max = std::bit_width(std::max({t.width, t.height, t.depth})) - 1;
  t.min_mip = t.mip_address ? std::min((f[4] >> 2) & 15, size_max) : 0;
  t.max_mip = t.mip_address ? std::max(t.min_mip, std::min((f[4] >> 6) & 15, size_max)) : 0;
  if (t.max_mip) {
    if (!t.base_address) t.min_mip = std::max(t.min_mip, 1u);
    if (t.min_mip) t.base_address = 0;
  } else t.mip_address = 0;
  if (!t.base_address && !t.mip_address) throw std::runtime_error("Texture has no resident base or mip region");
  const uint32_t packed_level = packed ? std::max(CeilLog(std::min(t.width, t.height)), 4u) - 4 : UINT32_MAX;
  uint64_t next_offset = 0;
  struct Storage { uint64_t offset, layer_stride; uint32_t pitch, rows; };
  std::array<Storage, 14> storage{};
  auto describe = [&](uint32_t level, bool base) {
    const uint32_t pitch_texels = base ? ((f[0] >> 22) & 511) * 32 : std::max(1u, std::bit_ceil(t.width) >> level);
    const uint32_t height = base ? t.height : std::max(1u, std::bit_ceil(t.height) >> level);
    uint32_t pitch = Align((pitch_texels + t.block.block_width - 1) / t.block.block_width, 32);
    if (!t.tiled && !base) pitch = Align(pitch * t.block.bytes, 256) / t.block.bytes;
    if (!pitch) throw std::runtime_error("Texture pitch is zero");
    const uint32_t rows = t.dimension == 0 ? 1 : Align((height + t.block.block_height - 1) / t.block.block_height, 32);
    const uint64_t raw_stride = uint64_t(pitch) * rows * t.block.bytes * (t.dimension == 2 ? Align(t.depth, 4) : 1);
    const uint64_t stride = (raw_stride + 4095) & ~4095ull;
    if (stride > 512 * 1024 * 1024) throw std::runtime_error("Texture stride exceeds safety limit");
    return Storage{base ? 0 : next_offset, stride, pitch, rows};
  };
  Storage base{};
  if (t.base_address) base = describe(0, true);
  for (uint32_t level = std::min(1u, packed_level); t.max_mip && level <= std::min(t.max_mip, packed_level); ++level) {
    storage[level] = describe(level, false);
    next_offset += storage[level].layer_stride * t.layers;
  }
  for (uint32_t mip = t.base_address ? 0 : std::max(1u, t.min_mip); mip <= t.max_mip; ++mip) {
    const auto s = mip ? storage[std::min(mip, packed_level)] : base;
    const auto offset = packed ? PackedOffset(t.width, t.height, t.depth, mip) : std::array<uint32_t, 3>{};
    for (uint32_t layer = 0; layer < t.layers; ++layer)
      t.levels.push_back({mip, layer, std::max(1u, t.width >> mip), std::max(1u, t.height >> mip),
          std::max(1u, t.depth >> mip), s.pitch, s.rows, offset[0] / t.block.block_width,
          offset[1] / t.block.block_height, t.dimension == 2 ? offset[2] : 0,
          s.offset + layer * s.layer_stride, mip != 0});
  }
  return t;
}

uint64_t TextureBlockOffset(const TextureLayout& t, const TextureLevel& level, uint32_t x, uint32_t y, uint32_t z) {
  x += level.x; y += level.y; z += level.z;
  const uint32_t log_bytes = std::countr_zero(t.block.bytes);
  return level.offset + (t.tiled ? t.dimension == 2
      ? Tiled3D(x, y, z, level.pitch_blocks, level.slice_rows, log_bytes)
      : Tiled2D(x, y, level.pitch_blocks, log_bytes)
      : (uint64_t(z) * level.slice_rows * level.pitch_blocks + uint64_t(y) * level.pitch_blocks + x) * t.block.bytes);
}

TextureData DecodeTexture(const std::array<uint32_t, 6>& fetch, std::span<const uint8_t> base, std::span<const uint8_t> mips) {
  TextureData result{DescribeTexture(fetch), {}};
  uint64_t total = 0;
  for (const auto& level : result.layout.levels) {
    const auto& t = result.layout;
    const uint32_t columns = (level.width + t.block.block_width - 1) / t.block.block_width;
    const uint32_t rows = (level.height + t.block.block_height - 1) / t.block.block_height;
    const uint64_t count = uint64_t(columns) * rows * level.depth * t.block.bytes;
    total += count;
    if (total > 256 * 1024 * 1024) throw std::runtime_error("Decoded texture exceeds safety limit");
    TextureData::Level output{level, std::vector<uint8_t>(count)};
    const auto data = level.mips ? mips : base;
    // Dispatch once per mip/layer, not per texel or byte.
    if (t.block.bytes == 4) DecodeTextureBlocks<4>(t, level, columns, rows, data, output.bytes);
    else if (t.block.bytes == 8) DecodeTextureBlocks<8>(t, level, columns, rows, data, output.bytes);
    else if (t.block.bytes == 16) DecodeTextureBlocks<16>(t, level, columns, rows, data, output.bytes);
    else DecodeNarrowTexture(t, level, columns, rows, data, output.bytes);
    result.levels.push_back(std::move(output));
  }
  return result;
}
std::vector<std::pair<uint32_t,uint32_t>> TextureMemoryRanges(const TextureLayout& t,bool mips,bool coalesce_blocks) {
  std::vector<std::pair<uint32_t,uint32_t>> ranges;
  constexpr uint32_t swaps[]{0,1,3,2};
  const uint32_t address=mips?t.mip_address:t.base_address;
  const auto append=[&](uint64_t first,uint64_t last) {
    if(last>512u*1024*1024)throw std::runtime_error("Texture block exceeds physical memory");
    if(!ranges.empty()&&first<=last&&ranges.back().first<=ranges.back().second&&
        first<=ranges.back().second&&last>=ranges.back().first) {
      ranges.back().first=std::min(ranges.back().first,uint32_t(first));
      ranges.back().second=std::max(ranges.back().second,uint32_t(last));
    } else ranges.emplace_back(uint32_t(first),uint32_t(last));
  };
  for(const auto& level:t.levels) {
    if(level.mips!=mips)continue;
    const uint32_t columns=(level.width+t.block.block_width-1)/t.block.block_width;
    const uint32_t rows=(level.height+t.block.block_height-1)/t.block.block_height;
    // Whole >=4-byte blocks are invariant under all four endian permutations.
    // Keep arbitrary/malformed public layouts on the original arithmetic path,
    // including unsigned offset wrap behavior. No padding is added to coverage.
    const bool bounded=coalesce_blocks&&(t.block.bytes==4||t.block.bytes==8||t.block.bytes==16)&&
        t.endian<4&&columns&&rows&&columns<=8192&&rows<=8192&&
        uint64_t(level.x)+columns<=8192&&uint64_t(level.y)+rows<=8192&&
        uint64_t(level.z)+level.depth<=2048&&level.pitch_blocks&&level.pitch_blocks<=8192&&
        level.slice_rows<=8192&&level.offset<=512u*1024*1024&&address<=512u*1024*1024;
    if(bounded&&!t.tiled) {
      // A linear row is one exact interval even with pitch gaps or overlapping
      // rows. Sorting/union below retains the original alias handling.
      for(uint32_t z=0;z<level.depth;++z)for(uint32_t y=0;y<rows;++y) {
        const uint64_t first=uint64_t(address)+TextureBlockOffset(t,level,0,y,z);
        append(first,first+uint64_t(columns)*t.block.bytes);
      }
      continue;
    }
    if(bounded&&t.tiled&&t.dimension!=2&&
       ((level.x|level.y|columns|rows)&31)==0) {
      // For log2(block bytes)>=2, Tiled2D maps the 1024 blocks of an
      // aligned32x32 tile bijectively onto its contiguous1024*bytes interval.
      // The macro tile index occupies only bits above that interval. Narrow
      // formats, partial tiles, packed tails and 3D keep the per-block path.
      for(uint32_t z=0;z<level.depth;++z)for(uint32_t y=0;y<rows;y+=32)
        for(uint32_t x=0;x<columns;x+=32) {
          const uint64_t first=uint64_t(address)+TextureBlockOffset(t,level,x,y,z);
          append(first,first+uint64_t(1024)*t.block.bytes);
        }
      continue;
    }
    for(uint32_t z=0;z<level.depth;++z)for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<columns;++x) {
      uint64_t offset=TextureBlockOffset(t,level,x,y,z);
      // Endian swaps permute bytes inside >=4-byte blocks. Smaller texels
      // can move across an adjacent texel's address within the same word.
      if(t.block.bytes<4)offset^=swaps[t.endian]&~(t.block.bytes-1);
      const uint64_t first=uint64_t(address)+offset,last=first+t.block.bytes;
      // Enumeration is not globally ordered for tiled, endian-swapped or
      // packed mip storage. Merge only touching/overlapping consecutive runs,
      // including descending ones, and retain the exact sort/union below.
      // This avoids first allocating one interval per texel or BC block.
      append(first,last);
    }
  }
  std::sort(ranges.begin(),ranges.end());
  size_t count=0;
  for(const auto& r:ranges) {
    if(count&&r.first<=ranges[count-1].second)ranges[count-1].second=std::max(ranges[count-1].second,r.second);
    else ranges[count++]=r;
  }
  ranges.resize(count);
  for(auto& [first,last]:ranges)last-=first;
  // These vectors are retained by footprint caches. Do not retain the much
  // larger temporary capacity used before the exact sorted union collapsed it.
  if(ranges.capacity()>ranges.size())return {ranges.begin(),ranges.end()};
  return ranges;
}

void AppendTextureLevelFootprint(TextureFootprintRanges& ranges, const TextureLayout& layout,
    const TextureLevel& level, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
    uint64_t address, uint64_t limit, bool reference) {
  const uint32_t bytes = layout.block.bytes;
  const unsigned endian_xor[] = {0, 1, 3, 2};
  const auto append_row = [&](uint32_t first_x, uint32_t last_x, uint32_t y) {
    for (uint32_t x = first_x; x < last_x; ++x) {
      uint64_t offset = address + TextureBlockOffset(layout, level, x, y, 0);
      if (bytes < 4) offset ^= endian_xor[layout.endian] & ~(bytes - 1u);
      if (offset + bytes > limit) throw std::runtime_error("Texture footprint exceeds its memory");
      AppendFootprintRange(ranges, uint32_t(offset), uint32_t(offset + bytes));
    }
  };
  // A tiled 2D level stores each aligned 32x32-block tile contiguously and a
  // linear level each block row, so those are one range; partial tiles at the
  // edges are enumerated block by block. Unstacked 2D levels at their own
  // origin (not inside a packed mip tail) with blocks of at least four bytes
  // (no endian sub-dword aliasing) and a 32-block-aligned pitch.
  // tests/texture_footprint_test.cpp checks against the enumeration.
  const bool plain = !reference && layout.dimension == 1 && bytes >= 4 && layout.layers == 1 &&
      !(layout.fetch[1] & 1024u) && !level.x && !level.y && !level.z && level.depth == 1 &&
      level.pitch_blocks && level.pitch_blocks <= 16352 && !(level.pitch_blocks & 31u);
  if (plain && !layout.tiled) {
    for (uint32_t y = top; y < bottom && left < right; ++y) {
      const uint64_t first = address + TextureBlockOffset(layout, level, left, y, 0);
      const uint64_t last = first + uint64_t(right - left) * bytes;
      if (last > limit) throw std::runtime_error("Texture footprint exceeds its memory");
      AppendFootprintRange(ranges, uint32_t(first), uint32_t(last));
    }
    return;
  }
  const uint32_t tile_left = (left + 31u) & ~31u, tile_right = right & ~31u;
  const uint32_t tile_top = (top + 31u) & ~31u, tile_bottom = bottom & ~31u;
  if (plain && tile_left < tile_right && tile_top < tile_bottom) {
    const uint32_t tile_bytes = 1024u * bytes;
    for (uint32_t y = top; y < bottom; ++y) {
      if (y < tile_top || y >= tile_bottom) { append_row(left, right, y); continue; }
      append_row(left, tile_left, y);
      if (!(y & 31u))
        for (uint32_t x = tile_left; x < tile_right; x += 32) {
          const uint64_t first = address + TextureBlockOffset(layout, level, x, y, 0);
          if (first + tile_bytes > limit) throw std::runtime_error("Texture footprint exceeds its memory");
          AppendFootprintRange(ranges, uint32_t(first), uint32_t(first + tile_bytes));
        }
      append_row(tile_right, right, y);
    }
    return;
  }
  for (uint32_t y = top; y < bottom; ++y) append_row(left, right, y);
}

std::vector<std::pair<uint32_t,uint32_t>> TextureBaseFootprint(std::array<uint32_t, 6> fetch,
    std::optional<std::array<int32_t, 4>> rectangle, bool reference) {
  fetch[4] &= ~((15u << 2) | (15u << 6));
  const auto layout = DescribeTexture(fetch);
  const auto r = rectangle.value_or(std::array<int32_t, 4>{0, 0, int32_t(layout.width), int32_t(layout.height)});
  if (r[0] < 0 || r[1] < 0 || r[2] <= r[0] || r[3] <= r[1] || uint32_t(r[2]) > layout.width || uint32_t(r[3]) > layout.height)
    throw std::runtime_error("Invalid live resolve rectangle");
  FootprintRanges ranges;
  const auto bw = layout.block.block_width, bh = layout.block.block_height;
  for (const auto& level : layout.levels) {
    if (level.mip || level.mips) continue;
    AppendTextureLevelFootprint(ranges, layout, level, uint32_t(r[0]) / bw, uint32_t(r[1]) / bh,
        (uint32_t(r[2]) + bw - 1) / bw, (uint32_t(r[3]) + bh - 1) / bh, layout.base_address,
        512u * 1024 * 1024, reference);
  }
  std::sort(ranges.begin(), ranges.end());
  FootprintRanges merged;
  for (auto [first, last] : ranges) {
    if (!merged.empty() && first <= merged.back().second) merged.back().second = std::max(last, merged.back().second);
    else merged.emplace_back(first, last);
  }
  return merged;
}

} // namespace gow3::renderer
