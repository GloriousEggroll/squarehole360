#pragma once
#include "shader_compiler.h"
#include "forward_pixel_catalog.h"
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace gow3::renderer {
inline constexpr std::string_view kForwardPixelCatalogBaselineCompiler="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
struct ForwardPixelCatalogProgram {
    std::string source_sha256,compiler_version,variant_version;
    std::string modified_hlsl_sha256,baseline_hlsl_sha256,compiled_spirv_sha256;
    uint32_t control_instructions=0,specialization_mask=0,output_mask=0;
    std::vector<uint32_t> spirv;
};
struct ForwardPixelCatalog {
    std::map<std::string,std::shared_ptr<const ForwardPixelCatalogProgram>,std::less<>> programs;
};
// Startup only; no on-disk candidate cache or gameplay provider is installed.
std::shared_ptr<const ForwardPixelCatalogProgram> TryPrepareForwardPixelCatalog(
    NativeShaderCompiler&,std::span<const uint8_t> source);
std::string ForwardPixelCatalogCompilerVersion();
} // namespace gow3::renderer
