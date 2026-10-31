#pragma once
// Diagnostic inventory of the work the native renderer does to keep EDRAM
// sharing exact: surface commits/restores, resolves and alias loads, keyed by
// the guest call stack that caused them. Read-only with respect to rendering.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace gow3::renderer::edram_inventory {
inline constexpr unsigned kStackDepth = 6;
using GuestStack = std::array<uint32_t, kStackDepth>;

// Configure once before guest boot; an empty path disables everything.
void Configure(const std::string& path);
bool Enabled();

// D3D boundary thread: guest return addresses for an API event sequence.
void RecordStack(uint64_t sequence, const GuestStack& stack);

// Executor thread: the operation now running, then records against it.
void BeginOperation(uint64_t event, uint32_t frame, std::string_view type);
void Note(std::string_view kind, std::string_view surface, std::string_view other,
          std::string_view outcome, uint64_t pixels);
// Resolve destinations by base address, so later sampled reads can name the
// resolve that produced their bytes.
void RecordResolveTarget(uint32_t base, std::string_view description);
std::string ResolveTarget(uint32_t base);
// Executor statistics to include in the report, as serialized JSON.
void SetSection(std::string_view name, std::string json);
// Call at each completed frame; rewrites the report every few hundred frames.
void FrameCompleted(uint32_t frame);
void Flush();
}  // namespace gow3::renderer::edram_inventory
