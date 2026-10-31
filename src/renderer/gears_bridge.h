#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "native/query_readiness_census.h"

namespace rex::memory { class Memory; }
namespace rex::graphics { class CommandProcessor; struct NativeCompletionMarkerPair; }

// Captures the Gears D3D API boundary for native execution and diagnostics.
// Packet ownership is decided separately on the ordered command thread.
void Gow3StartGraphicsBridge(rex::memory::Memory* memory,rex::graphics::CommandProcessor* processor=nullptr);
void Gow3StopGraphicsBridge();

namespace gow3::renderer {
struct MemoryExportCopy;
// Diagnostic observation only: available/live are last-observed title metadata,
// never a pin, allocator-generation certificate, or permission to defer stores.
struct ObservedMarkerCounterAllocation {
  uint32_t device=0,virtual_counter=0,physical_counter=0,allocation_base=0,allocation_bytes=0,logical_bytes=0;
  uint64_t registration_generation=0,command_epoch=0;
  bool available=false,live=false;
};
ObservedMarkerCounterAllocation ObserveMarkerCounterAllocation(uint32_t physical_target) noexcept;
// Synchronous authorization only. Acquire AFTER submission, retain only across
// the two original adjacent marker stores, and destroy before callbacks/drains.
// This owns locks and exact emitted-pair identity, never a translated pointer.
std::shared_ptr<void> AcquireNativeImmediateMarkerTargetGuard(
    const rex::graphics::NativeCompletionMarkerPair& pair);
// Immutable PM4 bytes captured before publishing the guest command pointer.
// Addresses alone are not identities: the game recycles command buffers.
struct GraphicsCommandSpan {
  uint64_t generation = 0, api_sequence = 0;
  uint32_t address = 0;
  std::vector<uint8_t> bytes;
};
struct GraphicsBoundaryBlock {
  uint32_t kind = 0, address = 0;
  std::vector<uint8_t> bytes;
  std::shared_ptr<const std::vector<uint8_t>> shared_bytes;
  // Live texture-only descriptor. Materialize synchronously after the planner
  // processes CPU invalidations and determines that CPU bytes are required.
  uint32_t deferred_physical_bytes = 0;
  std::span<const uint8_t> data() const {return shared_bytes?std::span<const uint8_t>(*shared_bytes):std::span<const uint8_t>(bytes);}
};
// Owns snapshots taken at the D3D boundary, including finalized UP vertices.
// GPU-produced render targets are dependencies, never CPU texture substitutes.
struct GraphicsBoundaryEvent {
  uint64_t sequence = 0;
  uint32_t function = 0, phase = 0, frame = 0, lr = 0, result = 0;
  std::array<uint32_t, 8> args{};
  std::vector<GraphicsBoundaryBlock> blocks;
  std::optional<GraphicsCommandSpan> command_span;
  bool span_only = false;
  std::string capture_error;
  // Valid only on the originating D3D thread during its sink invocation. This
  // capability is never retained in a LiveRenderFrame or queued GPU prefix.
  std::function<void(GraphicsBoundaryBlock&)> materialize_texture;
  // Optional synchronous CPU-only request batching with the same capability
  // lifetime. Blocks remain original-sized immutable snapshots, never views
  // into an aggregate payload owner.
  std::function<void(std::span<GraphicsBoundaryBlock* const>)> materialize_textures;
};
using GraphicsBoundarySink = std::function<void(GraphicsBoundaryEvent&&)>;
// Offered each verified GPU memcpy chunk before its event is captured. It
// returns true when it performed the copy itself; no event is then produced.
using DirectMemoryCopySink = std::function<bool(const MemoryExportCopy&)>;
// Shader discovery is speculative and does not create renderer events. Sources
// are immutable exact virtual+physical containers copied on the guest caller.
// speculative: a block neighbour the game has not asked for yet.
using ShaderDiscoverySink = std::function<void(std::shared_ptr<const std::vector<uint8_t>>, bool pixel, bool speculative)>;
}

// The sink runs on the guest D3D caller, in API order. It should enqueue owned
// events rather than render while holding the bridge lock. Installing it does
// not require or create a diagnostic capture file. Install before guest boot
// to retain all native previous-frame history.
void Gow3SetGraphicsBoundarySink(gow3::renderer::GraphicsBoundarySink sink,
                                 gow3::renderer::DirectMemoryCopySink copy_sink = {});
// Enable before installing a sink. Regular draws are then captured at each
// finalized chunk; diagnostic file records retain their existing API format.
void Gow3EnableGraphicsCommandSpans(bool enabled);

// Native-only optional preparation. The sink must enqueue without waiting; it
// runs under the bridge lock. Detach before joining its CPU workers elsewhere.
void Gow3SetShaderDiscoverySink(gow3::renderer::ShaderDiscoverySink sink);

// Native-only diagnostic scheduling after real pending GetData returns. The
// original function and all query availability/results remain authoritative.
// Disable invalidates each guest thread's pending streak without joining it.
void Gow3EnableNativeQueryPollYield(bool enabled);
// Cumulative guest time in the blocking occlusion-query retry loop: from the
// first retry poll of a query until GetData reports it complete.
struct Gow3QueryWaitStats {uint64_t waits=0,nanoseconds=0,direct_results=0,direct_latency_ns=0,direct_waits=0,direct_backlog=0;};
// Direct execution (no PM4 command processor): the guest D3D runs GPU-less and
// would report every occlusion query as visible. These carry the renderer's
// real results to GetData instead. Ended runs on the API thread at the query
// end; Result runs on the executor when the GPU result is known.
void Gow3EnableDirectQueries(bool enabled);
void Gow3DirectQueryBegun(uint32_t query);
void Gow3DirectQueryEnded(uint32_t query,uint64_t generation);
void Gow3DirectQueryResult(uint64_t generation,uint64_t samples);
// True once since the last call if GetData found a direct query still pending.
bool Gow3TakeDirectQueryWaiting();
Gow3QueryWaitStats Gow3GetQueryWaitStats();

// Optional exact CPU body selected only after strict Native startup. Refuses
// mismatched code/helpers and trace/profile builds; disable retains no owners.
void Gow3EnableNativeGetDataInline(bool enabled);

// Diagnostic only: sample real GetData without changing its result or context.
using Gow3QueryReadinessProbe = std::function<std::optional<gow3::renderer::QueryReadinessReceipt>(uint32_t)>;
void Gow3SetQueryReadinessProbe(Gow3QueryReadinessProbe probe);
struct Gow3QueryReadinessStatistics {
  uint64_t samples=0, tracked=0, published_before=0, pending_after_published=0,
           completed_after_published=0, changed_during_call=0, errors=0;
};
Gow3QueryReadinessStatistics Gow3ReadQueryReadinessStatistics();

// Optional native occlusion-result consumer; global markers are unchanged.
namespace gow3::renderer { class PublishedQueryResults; }
void Gow3SetPublishedQueryResults(std::shared_ptr<gow3::renderer::PublishedQueryResults> results);
std::array<uint64_t,6> Gow3ReadPublishedQueryStatistics();
