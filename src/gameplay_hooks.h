#pragma once

#include <cstdint>

namespace rex::ppc { union Register; }

// Declarations are force-included in the generated translation units. The
// hook sites themselves are emitted from config/gameplay-hooks.toml.
bool Gow3UncapFrameRate();
void Gow3SetPresentInterval(rex::ppc::Register& interval);
void Gow3ConfigureFrameRate(const uint8_t* guest_base);
void Gow3CaptureUpEnd(rex::ppc::Register& device,rex::ppc::Register& command_end);
void Gow3CapturePrimitiveChunk(rex::ppc::Register& device,rex::ppc::Register& topology,
    rex::ppc::Register& first,rex::ppc::Register& count,rex::ppc::Register& command_end);
void Gow3CaptureIndexedChunk(rex::ppc::Register& device,rex::ppc::Register& topology,
    rex::ppc::Register& first,rex::ppc::Register& count,rex::ppc::Register& command_end);
void Gow3CaptureOwnedCommands(rex::ppc::Register& device,rex::ppc::Register& command_end);

// Optional title-known counter metadata census; every hook preserves registers.
void Gow3ObserveMarkerCounterInitialized(rex::ppc::Register& device, rex::ppc::Register& counter) noexcept;
void Gow3ObserveMarkerCounterDestroyed(rex::ppc::Register& device) noexcept;
void Gow3ObserveMarkerCounterRingReset(rex::ppc::Register& device) noexcept;
void Gow3ObserveMarkerCounterRingReady(rex::ppc::Register& device, rex::ppc::Register& counter) noexcept;
void Gow3ObserveMarkerCounterRecovery(rex::ppc::Register& device) noexcept;
void Gow3ObserveMarkerCounterCpuDirect(rex::ppc::Register& device) noexcept;
void Gow3ObserveMarkerPairEmitted(rex::ppc::Register& device, rex::ppc::Register& last_word,
                                  rex::ppc::Register& counter, rex::ppc::Register& position,
                                  rex::ppc::Register& serial) noexcept;
