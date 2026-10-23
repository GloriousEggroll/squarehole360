#pragma once
#include "plume_render_interface_types.h"
#include "fsi_state_specialization_descriptor.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gow3::renderer {
// Hardware-independent recipes of pipelines actually created successfully.
// Source containers stay in this user's cache; no game content is packaged.
struct NativeFsiPipelineRecipe {
    std::string vertex;
    std::optional<std::string> pixel;
    std::array<uint32_t, 6> raster{};
    int32_t color_format = -1, depth_format = -1;
    auto operator<=>(const NativeFsiPipelineRecipe &) const = default;
};
using PipelineShaderSource = std::shared_ptr<const std::vector<uint8_t>>;
struct ObservedNativePipeline {
    NativeFsiPipelineRecipe recipe;
    PipelineShaderSource vertex, pixel;
};
bool ValidNativeFsiPipelineRecipe(const NativeFsiPipelineRecipe &) noexcept;

// Own all pointer targets until synchronous createGraphicsPipeline returns.
// Neither this object nor its descriptor may cross a deferred callback.
struct NativeFsiPipelineDescriptor {
    plume::RenderInputSlot slot{0, 16};
    plume::RenderInputElement element{"POSITION", 0, 0, plume::RenderFormat::R32G32B32A32_FLOAT,
                                      0,          0};
    plume::RenderSpecConstant specialization;
    std::optional<NativeFsiSpecializationConstants> fsi_specialization;
    plume::RenderGraphicsPipelineDesc descriptor;
    NativeFsiPipelineDescriptor(const std::array<uint32_t, 6> &raster, int32_t color, int32_t depth,
                                const plume::RenderPipelineLayout *, const plume::RenderShader *,
                                const plume::RenderShader *,
                                const NativeFsiStateSpecialization *state=nullptr);
    NativeFsiPipelineDescriptor(const NativeFsiPipelineDescriptor &) = delete;
    NativeFsiPipelineDescriptor &operator=(const NativeFsiPipelineDescriptor &) = delete;
};

class NativePipelineRecipeCache {
  public:
    static constexpr size_t kMaxRecipes = 2048, kMaxSources = 1024,
                            kMaxSourceBytes = 64u * 1024 * 1024;
    static constexpr size_t kMaxPending = 128, kMaxPendingBytes = 16u * 1024 * 1024;
    struct Limits {
        size_t recipes = kMaxRecipes, sources = kMaxSources, source_bytes = kMaxSourceBytes;
        size_t pending = kMaxPending, pending_bytes = kMaxPendingBytes;
    };
    struct Statistics {
        uint64_t admitted = 0, duplicate = 0, dropped = 0, written = 0, failures = 0;
        size_t pending = 0, pending_bytes = 0, peak_pending = 0, peak_pending_bytes = 0;
    };
    NativePipelineRecipeCache(std::filesystem::path directory, std::string compiler_version);
    NativePipelineRecipeCache(std::filesystem::path directory, std::string compiler_version,
                              Limits limits);
    ~NativePipelineRecipeCache();
    // Best effort and nonblocking: no waits for disk, space or a busy lock.
    void Observe(ObservedNativePipeline observation) noexcept;
    // Startup worker only; malformed/incompatible data is a cache miss.
    std::vector<ObservedNativePipeline> Load() const noexcept;
    void StopAccepting() noexcept;
    void Drain(); // Startup/tests/shutdown only, never a gameplay boundary.
    Statistics statistics() const;

  private:
    std::filesystem::path directory_;
    std::string version_;
    Limits limits_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<ObservedNativePipeline> pending_;
    std::set<NativeFsiPipelineRecipe> observed_;
    Statistics stats_;
    std::atomic<uint64_t> contended_drops_{0};
    bool stopping_ = false, working_ = false;
    std::thread worker_;
    void Worker() noexcept;
};
} // namespace gow3::renderer
