#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace gow3::renderer {

// The compiler's persistent output remains unchanged. This identity belongs
// only to immutable Scene shader modules created from that output.
enum class NativeShaderVariant : uint8_t {
    Unmodified,
    ImplicitCoherentInterlockV1,
    BuiltinAtomicDepthMaxV1,
    OpaqueFormat3FullWordOverwriteV1,
    ForwardOnlyGuestControlFlowV1,
    ForwardStraightVertexControlFlowV1,
    ForwardColorPixelCatalogV1,
    BuiltinReadonlyDepthQueryV2,
    PipelineStateEpilogueV1,
    PipelineStateForwardCfgV1,
    NativeReadonlyDepthD32LoadV2,
    RetainedOpaquePackedAttachmentsV1,
    RetainedOpaquePackedCatalogV2,
    RetainedFp24DepthWriterV1,
    ExactIdentityFsiStoresV1,
    ImmutableFsiStateHoistV1,
    ImmutableFsiStateAndIncomingDepthHoistV2,
    ConservativeEarlyDepthV1,
    ConservativeEarlyDepthCatalogV2,
    NativeReadonlyDepthLessV1,
    NativeReadonlyDepthLowerBoundQueryV1,
    ReplayStencilMaterialObserveV1,
    ReplayStencilMaterialCutoffV1,
    StencilMaterialCutoffV1,
    RetainedOpaquePhaseV3,
    RetainedOpaquePhaseConservativeV4,
};
static_assert(uint32_t(NativeShaderVariant::RetainedOpaquePhaseV3)==24);
static_assert(uint32_t(NativeShaderVariant::RetainedOpaquePhaseConservativeV4)==25);
inline constexpr bool IsNativeReadonlyDepthVariant(NativeShaderVariant variant) {
    return variant==NativeShaderVariant::NativeReadonlyDepthD32LoadV2||
           variant==NativeShaderVariant::NativeReadonlyDepthLessV1||
           variant==NativeShaderVariant::NativeReadonlyDepthLowerBoundQueryV1;
}
inline constexpr std::string_view kImplicitFsiVariantIdentity =
    "native-fsi-implicit-coherent-interlock-v1";

// Remove only the two Device-scope barriers immediately inside the native FSI
// epilogue. All remaining SPIR-V words are preserved, including Coherent and
// DXC's additional interlock instructions. Unsupported shapes throw before
// module creation: this is deliberately not a general SPIR-V optimizer.
// Input must be valid SPIR-V produced by the native compiler. This is a
// restricted epilogue transform, not a substitute for complete SPIR-V/CFG
// validation. Unknown native instruction shapes are rejected.
std::vector<uint32_t> TransformNativeFsiImplicitBarriers(std::span<const uint32_t> words);

} // namespace gow3::renderer
