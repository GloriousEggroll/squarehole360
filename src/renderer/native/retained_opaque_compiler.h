#pragma once
#include "shader_compiler.h"
namespace gow3::renderer {
struct NativeRetainedOpaqueProgram;
// Startup-only, pinned-source compiler. Gameplay accepts only a resident
// separately keyed pipeline and never compiles in the draw path.
std::shared_ptr<const NativeRetainedOpaqueProgram> TryPrepareNativeRetainedOpaque(
    NativeShaderCompiler&,std::span<const uint8_t>);
std::string NativeRetainedOpaqueCompilerVersion();
}
