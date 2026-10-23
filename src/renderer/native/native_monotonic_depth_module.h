#pragma once

#include "fsi_shader_variant.h"
#include <cstdint>
#include <span>
#include <string_view>

namespace gow3::renderer {

// A closed, byte-authenticated cohort of unchanged compiler output. This only
// certifies that external writes use the original coherent packed epilogue;
// it does NOT certify a draw, monotonic state, layout, query, or PSO. The caller
// must check those independently against the actually selected live modules.
// Unknown sources, variants, changed bytes and audit failures are refused.
// Evaluate once at immutable module creation and keep the result with its
// owner; never substitute a source digest for the selected module identity.
bool NativeMonotonicDepthModuleCertified(std::string_view source, bool pixel,
    NativeShaderVariant variant, std::span<const uint8_t> bytes) noexcept;

} // namespace gow3::renderer
