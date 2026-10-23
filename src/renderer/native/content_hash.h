#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#if GOW3_NATIVE_USE_OPENSSL
#include <openssl/sha.h>
#else
#include <picosha2.h>
#endif

namespace gow3::renderer {

// The identity is always SHA-256, regardless of the build's optional backend.
// OpenSSL dispatches to the host CPU's accelerated implementation where available.
// Caller-owned output avoids shared digest buffers when guest threads overlap.
inline std::string ContentHash(std::span<const uint8_t> bytes) {
    std::array<uint8_t, 32> digest{};
#if GOW3_NATIVE_USE_OPENSSL
    static constexpr uint8_t empty = 0;
    if (!SHA256(bytes.empty() ? &empty : bytes.data(), bytes.size(), digest.data()))
        throw std::runtime_error("Native content SHA-256 failed");
#else
    picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    return result;
}

} // namespace gow3::renderer
