#pragma once
#include "shader_byte_segments.h"
#include <xxhash.h>

namespace gow3::renderer {
// A lookup hint only. Every consumer must compare all bytes before accepting
// a hit, even when a caller supplies a colliding or different fingerprint.
struct RuntimeShaderInputFingerprint {
    bool fast=false;
    size_t operator()(const ShaderByteSegments& bytes) const {
        if(!fast)return bytes.Fingerprint();
        return bytes.FingerprintWith([](std::span<const uint8_t> segment) {
            return size_t(XXH3_64bits(segment.data(),segment.size()));
        });
    }
};
}
