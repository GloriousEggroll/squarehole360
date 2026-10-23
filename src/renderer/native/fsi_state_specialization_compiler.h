#pragma once
#include "renderer/native/shader_compiler.h"
#include "fsi_state_specialization.h"
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace gow3::renderer {
struct FsiStateSpecializationProgram {
    std::string source_sha256,compiler_version,variant_version;
    std::string modified_common_sha256,baseline_hlsl_sha256,modified_hlsl_sha256,compiled_spirv_sha256;
    uint32_t specialization_mask=0;
    bool disk_cache_hit=false,depth_only=false;
    std::vector<uint32_t> spirv;
};
struct FsiStateSpecializationCatalog {
    // Startup builds and deduplicates this by guest source digest. The Scene
    // receives an immutable snapshot. State tuple is a PSO key, not a module key.
    std::map<std::string,std::shared_ptr<const FsiStateSpecializationProgram>,std::less<>> programs;
    std::shared_ptr<const FsiStateSpecializationProgram> depth_only;
};
// Startup worker only. Accept any canonically validated pixel container; no
// source digest pin, gameplay compilation, process launch, or Vulkan work.
// A separate best-effort disk cache is namespaced by variant/compiler version.
std::shared_ptr<const FsiStateSpecializationProgram> TryPrepareFsiStateSpecialization(
    NativeShaderCompiler&,std::span<const uint8_t>,const std::filesystem::path& cache_directory={});
std::shared_ptr<const FsiStateSpecializationProgram> TryPrepareDepthOnlyFsiStateSpecialization(
    NativeShaderCompiler&,const std::filesystem::path& cache_directory={});
std::string FsiStateSpecializationCompilerVersion();
}
