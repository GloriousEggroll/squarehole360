#pragma once
#include "fsi_state_specialization_recipe.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
namespace gow3::renderer {
// Stable counters allow resident draws to increment a count without locks,
// serialization or source/byte copies. Bind runs once per Scene tuple; bounded
// first admission uses try_lock, and all disk/JSON work runs on the writer.
class NativeFsiStateRecipeCache {
public:
    static constexpr size_t kMaxRecipes=NativeFsiStateSidecar::kMaxRecipes;
    struct Statistics {uint64_t bound=0,dropped=0,written=0,failures=0;size_t resident=0;};
    NativeFsiStateRecipeCache(std::filesystem::path directory,std::string variant);
    ~NativeFsiStateRecipeCache();
    std::atomic<uint64_t>* Bind(const NativeFsiStateRecipe&) noexcept;
    std::vector<RankedNativeFsiStateRecipe> Load(std::span<const ObservedNativePipeline>) const noexcept;
    void StopAccepting() noexcept;
    void Drain(); // Tests/startup/shutdown only, never a gameplay boundary.
    Statistics statistics() const;
private:
    struct Slot {NativeFsiStateRecipe recipe;std::atomic<uint64_t> count{0};};
    struct Lease;
    std::filesystem::path directory_;
    std::string variant_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Slot,kMaxRecipes> slots_{};
    size_t count_=0;
    Statistics stats_;
    std::atomic<uint64_t> contended_drops_{0};
    bool stopping_=false,force_=false,working_=false,exited_=false;
    uint64_t requested_=0,completed_=0;
    std::thread worker_;
    std::unique_ptr<Lease> lease_;
    NativeFsiStateSidecar Snapshot() const;
    void Worker() noexcept;
};
}
