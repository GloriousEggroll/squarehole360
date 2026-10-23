#pragma once
#include "shader_compiler.h"
#include <memory>
#include <span>
#include <string>
namespace gow3::renderer {
struct NativeRetainedOpaqueProgram;
std::string NativeRetainedOpaqueCatalogCompilerVersion();
// Startup only. Only the exact bounded catalogue is admitted; the canonical
// source cache and V1 producer remain unchanged.
std::shared_ptr<const NativeRetainedOpaqueProgram> TryPrepareNativeRetainedOpaqueCatalog(
    NativeShaderCompiler&, std::span<const uint8_t>);
}
