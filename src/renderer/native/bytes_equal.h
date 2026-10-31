#pragma once
// Byte equality for exact snapshot comparisons. glibc and libSystem memcmp are
// already vectorized; the Microsoft C runtime's memcmp compares 8 bytes per
// step, which made snapshot comparisons the busiest code on the game thread
// in the Windows build. Only equality is needed, never ordering.
#include <cstddef>
#include <cstdint>
#include <cstring>
#if !defined(GOW3_BYTES_EQUAL_SSE2) && defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define GOW3_BYTES_EQUAL_SSE2 1
#endif
#if GOW3_BYTES_EQUAL_SSE2
#include <emmintrin.h>
#endif

namespace gow3::renderer {
inline bool BytesEqual(const void* a, const void* b, size_t size) noexcept {
#if GOW3_BYTES_EQUAL_SSE2
  auto* x = static_cast<const uint8_t*>(a);
  auto* y = static_cast<const uint8_t*>(b);
  for (; size >= 64; x += 64, y += 64, size -= 64) {
    // SSE2 only: every x86-64 CPU has it, whatever flags a target uses.
    const __m128i e0 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x)),
                                      _mm_loadu_si128(reinterpret_cast<const __m128i*>(y)));
    const __m128i e1 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x + 16)),
                                      _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + 16)));
    const __m128i e2 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x + 32)),
                                      _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + 32)));
    const __m128i e3 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x + 48)),
                                      _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + 48)));
    const __m128i all = _mm_and_si128(_mm_and_si128(e0, e1), _mm_and_si128(e2, e3));
    if (_mm_movemask_epi8(all) != 0xFFFF) return false;
  }
  return !size || !std::memcmp(x, y, size);
#else
  return !std::memcmp(a, b, size);
#endif
}
}  // namespace gow3::renderer
