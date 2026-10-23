#pragma once
#include "shader_compiler.h"
#include "retained_opaque_phase_catalog.h"

namespace gow3::renderer {
// Startup-only compilation of the closed observed material cohort. Unknown
// sources/bytes are refused; compilation exceptions remain visible. Gameplay
// requires an independently authenticated resident module and exact PSO recipe.
std::shared_ptr<const NativeRetainedOpaquePhaseProgram>
TryPrepareNativeRetainedOpaquePhase(NativeShaderCompiler&,
    std::span<const uint8_t> container);
std::string NativeRetainedOpaquePhaseCompilerVersion();
// Separate V4 preparation. Module bytes and matching restore/store encoding
// require a separately authenticated version; existing V3 remains unchanged.
std::shared_ptr<const NativeRetainedOpaquePhaseProgram>
TryPrepareNativeRetainedOpaquePhaseConservative(NativeShaderCompiler&,
    std::span<const uint8_t> container);
std::string NativeRetainedOpaquePhaseConservativeCompilerVersion();
} // namespace gow3::renderer
