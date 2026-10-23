#pragma once
#include "fsi_shader_variant.h"
#include <cstdint>
#include <span>
#include <string_view>

namespace gow3::renderer {
// Closed exact-byte cohort. This certifies only a module's authenticated
// source/output effects; draw state, selected owner, PSO and authority must be
// checked independently. Evaluate once when the immutable module is created.
// Canonical VS0 and distinct legacy PS24 / conservative PS25 byte cohorts.
// A Scene must select matching conversion modules for its admitted variant.
bool NativeRetainedOpaquePhaseModuleCertified(std::string_view source,
    bool pixel, NativeShaderVariant variant,
    std::span<const uint8_t> bytes) noexcept;

// Startup source-provenance check only. Canonical pixels remain ineligible for
// the packed-output module API above; this cannot authorize a selected PSO.
bool NativeRetainedOpaquePhaseCanonicalPixelCertified(std::string_view source,
    std::span<const uint8_t> bytes) noexcept;
std::string_view NativeRetainedOpaquePhaseBytecodeVersion() noexcept;
std::string_view NativeRetainedOpaquePhaseConservativeBytecodeVersion() noexcept;
} // namespace gow3::renderer
