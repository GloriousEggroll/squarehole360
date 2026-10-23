#pragma once
#include "native_early_depth_catalog.h"
#include "native_early_depth_shader.h"
#include <string_view>

namespace gow3::renderer {
inline constexpr std::string_view kFsiStateMaterialCompositionPolicy =
    "native-fsi-state-material-composition-v1";

// The optional admission only covers the exact two pairs whose early-depth
// catalogue is inactive. The shipping early-depth source remains excluded.
inline bool FsiStateInactiveEarlyCatalogPair(std::string_view vertex,
                                            std::string_view pixel) {
    if(vertex!=kNativeEarlyDepthCatalogVertexSource)return false;
    for(auto source:kNativeEarlyDepthCatalogPixelSources)
        if(pixel==source)return true;
    return false;
}

// Pixel-only exclusions remain stronger than the early-depth pair classifiers
// unless explicitly admitting an inactive catalogue's exact canonical pair.
inline bool FsiStateMaterialCompositionSourceAllowed(std::string_view vertex,
                                                    std::string_view pixel,
                                                    bool admit_inactive_catalog=false,
                                                    bool early_depth_catalog=false) {
    if (vertex.size() != 64 || pixel.size() != 64 ||
        pixel == kNativeEarlyDepthPixelSource) return false;
    for (auto excluded : kNativeEarlyDepthCatalogPixelSources)
        if (pixel == excluded) return admit_inactive_catalog&&!early_depth_catalog&&
            FsiStateInactiveEarlyCatalogPair(vertex,pixel);
    return true;
}

struct FsiStateMaterialCompositionProof {
    bool active_query = false;
    bool discarded = false;
    bool rectangle = false;
    bool canonical_vertex = false;
    bool canonical_pixel = false;
    bool private_framebuffer = false;
};
enum class FsiStateMaterialCompositionAdmission { Source, Query, State, Admitted };
inline FsiStateMaterialCompositionAdmission ClassifyFsiStateMaterialComposition(
    std::string_view vertex, std::string_view pixel,
    const FsiStateMaterialCompositionProof& proof,
    bool admit_inactive_catalog=false, bool early_depth_catalog=false) {
    if (!FsiStateMaterialCompositionSourceAllowed(vertex, pixel,
            admit_inactive_catalog,early_depth_catalog))
        return FsiStateMaterialCompositionAdmission::Source;
    if (proof.active_query) return FsiStateMaterialCompositionAdmission::Query;
    if (proof.discarded || proof.rectangle || !proof.canonical_vertex ||
        !proof.canonical_pixel || !proof.private_framebuffer)
        return FsiStateMaterialCompositionAdmission::State;
    return FsiStateMaterialCompositionAdmission::Admitted;
}
}  // namespace gow3::renderer
