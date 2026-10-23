#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::graphics { class GraphicsSystem; class ShaderStorageProgress; }

// Start before guest boot in Native mode, with exclusive scene authority.
// Xenia mode does not install native callbacks. Native startup errors propagate
// to the application; changing renderer requires a restart.
void Gow3StartNativeRenderer(rex::graphics::GraphicsSystem* graphics,
                            const std::filesystem::path& cache_root);
void Gow3StopNativeRenderer();

// Called on UI before the startup worker exists. The closure owns runtime
// lifetime through source preparation and its CP task, including close/cancel.
std::function<void(rex::graphics::ShaderStorageProgress&)> Gow3CreateNativePipelinePrewarmTask();

// Low-latency pacing: waits until the native renderer has completed API frame
// `frame` (or the timeout passes). Returns false at once when no native
// renderer is running.
bool Gow3WaitForNativeFrame(uint32_t frame, std::chrono::milliseconds timeout);
