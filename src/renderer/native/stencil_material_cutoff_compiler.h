#pragma once
#include "stencil_material_cutoff.h"
#include "shader_compiler.h"
namespace gow3::renderer {
std::string NativeStencilMaterialCompilerVersion();
// Startup-only bounded single source, no global cache/provider mutations.
std::shared_ptr<const NativeStencilMaterialProgram> CompileNativeStencilMaterial(
    NativeShaderCompiler&,std::span<const uint8_t>,NativeStencilMaterialMode);
}
