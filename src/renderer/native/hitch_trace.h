#pragma once
// Diagnostic stall tracer (gow3_hitch_trace): when no native frame completes
// for longer than the threshold, a watchdog samples every thread of the game
// every 20 ms until frames resume, then logs where each thread was. Windows
// only; elsewhere it does nothing.
#include <cstdint>

namespace gow3::renderer::hitch_trace {
// Starts the watchdog when enabled; safe to call more than once.
void Start(uint32_t threshold_ms);
// Native frame completion callback.
void FrameCompleted();
void Stop();
}  // namespace gow3::renderer::hitch_trace
