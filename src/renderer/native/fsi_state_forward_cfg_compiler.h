#pragma once
#include "fsi_state_specialization_compiler.h"
namespace gow3::renderer {
// Separate derived compiler/cache identity. Startup only; canonical.Get is
// unchanged and the result owns the same seventeen exact FSI spec constants.
// Runtime boolean loads and all guest float/loop constants remain dynamic.
std::shared_ptr<const FsiStateSpecializationProgram> TryPrepareFsiStateForwardCfg(
    NativeShaderCompiler&,std::span<const uint8_t>,const std::filesystem::path& cache_directory={});
std::string FsiStateForwardCfgCompilerVersion();
}
