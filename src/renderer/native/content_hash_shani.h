#pragma once
// SHA-256 with the x86 SHA extensions, for builds without OpenSSL (Windows).
// Picosha2 hashed live payloads at a fraction of this speed and was a top cost
// of the Windows planner thread. Same digest as any SHA-256; callers fall back
// to Picosha2 when the CPU lacks the extensions.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
#define GOW3_CONTENT_HASH_SHANI 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define GOW3_CONTENT_HASH_SHANI 0
#endif

namespace gow3::renderer::shani {
#if GOW3_CONTENT_HASH_SHANI
inline bool Supported() noexcept {
  static const bool supported = [] {
    unsigned a = 0, b = 0, c = 0, d = 0;
#if defined(_MSC_VER)
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    c = unsigned(info[2]);
    const bool ssse3 = c & (1u << 9), sse41 = c & (1u << 19);
    __cpuidex(info, 7, 0);
    b = unsigned(info[1]);
#else
    if (__get_cpuid_max(0, nullptr) < 7) return false;
    __cpuid(1, a, b, c, d);
    const bool ssse3 = c & (1u << 9), sse41 = c & (1u << 19);
    __cpuid_count(7, 0, a, b, c, d);
#endif
    return ssse3 && sse41 && (b & (1u << 29));
  }();
  return supported;
}

__attribute__((target("sha,sse4.1,ssse3"))) inline void Blocks(uint32_t state[8], const uint8_t* data,
                                                               size_t blocks) noexcept {
  alignas(16) static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state)), 0xB1);
  __m128i state1 = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4)), 0x1B);
  __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);  // ABEF
  state1 = _mm_blend_epi16(state1, tmp, 0xF0);       // CDGH
  for (; blocks; --blocks, data += 64) {
    const __m128i abef = state0, cdgh = state1;
    __m128i w[4];
    for (int i = 0; i < 4; ++i)
      w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
    for (int g = 0; g < 16; ++g) {
      // Groups 4..15 extend the schedule in place: W[t-16] + s0(W[t-15]) +
      // W[t-7] + s1(W[t-2]) for the next four words.
      if (g >= 4)
        w[g & 3] = _mm_sha256msg2_epu32(
            _mm_add_epi32(_mm_sha256msg1_epu32(w[g & 3], w[(g + 1) & 3]),
                          _mm_alignr_epi8(w[(g + 3) & 3], w[(g + 2) & 3], 4)),
            w[(g + 3) & 3]);
      __m128i message = _mm_add_epi32(w[g & 3], _mm_load_si128(reinterpret_cast<const __m128i*>(k + 4 * g)));
      state1 = _mm_sha256rnds2_epu32(state1, state0, message);
      state0 = _mm_sha256rnds2_epu32(state0, state1, _mm_shuffle_epi32(message, 0x0E));
    }
    state0 = _mm_add_epi32(state0, abef);
    state1 = _mm_add_epi32(state1, cdgh);
  }
  tmp = _mm_shuffle_epi32(state0, 0x1B);     // FEBA
  state1 = _mm_shuffle_epi32(state1, 0xB1);  // DCHG
  _mm_storeu_si128(reinterpret_cast<__m128i*>(state), _mm_blend_epi16(tmp, state1, 0xF0));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), _mm_alignr_epi8(state1, tmp, 8));
}

inline std::array<uint8_t, 32> Digest(std::span<const uint8_t> bytes) noexcept {
  uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const size_t full = bytes.size() / 64;
  if (full) Blocks(state, bytes.data(), full);
  uint8_t tail[128] = {};
  const size_t rest = bytes.size() - full * 64;
  if (rest) std::memcpy(tail, bytes.data() + full * 64, rest);
  tail[rest] = 0x80;
  const size_t tail_blocks = rest < 56 ? 1 : 2;
  const uint64_t bits = uint64_t(bytes.size()) * 8;
  for (int i = 0; i < 8; ++i) tail[tail_blocks * 64 - 1 - i] = uint8_t(bits >> (8 * i));
  Blocks(state, tail, tail_blocks);
  std::array<uint8_t, 32> digest{};
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) digest[4 * i + j] = uint8_t(state[i] >> (24 - 8 * j));
  return digest;
}
#else
inline bool Supported() noexcept { return false; }
inline std::array<uint8_t, 32> Digest(std::span<const uint8_t>) noexcept { return {}; }
#endif
}  // namespace gow3::renderer::shani
