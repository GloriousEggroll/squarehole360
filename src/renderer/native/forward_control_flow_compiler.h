#pragma once
#include "shader_compiler.h"
#include "forward_control_flow.h"
#include "fsi_state.h"
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer {
inline constexpr std::string_view kForwardControlFlowPixel =
    "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced";
inline constexpr std::string_view kForwardControlFlowVertex =
    "f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
// Broaden only after revalidating the actual compiler output and GPU fixture.
inline constexpr std::string_view kForwardControlFlowBaselineCompiler =
    "1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline bool ForwardControlFlowRasterKey(const std::array<uint32_t,6>& key) noexcept {
    return key==std::array<uint32_t,6>{3,1,1,4,0,0}||
           key==std::array<uint32_t,6>{3,2,1,4,0,0};
}
// The complete material, alpha/discard path and terminal Native FSI helper
// remain unchanged. No color/depth/blend/query state is specialized here.
// The pinned source pair and two prewarmed host raster shapes define admission;
// the caller retains its ordinary Native state/layout validation and residency.
inline bool ForwardControlFlowAllowed(std::string_view vertex,std::string_view pixel,
    const std::array<uint32_t,6>& host_raster) noexcept {
    return vertex==kForwardControlFlowVertex&&pixel==kForwardControlFlowPixel&&
        ForwardControlFlowRasterKey(host_raster);
}
struct ForwardControlFlowProgram {
    std::string source_sha256;
    std::string compiler_version;
    std::string variant_version;
    std::string modified_hlsl_sha256;
    std::string baseline_hlsl_sha256;
    uint32_t control_instructions=0;
    std::string compiled_spirv_sha256;
    uint32_t specialization_mask=0;
    std::vector<uint32_t> spirv;
};
// Startup worker only. Reuses canonical.Get's exact container validation;
// Unknown compiler/source/helper shapes return nullptr before unsupported
// sources enter the canonical translator. The pinned container remains fully
// validated; DXC errors throw for an optional experiment refusal.
// No disk cache, process launch, Vulkan work or gameplay compilation.
std::shared_ptr<const ForwardControlFlowProgram> TryPrepareForwardControlFlow(
    NativeShaderCompiler&,std::span<const uint8_t> source);
std::string ForwardControlFlowCompilerVersion();
} // namespace gow3::renderer
