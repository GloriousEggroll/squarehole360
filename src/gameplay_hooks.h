#pragma once

#include <cstdint>

namespace rex::ppc { union Register; }
namespace rex::ui { class Window; }

// Declarations are force-included in the generated translation units. The
// hook sites themselves are emitted from config/gameplay-hooks.toml.
bool Gow3UncapFrameRate();
void Gow3SetPresentInterval(rex::ppc::Register& interval);
void Gow3ExpireDepartedConnection(rex::ppc::Register& connection);
void Gow3ConfigureFrameRate(const uint8_t* guest_base);
void Gow3ConfigureInputPrompts(const uint8_t* guest_base);
void Gow3OverrideScreenSize();
// The height of the game's own layout: gow3_render_height, except 1440p,
// which is the 720p layout drawn at twice the size by the native renderer.
int Gow3GuestRenderHeight();
// The window gow3_aspect_ratio = auto follows; set before the game starts.
void Gow3SetDisplayWindow(rex::ui::Window* window);
void Gow3ScreenWidthConstant(rex::ppc::Register& value);
void Gow3ScreenHeightConstant(rex::ppc::Register& value);
void Gow3ScaleformScaleMode(rex::ppc::Register& mode);
void Gow3ScaleformViewport(rex::ppc::Register& viewport);
void Gow3ScreenWidthFloat(rex::ppc::Register& value);
void Gow3ScreenHeightFloat(rex::ppc::Register& value);
void Gow3ConstrainedProjection(rex::ppc::Register& half_fov, rex::ppc::Register& aspect,
                               rex::ppc::Register& height, rex::ppc::Register& camera);
void Gow3UnconstrainedProjection(rex::ppc::Register& half_fov, rex::ppc::Register& half_fov_y,
                                 rex::ppc::Register& local_player);
void Gow3CaptureUpEnd(rex::ppc::Register& device,rex::ppc::Register& command_end);
void Gow3CapturePrimitiveChunk(rex::ppc::Register& device,rex::ppc::Register& topology,
    rex::ppc::Register& first,rex::ppc::Register& count,rex::ppc::Register& command_end);
void Gow3CaptureIndexedChunk(rex::ppc::Register& device,rex::ppc::Register& topology,
    rex::ppc::Register& first,rex::ppc::Register& count,rex::ppc::Register& command_end);
void Gow3CaptureOwnedCommands(rex::ppc::Register& device,rex::ppc::Register& command_end);

// Optional title-known counter metadata census; every hook preserves registers.
// Not noexcept: codegen emits its own extern declarations for midasm hooks,
// without an exception specification. The observers catch internally.
void Gow3ObserveMarkerCounterInitialized(rex::ppc::Register& device, rex::ppc::Register& counter);
void Gow3ObserveMarkerCounterDestroyed(rex::ppc::Register& device);
void Gow3ObserveMarkerCounterRingReset(rex::ppc::Register& device);
void Gow3ObserveMarkerCounterRingReady(rex::ppc::Register& device, rex::ppc::Register& counter);
void Gow3ObserveMarkerCounterRecovery(rex::ppc::Register& device);
void Gow3ObserveMarkerCounterCpuDirect(rex::ppc::Register& device);
void Gow3ObserveMarkerPairEmitted(rex::ppc::Register& device, rex::ppc::Register& last_word,
                                  rex::ppc::Register& counter, rex::ppc::Register& position,
                                  rex::ppc::Register& serial);
