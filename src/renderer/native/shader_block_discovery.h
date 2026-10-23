#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace gow3::renderer {

inline constexpr size_t kMaxShaderDiscoveryBlockBytes = 1024 * 1024;
inline constexpr size_t kMaxShaderDiscoveryBlockCandidates = 128;
inline constexpr size_t kMaxShaderDiscoverySectionBytes = 128 * 1024;

struct ShaderBlockDiscoveryStatistics {
  // Prefix visited by the byte scanner, including the four-byte magic probe.
  uint64_t inspected = 0;
  uint64_t candidates = 0;
  uint64_t emitted = 0;
  uint64_t emitted_bytes = 0;
  uint64_t refused = 0;
  bool limit_reached = false;
};

namespace shader_block_discovery_detail {
inline bool Range(size_t offset, size_t bytes, size_t limit) {
  return !(offset & 3) && offset <= limit && bytes <= limit - offset;
}

// Callers first prove that all four bytes are present. No guest pointer or
// alignment assumptions are made: serialized containers can be unaligned.
inline uint32_t Word(std::span<const uint8_t> bytes, size_t at) {
  return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
         uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
}

// A narrow speculative input filter, not a substitute for the native shader
// compiler's validation. Returns the exact contiguous virtual+physical size.
// Pixel RHI construction copies these two sections without rewriting them;
// vertex linking does rewrite its program and is deliberately not covered.
inline size_t PixelContainerBytes(std::span<const uint8_t> bytes) {
  if (bytes.size() < 36 || Word(bytes, 0) != 0x102a1100u ||
      Word(bytes, 12) || Word(bytes, 16) || Word(bytes, 28) || Word(bytes, 32)) {
    return 0;
  }
  const size_t virtual_size = Word(bytes, 4), physical_size = Word(bytes, 8);
  if (virtual_size < 36 || virtual_size > kMaxShaderDiscoverySectionBytes ||
      !physical_size || physical_size > kMaxShaderDiscoverySectionBytes ||
      !Range(virtual_size, physical_size, bytes.size())) {
    return 0;
  }
  const size_t shader = Word(bytes, 24);
  if (shader < 36 || !Range(shader, 32, virtual_size)) return 0;
  const size_t code_offset = Word(bytes, shader), code_size = Word(bytes, shader + 4);
  if (!code_size || code_size > 4096 * 12 || code_size % 12 ||
      !Range(code_offset, code_size, physical_size)) {
    return 0;
  }
  const uint32_t interpolators = (Word(bytes, shader + 20) >> 5) & 31;
  const size_t declarations = shader + 32;
  if (!Range(declarations, size_t(interpolators) * 4, virtual_size)) return 0;
  for (uint32_t i = 0; i < interpolators; ++i) {
    if (((Word(bytes, declarations + size_t(i) * 4) >> 4) & 15) >= 14) return 0;
  }

  // Preserve the compiler's bounded float/loop definition validation. In
  // particular both terminators must be inside the virtual section, even when
  // there are no definitions of one kind.
  if (const size_t definitions = Word(bytes, 20)) {
    if (definitions < 36 || !Range(definitions, 24, virtual_size)) return 0;
    size_t cursor = definitions + 20;
    for (;;) {
      if (!Range(cursor, 4, virtual_size)) return 0;
      const uint32_t definition = Word(bytes, cursor);
      if (!definition) break;
      if (!Range(cursor, 8, virtual_size)) return 0;
      const uint32_t first = definition >> 16, count = ((definition & 65535) + 3) / 4;
      if (first < 256 || first - 256 + count > 256 ||
          !Range(Word(bytes, cursor + 4), size_t(count) * 16, physical_size)) {
        return 0;
      }
      cursor += 8;
    }
    cursor += 4;
    for (;;) {
      if (!Range(cursor, 4, virtual_size)) return 0;
      const uint32_t definition = Word(bytes, cursor);
      if (!definition) break;
      const uint32_t first = definition >> 16, count = definition & 65535;
      if (first < 8992 || (first - 8992) % 4 || (first - 8992) / 4 + count > 32 ||
          !Range(cursor, 8 + size_t(count) * 4, virtual_size)) {
        return 0;
      }
      cursor += 8 + size_t(count) * 4;
    }
  }

  // The upstream decoder follows both packed control-flow words. Check their
  // clause ranges before a discovered source is offered to compilation.
  const size_t code = virtual_size + code_offset;
  size_t control_bytes = code_size;
  for (size_t cursor = 0; cursor < control_bytes; cursor += 12) {
    if (cursor > code_size || code_size - cursor < 12) return 0;
    const uint32_t a = Word(bytes, code + cursor);
    const uint32_t b = Word(bytes, code + cursor + 4);
    const uint32_t c = Word(bytes, code + cursor + 8);
    const uint64_t words[]{uint64_t(a) | uint64_t(b & 65535) << 32,
                           uint64_t(b >> 16) | uint64_t(c) << 16};
    for (const uint64_t cf : words) {
      const uint32_t opcode = (cf >> 44) & 15;
      if ((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14) {
        const size_t address = size_t(cf & 4095) * 12;
        if (!Range(address, size_t((cf >> 12) & 7) * 12, code_size)) return 0;
        if (address && address < control_bytes) control_bytes = address;
      }
    }
  }
  return virtual_size + physical_size;
}
}  // namespace shader_block_discovery_detail

// Scan a successfully decompressed, synchronously readable block. Only owned,
// byte-exact stripped pixel sources escape this function. No allocations occur
// before a candidate passes the bounded structural checks. The callback may
// enqueue speculative compilation; it must still use the normal compiler.
//
// The candidate cap includes rejected headers to bound adversarial validation
// work. Overlapping candidates are inspected, so an incidental outer header
// cannot hide a real inner one. An incomplete scan never means fully prewarmed.
// Allocation/callback exceptions propagate to the caller's speculative guard.
template <class Emit>
ShaderBlockDiscoveryStatistics DiscoverDecodedPixelShaders(
    std::span<const uint8_t> block, Emit&& emit) {
  ShaderBlockDiscoveryStatistics statistics;
  if (block.size() > kMaxShaderDiscoveryBlockBytes) {
    statistics.refused = 1;
    statistics.limit_reached = true;
    return statistics;
  }
  for (size_t at = 0; at < block.size(); ++at) {
    if (block.size() - at < 4) break;
    statistics.inspected = at + 4;
    if (block[at] != 0x10 || block[at + 1] != 0x2a ||
        block[at + 2] != 0x11 || block[at + 3] != 0) continue;
    if (statistics.candidates == kMaxShaderDiscoveryBlockCandidates) {
      statistics.limit_reached = true;
      return statistics;
    }
    ++statistics.candidates;
    const auto remaining = block.subspan(at);
    const size_t size = shader_block_discovery_detail::PixelContainerBytes(remaining);
    if (!size) {
      ++statistics.refused;
      continue;
    }
    if (size > kMaxShaderDiscoveryBlockBytes - statistics.emitted_bytes) {
      statistics.limit_reached = true;
      return statistics;
    }
    std::shared_ptr<const std::vector<uint8_t>> source =
        std::make_shared<const std::vector<uint8_t>>(remaining.begin(), remaining.begin() + size);
    emit(std::move(source));
    ++statistics.emitted;
    statistics.emitted_bytes += size;
  }
  statistics.inspected = block.size();
  return statistics;
}

}  // namespace gow3::renderer
