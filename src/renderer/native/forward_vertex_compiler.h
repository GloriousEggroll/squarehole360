#pragma once
#include "shader_compiler.h"
#include "forward_vertex_control_flow.h"
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace gow3::renderer {
inline constexpr std::string_view kForwardVertexSource="f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
inline constexpr std::string_view kForwardVertexBaselineCompiler="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline constexpr uint32_t kForwardVertexMaxPipelines=128;
inline bool ForwardVertexRasterKey(const std::array<uint32_t,6>& key) noexcept {
    return key==std::array<uint32_t,6>{3,1,1,4,0,0}||key==std::array<uint32_t,6>{3,2,1,4,0,0}||key==std::array<uint32_t,6>{1,2,1,4,0,0};
}
inline bool ForwardVertexAllowed(std::string_view vertex,const std::array<uint32_t,6>& key) noexcept {
    return vertex==kForwardVertexSource&&ForwardVertexRasterKey(key);
}
struct ForwardVertexProgram {
    std::string source_sha256,compiler_version,variant_version;
    std::string modified_hlsl_sha256,baseline_hlsl_sha256,compiled_spirv_sha256;
    uint32_t control_instructions=0,terminal=0,padding_clauses=0,specialization_mask=0;
    std::vector<uint32_t> spirv;
};
// Startup only. The source hash and fixed straight-line CF are closed before
// the canonical translator; Get still fully validates the admitted container.
// All original generic caches/outputs remain unchanged.
std::shared_ptr<const ForwardVertexProgram> TryPrepareForwardVertex(
    NativeShaderCompiler&,std::span<const uint8_t> source);
std::string ForwardVertexCompilerVersion();
}
