#pragma once

#include "renderer/native/native_early_depth_shader.h"
#include <array>

namespace gow3::renderer {
inline constexpr bool kNativeEarlyDepthCatalogDefaultEnabled = false;
inline constexpr std::string_view kNativeEarlyDepthCatalogVariantIdentity =
    "native-fsi-canonical-two-material-early-fragment-tests-catalog-v2";
inline constexpr std::string_view kNativeEarlyDepthCatalogVertexSource =
    "75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802";
inline constexpr std::array<std::string_view, 2> kNativeEarlyDepthCatalogPixelSources{
    "a3c9ae7e8cebe9024d1e53447bf8883ed57a9013fda97ef08a7d9f7fc04a145d",
    "55c79ab30e62247886a8c74dc440f04695ffd0f6f88842ffa1aea0b6829ee6c6"};

struct NativeEarlyDepthCatalogPin {
    std::string_view vertex_source, pixel_source;
    std::string_view vertex_spirv_sha256, pixel_spirv_sha256;
    uint32_t vertex_bytes, pixel_bytes;
    uint32_t pixel_edram_stores, pixel_implicit_lod_samples, pixel_derivative_instructions;
};
std::span<const NativeEarlyDepthCatalogPin> NativeEarlyDepthCatalogPins();
bool NativeEarlyDepthCatalogPair(std::string_view vertex_source, std::string_view pixel_source);

// Exact canonical byte certificates for the closed two-pair catalogue only.
// Valid original Vulkan SPIR-V is a prerequisite; this is not a general validator.
// The returned program has only one additional three-word EarlyFragmentTests
// execution mode. Original arithmetic, derivatives, discard, interlock cleanup,
// barriers, depth-offset and packed EDRAM epilogue words remain unchanged.
// Shader certificates do not establish host query/state/numeric/alias ownership:
// selection must exclude active queries and keep the audited monotonic GE,
// sample1, no stencil/bias, defined raster and exact geometry/state contract.
std::shared_ptr<const NativeEarlyDepthProgram> TryPrepareNativeEarlyDepthCatalogShader(
    std::span<const uint32_t> canonical, std::string_view source,
    std::string_view canonical_cache_version);
bool NativeEarlyDepthCatalogVertexCertified(std::span<const uint32_t> canonical,
    std::string_view source, std::string_view canonical_cache_version);
std::string NativeEarlyDepthCatalogVersion();
} // namespace gow3::renderer
