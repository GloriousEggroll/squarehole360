#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer {

inline constexpr std::string_view kNativeEarlyDepthPixelSource =
    "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced";
inline constexpr std::string_view kNativeEarlyDepthVertexSource =
    "f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
inline constexpr std::string_view kNativeEarlyDepthCanonicalVersion =
    "1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline constexpr std::string_view kNativeEarlyDepthPixelSpirvHash =
    "8dbb0ddbef881cd8bdeab0a61d4b80493095799d2ef62be10411bec0269f3a54";
inline constexpr std::string_view kNativeEarlyDepthVertexSpirvHash =
    "1e5be1eb3e3a59d19f33d96dccd4539ff1cc12d9027fe671188964e3f76ab999";
inline constexpr std::string_view kNativeEarlyDepthVariantIdentity =
    "native-fsi-canonical-49f2-early-fragment-tests-v1";

struct NativeEarlyDepthProgram {
    std::string source_sha256;
    std::string canonical_spirv_sha256;
    std::string program_sha256;
    std::vector<uint32_t> spirv;
    size_t added_execution_mode_word = 0;
    uint32_t entry_point = 0;
    uint32_t canonical_edram_stores = 0;
    uint32_t canonical_implicit_lod_samples = 0;
    uint32_t canonical_derivative_instructions = 0;
};

// Valid native compiler output is a prerequisite. These are exact byte
// certificates for one reviewed original VS/PS pair, not a general SPIR-V
// validator. The PS additionally checks pointer ancestry and external effects.
// Adding the three-word execution mode preserves every original word, including
// DXC's extra interlock cleanup instructions. Original shader arithmetic, kill,
// derivatives, barriers and packed EDRAM epilogue remain authoritative.
// Unknown inputs return null/false. Allocation/hash failures may throw before
// admission; callers must retain ordinary Native fallback.
std::shared_ptr<const NativeEarlyDepthProgram> TryPrepareNativeEarlyDepthShader(
    std::span<const uint32_t> canonical, std::string_view source,
    std::string_view canonical_cache_version);
bool NativeEarlyDepthVertexCertified(std::span<const uint32_t> canonical,
    std::string_view source, std::string_view canonical_cache_version);

// Separate producer/schema identity. Canonical compiler/cache identity is never
// changed or used as the derived Scene shader-module identity.
std::string NativeEarlyDepthShaderVersion();

} // namespace gow3::renderer
