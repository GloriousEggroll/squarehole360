#pragma once
#include "fsi_state_specialization_recipe.h"
#include "forward_pixel_catalog.h"
#include <optional>
#include <string>
#include <string_view>
namespace gow3::renderer {
inline constexpr std::string_view kFsiStateForwardCfgIdentity="native-fsi-state-forward-cfg-49f2-v1";
inline constexpr std::string_view kFsiStateForwardCfgPixel="49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced";
inline constexpr std::string_view kFsiStateForwardCfgVertex="f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
inline constexpr std::string_view kFsiStateForwardCfgBaselineCompiler="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
// Start with the actual paired shader/raster identities covered by both ISA
// diagnostics and the accepted room sidecars. No builtin, depth/MRT or VS rewrite.
inline bool FsiStateForwardCfgAllowed(std::string_view vertex,std::string_view pixel,
                                     const std::array<uint32_t,6>& raster) noexcept {
    return vertex==kFsiStateForwardCfgVertex&&pixel==kFsiStateForwardCfgPixel&&
        (raster==std::array<uint32_t,6>{3,1,1,4,0,0}||raster==std::array<uint32_t,6>{3,2,1,4,0,0});
}
inline bool FsiStateForwardCfgAllowed(const NativeFsiStateRecipe& recipe) noexcept {
    return ValidFsiStateRecipe(recipe)&&recipe.generic.pixel&&
        FsiStateForwardCfgAllowed(recipe.generic.vertex,*recipe.generic.pixel,recipe.generic.raster);
}
// The first stage changes only the common. Prove that independently before
// asking the existing exact wrapper helper to rewrite the guest control flow.
// Its material/FP/discard clauses, original derivatives and entire late FSI
// terminal (including color scale/depth offset handling) remain untouched.
inline std::optional<std::string> TryComposeFsiStateForwardCfgSource(
    std::string_view baseline,std::string_view state_source,std::string_view canonical_common,
    std::string_view state_common,const ForwardCfProgram& program) {
    if(canonical_common.empty()||state_common.empty()||canonical_common==state_common)return std::nullopt;
    auto restored=std::string(state_source);
    const auto at=restored.find(state_common);
    if(at==restored.npos||restored.find(state_common,at+state_common.size())!=restored.npos)return std::nullopt;
    restored.replace(at,state_common.size(),canonical_common);
    if(restored!=baseline)return std::nullopt;
    return TryForwardPixelCatalogSource(state_source,program);
}
}
