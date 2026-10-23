#pragma once
#include "draw_timestamp_ledger.h"
#include "plume_render_interface.h"
#include <memory>
#include <nlohmann/json.hpp>

namespace gow3::renderer {
class NativeDrawTimestamps {
public:
  using Token = NativeDrawTimestampLedger::Token;
  NativeDrawTimestamps(plume::RenderDevice &, plume::RenderCommandQueue &,
                       uint32_t capacity, bool every_frame=false);
  ~NativeDrawTimestamps();
  bool SamplesFrame(uint32_t frame) const noexcept;
  void RejectMetadata() noexcept;
  uint32_t InternPair(std::string_view vertex,
                      std::optional<std::string_view> pixel) noexcept;
  bool Begin(Token, uint32_t frame, plume::RenderCommandList &) noexcept;
  void End(Token, const NativeGpuWorkTag &,
           plume::RenderCommandList &) noexcept;
  void Accept(Token) noexcept;
  void Cancel(Token) noexcept;
  // Called only after the existing queue fence has proven exactly these
  // accepted draw tokens complete; never queries/waits on a newer token.
  void Retire(std::span<const Token>, uint64_t submission) noexcept;
  nlohmann::json Report(bool details) const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

// Standalone preparing commands may be discarded before enqueue. Shared Scene
// recording instead latches a Native fatal failure before this unaccepted scope
// unwinds: it must never discard, submit or reuse an accepted leader's partial
// child. Cancel releases only this diagnostic ledger identity, not GPU owners.
class NativeDrawTimestampScope {
public:
  NativeDrawTimestampScope(NativeDrawTimestamps *owner,
                           NativeDrawTimestamps::Token token, uint32_t frame,
                           plume::RenderCommandList &commands)
      : owner_(owner), token_(token) {
    if (owner_ && !owner_->Begin(token_, frame, commands))
      owner_ = nullptr;
  }
  ~NativeDrawTimestampScope() noexcept {
    if (owner_ && !accepted_)
      owner_->Cancel(token_);
  }
  NativeDrawTimestampScope(const NativeDrawTimestampScope &) = delete;
  NativeDrawTimestampScope &
  operator=(const NativeDrawTimestampScope &) = delete;
  bool active() const noexcept { return owner_ != nullptr; }
  void End(const NativeGpuWorkTag &tag,
           plume::RenderCommandList &commands) noexcept {
    if (owner_)
      owner_->End(token_, tag, commands);
  }
  void Accepted() noexcept {
    // Enqueue has succeeded. Never cancel potentially submitted ownership
    // if an invariant failure is raised by the bookkeeping below.
    accepted_ = true;
    if (owner_)
      owner_->Accept(token_);
  }

private:
  NativeDrawTimestamps *owner_;
  NativeDrawTimestamps::Token token_;
  bool accepted_ = false;
};
} // namespace gow3::renderer
