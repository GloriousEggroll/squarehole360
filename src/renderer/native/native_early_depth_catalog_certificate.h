#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace gow3::renderer::detail {
// Fixture-only structural predicate: deliberately omits original byte-hash
// admission so the CPU fixture can reach parser/effect refusal branches.
// This never returns a shader program and cannot extend the closed catalogue.
// Production must use the public exact-byte constructor/certificate instead.
bool NativeEarlyDepthCatalogStructureForFixture(std::span<const uint32_t> words,
    bool pixel, size_t pin_index);
}
