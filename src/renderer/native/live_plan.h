#pragma once
#include "renderer/gears_bridge.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/live_prefix_resources.h"
#include <nlohmann/json.hpp>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace gow3::renderer {
struct LiveRenderFrame {
    LiveRenderFrame()=default;
    LiveRenderFrame(const LiveRenderFrame&)=default;
    LiveRenderFrame(LiveRenderFrame&&) noexcept=default;
    // A throwing copy assignment could replace operation anchors before replacing
    // the table that borrows them. Published frames may be copied or moved,
    // but cannot be partially overwritten that way.
    LiveRenderFrame& operator=(const LiveRenderFrame&)=delete;
    LiveRenderFrame& operator=(LiveRenderFrame&&) noexcept=default;
    uint32_t frame = 0;
    LiveOperations operations;
    using Bytes = LiveResourceBytes;
    LiveResourceMap payloads, shaders;
    size_t retained_bytes = 0;
    // Declared after operations: borrowed digest views die before their immutable
    // anchors. Prefixes never retain the mutable producer frame or its bank.
    std::optional<LivePrefixResources> prefix_resources;
    // Only the producer mutates this builder; frames/prefixes seal before publication.
    std::shared_ptr<LazyPayloadTable> lazy_payloads;
    const Bytes* FindResource(std::string_view hash,bool shader=false) const {
        if(!shader&&lazy_payloads)return lazy_payloads->Find(hash);
        if(prefix_resources)return prefix_resources->Find(hash,shader);
        const auto& values=shader?shaders:payloads;const auto found=values.find(hash);
        return found==values.end()?nullptr:&found->second;
    }
    const Bytes& ResourceAt(std::string_view hash,bool shader=false) const {
        if(!shader&&lazy_payloads) {const auto* found=FindResource(hash,false);if(!found)throw std::out_of_range("Missing lazy payload");return *found;}
        if(!prefix_resources) {
            const auto& values=shader?shaders:payloads;const auto found=values.find(hash);
            if(found==values.end())throw std::out_of_range("unordered_map::at");
            return found->second;
        }
        const auto* found=prefix_resources->Find(hash,shader);
        if(!found)throw std::out_of_range("Missing native prefix resource");
        return *found;
    }
    size_t ResourceCount(bool shader=false) const {
        if(!shader&&lazy_payloads)return lazy_payloads->size();
        return prefix_resources?prefix_resources->size(shader):(shader?shaders:payloads).size();
    }
    template<class Visit>void ForEachResource(bool shader,Visit&& visit) const {
        if(!shader&&lazy_payloads)lazy_payloads->ForEach(std::forward<Visit>(visit));
        else if(prefix_resources)prefix_resources->ForEach(shader,std::forward<Visit>(visit));
        else for(const auto& [hash,bytes]:(shader?shaders:payloads))visit(hash,bytes);
    }
    LiveResourceMap MaterializeResources(bool shader=false) const {
        if(!shader&&lazy_payloads)return lazy_payloads->Canonical();
        return prefix_resources?prefix_resources->Materialize(shader):(shader?shaders:payloads);
    }
};
struct LiveQueryPrefix {
    std::shared_ptr<LiveRenderFrame> batch;
    size_t first_operation=0,operation_end=0;
    uint32_t end_address=0;
    uint64_t generation=0;
};
struct LivePacketPrefix {
    std::shared_ptr<LiveRenderFrame> batch;
    size_t first_operation=0,operation_end=0;
    GraphicsCommandSpan span;
};

// Direct prefixes also end before exceeding the flat dependency bound.
extern bool dependency_bounded_direct_prefixes;

// Converts immutable API snapshots to the same contract used by scene replay.
// Events are consumed in order and deduplicated immediately, so a frame never
// retains a separate copy of every unchanged texture for every draw. A completed
// frame can be queued to the GPU thread without retaining guest-memory pointers.
class GearsLivePlan {
public:
    explicit GearsLivePlan(std::function<void(uint32_t,uint32_t)> watch_memory={},bool query_prefixes=false,bool lazy_device_states=false,bool direct_resource_refs=false,bool fast_fingerprints=false);
    ~GearsLivePlan();
    // Thread-safe diagnostic state; no bank materialization or hashing.
    const char* LazyStateMode()const noexcept;
    struct ResourceHandoffStatistics {bool enabled=false;uint64_t handoffs=0;};
    // Immutable option and relaxed batched producer counter only; no bank read.
    ResourceHandoffStatistics ResourceHandoffStats()const noexcept;
    // Lazy device-state interning outcomes (relaxed producer counters).
    struct DeviceStateStatistics {uint64_t calls=0,current_hits=0,previous_hits=0,created=0,collisions=0;};
    DeviceStateStatistics DeviceStateStats()const noexcept;
    // Payload interning outcomes per class (0 vertex/index, 1 UP, 2 texture,
    // 3 other); outcome 0 snapshot identity, 1 current, 2 previous, 3 created.
    struct PayloadInternStatistics {std::array<std::array<uint64_t,4>,4> calls{},bytes{};};
    PayloadInternStatistics PayloadInternStats()const noexcept;
    std::shared_ptr<LiveRenderFrame> Push(GraphicsBoundaryEvent&& event);
    // Enabled at construction. Consume immediately after Push: no GPU wait or
    // queue backpressure belongs here, because the guest may not have submitted
    // the command buffer containing this query yet.
    std::optional<LiveQueryPrefix> TakeQueryPrefix();
    std::optional<LivePacketPrefix> TakePacketPrefix();
    // The memory-export copy planned by the last Push, if any. cpu_source is
    // true when no source byte was GPU-owned at capture.
    struct MemoryCopy {uint32_t source=0,destination=0,bytes=0;bool cpu_source=false;};
    std::optional<MemoryCopy> TakeMemoryCopy();
    // Direct execution: once at least min_operations follow the last prefix,
    // detach them (no query end) so execution keeps pace with the API thread.
    std::optional<LiveQueryPrefix> TakeDirectPrefix(size_t min_operations);
    // Called in API order before Push. Empty data means a freed/unreadable
    // range; readable CPU writes seed the physical backing for partial reuse.
    void InvalidateMemory(uint32_t address,uint32_t bytes,std::vector<uint8_t> data={},uint64_t coherence_epoch=0);
private:
    struct State;
    std::unique_ptr<State> state_;
};
// Guest-thread replica of the planner's GPU byte ownership, for a planner that
// runs on its own thread. Fed the same API-ordered invalidations and events, it
// reaches the planner's decisions at each event, so CPU texture snapshots,
// memory watches and CPU-sourced memory-export copies still happen at the API
// point. Any divergence fails closed: the planner refuses an unsnapshotted
// texture it needs, and checks every query generation.
class GearsLiveOwnership {
public:
    explicit GearsLiveOwnership(std::function<void(uint32_t,uint32_t)> watch_memory);
    ~GearsLiveOwnership();
    void InvalidateMemory(uint32_t address,uint32_t bytes);
    struct Decision {
        std::optional<GearsLivePlan::MemoryCopy> memory_copy;
        bool query_begin=false,query_end=false;
        uint32_t query_handle=0;uint64_t query_generation=0;
        std::array<GraphicsBoundaryBlock*,64> textures{};size_t texture_count=0;
    };
    // Call in API order before the event leaves the guest thread.
    Decision Observe(GraphicsBoundaryEvent& event);
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace gow3::renderer
