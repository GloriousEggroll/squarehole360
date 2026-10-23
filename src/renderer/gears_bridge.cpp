#include "gears_bridge.h"
#include "native/shader_block_discovery.h"
#include "native/content_hash.h"
#include "native/up_commit_sites.h"
#include "native/packet_commit_sites.h"
#include "native/packet_ownership.h"
#include "native/shader_input_cache.h"
#include "native/fast_shader_fingerprint.h"
#include "native/shader_discovery.h"
#include "native/query_poll_yield.h"
#include "native/published_query_results.h"
#include "native/getdata_inline_leaf.h"
#include "native/getdata_inline_selection.h"
#include "../gow3_renderer_mode.h"
#include <thread>
#include "native/immutable_snapshots.h"
#include "renderer/game_profile.h"
#include "native/lazy_device_state.h"
#include "native/live_metadata_capture.h"
#include "native/deferred_texture_snapshots.h"
#include "native/capture_texture_extents.h"
#include "generated/default/gow3_init.h"
#include <rex/cvar.h>
#include <rex/memory.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/native_completion_markers.h>
#include <rex/graphics/frame_limiter.h>
#if GOW3_HAS_NATIVE_RENDERER
#include "native/runtime_bridge.h"
#endif
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

REXCVAR_DECLARE(bool, gpu_command_summary);
REXCVAR_DECLARE(double, framerate_limit);
REXCVAR_DECLARE(bool, present_anti_lag);
REXCVAR_DEFINE_BOOL(gow3_low_latency, false, "Display",
                    "Keep the game at most one frame ahead of the renderer and apply the "
                    "frame-rate limit before the next frame reads input");
REXCVAR_DECLARE(bool, gpu_native_marker_operands);
REXCVAR_DECLARE(bool, gpu_native_submit_completion_markers);
REXCVAR_DECLARE(bool, gow3_native_fast_snapshot_fingerprints);

REXCVAR_DEFINE_BOOL(gow3_native_block_shader_prewarm, false, "Renderer diagnostics",
                   "Prepare exact pixel shader neighbors in successfully decoded shader-cache blocks")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_native_thin_live_metadata, false, "Renderer diagnostics",
                   "Omit unconsumed live object copies; preserve full diagnostic capture and missing-VS context")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_snapshot_metadata_lru, true, "Renderer diagnostics",
                   "Retire one oldest snapshot at the metadata bound instead of clearing all snapshots")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_certified_snapshots, true, "Renderer diagnostics",
                   "Experimental SDK-certified physical snapshots with exact fallback")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_direct_getdata_wait_us, 2000, "Renderer diagnostics",
                    "Direct execution: a repeated GetData for a pending query blocks up to this many microseconds "
                    "for the next published result instead of spinning (0 spins)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_stable_certified_snapshots, true, "Renderer diagnostics",
                   "Arm SDK byte certificates only after exact reuse; invalid tokens cool down without immediate re-arm")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_certified_min_snapshot_bytes, 4096, "Renderer diagnostics",
                    "Only request SDK byte certificates for snapshots at least this large; zero certifies all eligible snapshots")
    .range(0,8*1024*1024).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_certified_fault_fastpath, true, "Renderer diagnostics",
                   "Dispatch proven snapshot write faults without an OS permission scan")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_certified_batch_queries, true, "Renderer diagnostics",
                   "Query all snapshot alias permissions together before protecting pages")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_batch_texture_snapshots, true, "Renderer diagnostics",
                   "Batch required live CPU texture certificates without changing scalar snapshot policy")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_warm_snapshot_readability, false, "Renderer diagnostics",
                   "Reuse only wholly validated warm texture batches before repeating physical readability probes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_snapshot_generation_memo, true, "Renderer diagnostics",
                   "Reuse a draw's certified texture snapshots while no certified page has changed")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_snapshot_generation_memo_verify, false, "Renderer diagnostics",
                   "Also run the full snapshot path on every memo hit and count differences")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_validate_active_small_snapshots, false, "Renderer diagnostics",
                   "Freshly validate already-active small texture snapshots without changing cold admission")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_cache_capture_texture_extents, true, "Renderer diagnostics",
                   "Reuse exact descriptor-derived texture extents while preserving every physical access probe")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_certified_diagnostics, false, "Renderer diagnostics",
                   "Measure cold snapshot phases; successful certificate hits remain clock-free")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_snapshot_workers, 2, "Renderer diagnostics",
                    "Optional helper threads for exact large native resource comparisons")
    // Keep default2 valid even if hardware_concurrency reports zero/<=2;
    // the pool still clamps the actual allocation to hardware_threads-2.
    .range(0,int32_t(std::max(2u,std::min(
        gow3::renderer::ParallelSnapshotCompare::HardwareHelperLimit(),
        unsigned(std::numeric_limits<int32_t>::max())))))
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_renderer_capture, "", "Renderer diagnostics",
                     "Record Gears D3D calls to this file; empty disables capture")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_renderer_history, "", "Renderer diagnostics",
                     "Capture matching initial GPU history into this directory")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_renderer_capture_start, 0, "Renderer diagnostics",
                     "First presented frame to record")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_renderer_capture_frames, 2, "Renderer diagnostics",
                     "Number of frames to record (1-120)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_renderer_capture_trigger, "", "Renderer diagnostics",
                     "Wait for this file before recording, so gameplay can be reached first")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
// Executable-specific addresses and device offsets: renderer/profiles/.
namespace profile = gow3::renderer::profile;
constexpr uint32_t kDeviceBytes = profile::device::kCaptureBytes;
constexpr size_t kCaptureLimit = 256 * 1024 * 1024;
constexpr uint32_t kPresent = profile::d3d::kPresent;
constexpr std::array<std::array<uint32_t,2>,9> kShaderBlockDiscoverySites{{
    {{0x823F7010,0x80FF0008}},{{0x823F7014,0x80DF0004}},
    {{0x823F701C,0x80BF0000}},{{0x823F7020,0x83E10050}},
    {{0x823F7024,0x7FE4FB78}},{{0x823F7028,0x4BF0B649}},
    {{0x823F702C,0xA09D0002}},{{0x82302670,0x7D8802A6}},
    {{0x82302724,0x38600001}},
}};
std::atomic<bool> enabled{false};
std::atomic<bool> live_enabled{false};
std::atomic<bool> command_spans_enabled{false};
gow3::renderer::GraphicsBoundarySink live_sink;
gow3::renderer::ShaderDiscoverySink shader_discovery_sink;
std::atomic<bool> shader_discovery_enabled{false};
std::atomic<bool> shader_block_discovery_enabled{false};
// Capture-lock protected, bounded content identities only. Never retain a guest
// pointer or a decoded block after the shader-cache loader resumes.
std::array<std::string,64> discovered_shader_blocks;
size_t next_discovered_shader_block=0;
struct ShaderBlockStatistics {
  uint64_t calls=0,duplicates=0,blocks=0,bytes=0,emitted=0,emitted_bytes=0,
      refused=0,limited=0,errors=0,prioritized=0,ineligible_requests=0;
  double milliseconds=0,maximum_ms=0;
} shader_block_stats;
// Epoch changes invalidate thread-local streaks across teardown/re-enable.
// Counters are observed only when disabling; they never trigger work.
std::atomic<uint64_t> query_poll_epoch{0};
std::atomic<uint64_t> query_poll_pending{0},query_poll_yields{0},query_poll_completed{0};
thread_local gow3::renderer::QueryPollYieldPolicy query_poll_policy;
std::atomic<bool> published_queries_enabled{false};
std::shared_ptr<gow3::renderer::PublishedQueryResults> published_queries;
std::atomic<uint64_t> published_query_accepts{0},published_query_checks{0},published_query_binds{0};
thread_local uint32_t published_query_retry_count=0;
thread_local bool published_query_gate_used=false;
std::atomic<uint64_t> published_query_successes{0},published_query_pending{0},published_query_errors{0};
std::atomic<bool> query_readiness_enabled{false};
Gow3QueryReadinessProbe query_readiness_probe;
std::atomic<uint64_t> readiness_samples{0},readiness_tracked{0},readiness_published_before{0},
    readiness_pending_after_published{0},readiness_completed_after_published{0},
    readiness_changed_during_call{0},readiness_errors{0};
thread_local uint32_t readiness_calls=0;
gow3::renderer::GetDataInlineSelection getdata_inline_selection;
void DisableQueryPollYield() {
  const auto previous=query_poll_epoch.fetch_and(~uint64_t{1},std::memory_order_acq_rel);
  if(previous&1)REXLOG_INFO("Native query poll yield snapshot: pending={}, yields={}, completed={}, interval={}",
      query_poll_pending.load(std::memory_order_relaxed),query_poll_yields.load(std::memory_order_relaxed),
      query_poll_completed.load(std::memory_order_relaxed),gow3::renderer::kQueryPollYieldInterval);
}
gow3::renderer::GraphicsBoundaryEvent* live_event = nullptr;
bool writing_capture_event = false;
bool thin_live_metadata_enabled=false,thin_live_metadata_reported=false;
gow3::renderer::LiveMetadataCaptureStatistics thin_live_metadata_stats;
void ReportThinLiveMetadata() {
  if(thin_live_metadata_reported||!thin_live_metadata_stats.omitted_blocks)return;
  thin_live_metadata_reported=true;
  const auto& s=thin_live_metadata_stats;
  REXLOG_INFO("Native thin live metadata: enabled=true omitted_blocks={} readable_copies_avoided={} copy_bytes_avoided={} unreadable_blocks={} kind11_copies={} kind11_bytes={} kind12_copies={} kind12_bytes={} kind13_copies={} kind13_bytes={}",
      s.omitted_blocks,s.readable_copies_avoided,s.copy_bytes_avoided,s.unreadable_blocks,
      s.copies_by_kind[0],s.bytes_by_kind[0],s.copies_by_kind[1],s.bytes_by_kind[1],
      s.copies_by_kind[2],s.bytes_by_kind[2]);
}
uint64_t live_sequence = 0;
uint64_t command_generation = 0;
thread_local uint64_t last_observed_sequence = UINT64_MAX;
struct CommandOwner {uint64_t sequence=UINT64_MAX;uint32_t device=0;};
thread_local CommandOwner command_owner;
thread_local std::optional<gow3::renderer::GraphicsCommandSpan> diagnostic_command_span;
thread_local std::array<uint32_t,8> diagnostic_command_args{};
bool Observing() { return enabled.load(std::memory_order_relaxed) || live_enabled.load(std::memory_order_relaxed); }
std::mutex capture_mutex;
gow3::renderer::ImmutableSnapshots live_snapshots;
bool warm_snapshot_readability_enabled=false;
// Texture snapshot owners certified at one tracker mutation generation. Any
// page invalidation, revocation or enable change advances the generation
// before the guarded write lands, so an unchanged value proves the bytes.
struct SnapshotGenerationMemo {
  bool enabled=false,verify=false;
  uint64_t generation=0;
  std::unordered_map<uint64_t,std::shared_ptr<const std::vector<uint8_t>>> owners;
  uint64_t hits=0,misses=0,stored=0,verified=0,mismatches=0;
  void Reset(){generation=0;owners.clear();}
} snapshot_memo;
bool capture_texture_extents_enabled=false;
gow3::renderer::CaptureTextureExtentCache capture_texture_extents;
gow3::renderer::BasicShaderInputCache<gow3::renderer::RuntimeShaderInputFingerprint> live_shader_inputs;
rex::memory::Memory* guest_memory = nullptr;

// Separate from capture_mutex. No waits, guest reads, callbacks, or ownership
// are available to the decoded lookup. Title hooks only observe heap metadata.
using MarkerAllocation=gow3::renderer::ObservedMarkerCounterAllocation;
struct MarkerObservation {
  MarkerAllocation value{};bool ring_reset=false;
  rex::memory::PhysicalAllocationObservations::Ptr allocation;
  uint32_t last_serial=0;bool emitted=false;
};
struct MarkerEmission {
  uint32_t source=0,device=0;std::array<uint32_t,8> words{};
  uint64_t generation=0,epoch=0;bool valid=false;
};
std::array<MarkerEmission,128> marker_emissions{};
size_t marker_emission_cursor=0;
std::atomic<bool> marker_submission_enabled{false};
struct ImmediateMarkerTargetGuard {
  std::unique_lock<std::recursive_mutex> memory_lock{rex::thread::global_critical_region::AcquireDirect()};
  std::unique_lock<std::mutex> observation_lock;
  explicit ImmediateMarkerTargetGuard(std::mutex& mutex):observation_lock(mutex){}
};
std::mutex marker_observation_mutex;
std::atomic<bool> marker_observation_enabled{false};
rex::memory::Memory* marker_observation_memory=nullptr;
std::array<MarkerObservation,4> marker_observations{};
uint64_t marker_registration_generation=0;
uint64_t marker_registrations=0,marker_revalidations=0,marker_invalidations=0,marker_refusals=0;

bool MarkerAllocationFacts(uint32_t device,uint32_t counter,MarkerAllocation& out) {
  auto* memory=marker_observation_memory;
  if(!memory||!device||!counter||uint64_t(counter)+96>(uint64_t{1}<<32))return false;
  auto* alias=memory->LookupHeap(counter);
  if(!alias||alias->heap_type()!=rex::memory::HeapType::kGuestPhysical||
     alias!=memory->LookupHeap(counter+95))return false;
  const auto physical=memory->GetPhysicalAddress(counter);
  if(physical==UINT32_MAX||uint64_t(physical)+96>0x20000000ull||
     memory->GetPhysicalAddress(counter+95)!=physical+95)return false;
  auto* parent=memory->GetPhysicalHeap();
  rex::memory::HeapAllocationInfo a{},z{},p{},q{};
  if(!alias->QueryRegionInfo(counter,&a)||!alias->QueryRegionInfo(counter+95,&z)||
     !parent->QueryRegionInfo(physical,&p)||!parent->QueryRegionInfo(physical+95,&q))return false;
  const auto valid=[](const auto& info,uint32_t address) {
    return (info.state&rex::memory::kMemoryAllocationCommit)&&
        (info.protect&rex::memory::kMemoryProtectWrite)&&info.allocation_size&&
        info.allocation_base<=address&&uint64_t(address)<uint64_t(info.allocation_base)+info.allocation_size;
  };
  if(!valid(a,counter)||!valid(z,counter+95)||!valid(p,physical)||!valid(q,physical+95)||
     a.allocation_base!=z.allocation_base||a.allocation_size!=z.allocation_size||
     p.allocation_base!=q.allocation_base||p.allocation_size!=q.allocation_size||
     a.allocation_size!=p.allocation_size||memory->GetPhysicalAddress(a.allocation_base)!=p.allocation_base)return false;
  out={device,counter,physical,p.allocation_base,p.allocation_size,96,0,0,true,true};return true;
}
void ObserveMarkerRegistration(uint32_t device,uint32_t counter,bool ring_ready) noexcept {
  if(!marker_observation_enabled.load(std::memory_order_acquire))return;
  try {
    auto memory_lock=rex::thread::global_critical_region::AcquireDirect();
    std::lock_guard lock(marker_observation_mutex);
    if(!marker_observation_enabled.load(std::memory_order_relaxed))return;
    MarkerObservation* slot=nullptr;
    for(auto& observed:marker_observations)if(observed.value.device==device){slot=&observed;break;}
    if(ring_ready&&(!slot||!slot->ring_reset||slot->value.virtual_counter!=counter||
        !slot->allocation||!slot->allocation->current())) {++marker_refusals;return;}
    MarkerAllocation next;
    if(!MarkerAllocationFacts(device,counter,next)) {
      if(slot){slot->value.live=false;slot->ring_reset=false;}++marker_refusals;return;
    }
    if(ring_ready) {
      // A reset reopens only the same allocator observation; an overlapping
      // free/protect/recommit invalidates its token and cannot be reauthorized.
      const auto& old=slot->value;
      if(next.physical_counter!=old.physical_counter||next.allocation_base!=old.allocation_base||
         next.allocation_bytes!=old.allocation_bytes) {slot->ring_reset=false;++marker_refusals;return;}
      next.registration_generation=old.registration_generation;next.command_epoch=old.command_epoch;
      slot->value=next;slot->ring_reset=false;slot->emitted=false;++marker_revalidations;return;
    }
    if(marker_registration_generation==UINT64_MAX){marker_observation_enabled.store(false);return;}
    if(!slot)for(auto& observed:marker_observations)if(!observed.value.live){slot=&observed;break;}
    if(!slot){++marker_refusals;return;}
    auto allocation=marker_observation_memory->ObservePhysicalAllocation(next.physical_counter,96);
    if(!allocation||allocation->address!=next.allocation_base||allocation->bytes!=next.allocation_bytes) {
      ++marker_refusals;return;
    }
    next.registration_generation=++marker_registration_generation;next.command_epoch=1;
    *slot={next,false,std::move(allocation),0,false};++marker_registrations;
  } catch(...) {marker_observation_enabled.store(false,std::memory_order_release);}
}
void InvalidateMarkerObservation(uint32_t device,bool ring_reset) noexcept {
  if(!marker_observation_enabled.load(std::memory_order_acquire))return;
  try {
    std::lock_guard lock(marker_observation_mutex);
    for(auto& observed:marker_observations)if(observed.value.device==device) {
      observed.value.live=false;observed.ring_reset=false;++marker_invalidations;
      for(auto& emission:marker_emissions)if(emission.device==device)emission.valid=false;
      if(observed.value.command_epoch==UINT64_MAX){marker_observation_enabled.store(false);return;}
      ++observed.value.command_epoch;observed.ring_reset=ring_reset;return;
    }
  } catch(...) {marker_observation_enabled.store(false,std::memory_order_release);}
}
std::ofstream capture_file;
std::vector<uint8_t> capture_bytes;
uint32_t frame = 0, first_frame = 0, frame_count = 0, events = 0;
struct SnapshotMetadataObservation {
  gow3::renderer::ImmutableSnapshots::MetadataEviction eviction;
  uint64_t sequence=0,steady_ns=0,utc_ns=0;
  uint32_t frame=0;
  bool exact_event=false;
};
// Last64 metadata events only; no strings, byte owners or logging on the
// producer/fault path. Both clocks run only when metadata actually evicts.
std::array<SnapshotMetadataObservation,64> snapshot_metadata_observations{};
uint64_t snapshot_metadata_observed=0;
bool snapshot_metadata_reported=false;
void ObserveSnapshotMetadata(const gow3::renderer::ImmutableSnapshots::MetadataEviction& eviction)noexcept {
  auto& observation=snapshot_metadata_observations[snapshot_metadata_observed++%snapshot_metadata_observations.size()];
  observation.eviction=eviction;
  observation.sequence=live_event?live_event->sequence:live_sequence;
  observation.frame=live_event?live_event->frame:frame;
  observation.exact_event=live_event!=nullptr;
  observation.steady_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  observation.utc_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
void ReportSnapshotMetadata() {
  if(snapshot_metadata_reported)return;
  snapshot_metadata_reported=true;
  const auto count=std::min<uint64_t>(snapshot_metadata_observed,snapshot_metadata_observations.size());
  if(count)REXLOG_INFO("Native snapshot metadata history: total={} retained={} overwritten={}",
      snapshot_metadata_observed,count,snapshot_metadata_observed-count);
  for(uint64_t i=snapshot_metadata_observed-count;i<snapshot_metadata_observed;++i) {
    const auto& o=snapshot_metadata_observations[i%snapshot_metadata_observations.size()];
    REXLOG_INFO("Native snapshot metadata event: ordinal={} frame={} sequence={} exact_event={} steady_ns={} utc_ns={} metadata_lru={} address={} requested_bytes={} entries_before={} retained_bytes_before={}",
        i,o.frame,o.sequence,o.exact_event,o.steady_ns,o.utc_ns,o.eviction.lru,o.eviction.address,
        o.eviction.requested_bytes,o.eviction.entries,o.eviction.retained_bytes);
  }
}
rex::graphics::CommandProcessor* capture_processor=nullptr;
std::filesystem::path history_directory;
std::filesystem::path trigger_file;
bool waiting_for_trigger = false;
struct CapturedBlob { uint32_t offset, size; };
std::unordered_multimap<size_t, CapturedBlob> blobs;
struct CaptureFull {};
struct PendingUp {
  uint32_t function = 0, lr = 0, result = 0;
  std::array<uint32_t, 8> args{};
};
thread_local PendingUp pending_up;

void Word(uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) capture_bytes.push_back(v >> shift);
}

uint32_t BigWord(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return std::endian::native == std::endian::little ? std::byteswap(v) : v;
}

bool Readable(uint32_t address, uint32_t size) {
  if (!guest_memory || !address || !size || uint64_t(address) + size > (uint64_t{1} << 32))
    return false;
  const uint32_t end = address + size - 1;
  auto* heap = guest_memory->LookupHeap(address);
  if (!heap || heap != guest_memory->LookupHeap(end)) return false;
  return (int(heap->QueryRangeAccess(address, end)) & int(rex::memory::PageAccess::kReadOnly)) != 0;
}

// Blocks retain guest byte order. Record headers are little endian. Missing
// memory is explicit (size zero), never substituted with invented zero data.
void DataBlock(uint32_t kind, uint32_t address, const uint8_t* data, uint32_t size,bool physical=false) {
  if (live_event) {
    auto& block = live_event->blocks.emplace_back();
    block.kind = kind; block.address = address;
    if (physical&&size>=4096)block.shared_bytes=live_snapshots.Get(address,{data,size});
    else if (size) {
      if (size == gow3::renderer::LazyDeviceState::kBytes) gow3::renderer::DeviceStateBufferPool::Take(block.bytes);
      block.bytes.assign(data, data + size);
    }
  }
  if (!writing_capture_event) return;
  if (capture_bytes.size() + 12 + 16 > kCaptureLimit) throw CaptureFull{};
  Word(kind); Word(address);
  if (!size) { Word(0); return; }
  const auto hash = std::hash<std::string_view>{}({reinterpret_cast<const char*>(data), size});
  const auto [begin, end] = blobs.equal_range(hash);
  for (auto it = begin; it != end; ++it) {
    const auto blob = it->second;
    if (blob.size == size && !std::memcmp(capture_bytes.data() + blob.offset, data, size)) {
      // Version 2: a high-bit length references an earlier inline block's data
      // offset. Compare bytes as well as hashes, including mutable buffers.
      Word(0x80000000u | blob.offset);
      return;
    }
  }
  if (capture_bytes.size() + 4 + size + 16 > kCaptureLimit) throw CaptureFull{};
  Word(size);
  blobs.emplace(hash, CapturedBlob{uint32_t(capture_bytes.size()), size});
  capture_bytes.insert(capture_bytes.end(), data, data + size);
}

void Block(uint8_t* base, uint32_t kind, uint32_t address, uint32_t size) {
  if (!Readable(address, size)) size = 0;
  DataBlock(kind, address, size ? base + address : nullptr, size);
}

void MetadataBlock(uint8_t* base,uint32_t kind,uint32_t address,uint32_t size,
                   const gow3::renderer::LiveMetadataCapturePolicy& policy) {
  if(!policy.Omit(kind)){Block(base,kind,address,size);return;}
  // Preserve the original access probe, kind and address. Only a copy which
  // cannot affect live execution is removed; malformed/unreadable memory is
  // still represented explicitly by an empty block.
  thin_live_metadata_stats.Record(kind,size,Readable(address,size));
  DataBlock(kind,address,nullptr,0);
}

void PhysicalBlock(uint32_t kind, uint32_t address, uint32_t size) {
  if (!size || size > 8 * 1024 * 1024 || uint64_t(address) + size > 0x20000000ull ||
      !(int(guest_memory->GetPhysicalHeap()->QueryRangeAccess(address, address + size - 1)) &
        int(rex::memory::PageAccess::kReadOnly))) size = 0;
  if (size && live_event && live_event->materialize_texture && kind>=32 && kind<=95) {
    auto& block=live_event->blocks.emplace_back();
    block.kind=kind;block.address=address;block.deferred_physical_bytes=size;
    return;
  }
  DataBlock(kind, address, size ? guest_memory->TranslatePhysical(address) : nullptr, size,true);
}

std::shared_ptr<const std::vector<uint8_t>> CaptureDeferredTexture(uint32_t address,uint32_t size) {
  // Ownership may have changed while ordered CPU invalidations were drained.
  // Recheck access before taking a snapshot on the same D3D caller/bridge lock.
  if (!(int(guest_memory->GetPhysicalHeap()->QueryRangeAccess(address,address+size-1))&
        int(rex::memory::PageAccess::kReadOnly)))return {};
  const auto* bytes=guest_memory->TranslatePhysical(address);
  if(size>=4096)return live_snapshots.Get(address,{bytes,size});
  return std::make_shared<const std::vector<uint8_t>>(bytes,bytes+size);
}

std::vector<gow3::renderer::DeferredTextureSnapshotScope::Bytes> CaptureDeferredTexturesFull(
    std::span<const gow3::renderer::DeferredTextureSnapshotScope::Request> requests);

std::vector<gow3::renderer::DeferredTextureSnapshotScope::Bytes> CaptureDeferredTextures(
    std::span<const gow3::renderer::DeferredTextureSnapshotScope::Request> requests) {
  auto& memo=snapshot_memo;
  if(!memo.enabled)return CaptureDeferredTexturesFull(requests);
  // Read before any validation: a mutation after this read advances past it.
  const uint64_t generation=guest_memory->CertifiedPhysicalSnapshotGeneration();
  auto key=[](const auto& request){return uint64_t(request.address)<<32|request.bytes;};
  if(generation&&generation==memo.generation) {
    std::vector<gow3::renderer::DeferredTextureSnapshotScope::Bytes> out(requests.size());
    bool complete=true;
    for(size_t i=0;i<requests.size()&&complete;++i) {
      const auto found=memo.owners.find(key(requests[i]));
      if(found==memo.owners.end())complete=false;else out[i]=found->second;
    }
    if(complete) {
      ++memo.hits;
      if(memo.verify) {
        const auto full=CaptureDeferredTexturesFull(requests);++memo.verified;
        for(size_t i=0;i<requests.size();++i)
          if(!full[i]||full[i]->size()!=out[i]->size()||
             !std::equal(full[i]->begin(),full[i]->end(),out[i]->begin()))++memo.mismatches;
      }
      return out;
    }
  }
  ++memo.misses;
  auto out=CaptureDeferredTexturesFull(requests);
  if(!generation)return out;
  if(generation!=memo.generation){memo.owners.clear();memo.generation=generation;}
  if(memo.owners.size()>=4096)memo.owners.clear();
  for(size_t i=0;i<requests.size();++i)
    if(out[i]&&requests[i].bytes>=4096&&
       live_snapshots.CertifiedActive(requests[i].address,requests[i].bytes,out[i])) {
      memo.owners[key(requests[i])]=out[i];++memo.stored;
    }
  return out;
}

std::vector<gow3::renderer::DeferredTextureSnapshotScope::Bytes> CaptureDeferredTexturesFull(
    std::span<const gow3::renderer::DeferredTextureSnapshotScope::Request> requests) {
  using Snapshots=gow3::renderer::ImmutableSnapshots;
  if(warm_snapshot_readability_enabled&&requests.size()<=64) {
    // These are descriptors, not borrowed live pointers. Every accepted member
    // receives fresh all-alias/readability validation from its exact SDK token.
    // Any refusal executes the original WHOLE path below: unreadable members
    // are excluded before its cohort/admission policy is calculated.
    std::array<Snapshots::WarmRange,64> ranges{};
    for(size_t i=0;i<requests.size();++i)ranges[i]={requests[i].address,requests[i].bytes};
    if(auto reused=live_snapshots.TryReuseReadableBatch(
        std::span<const Snapshots::WarmRange>(ranges).first(requests.size())))return std::move(*reused);
  }
  std::vector<gow3::renderer::DeferredTextureSnapshotScope::Bytes> out(requests.size());
  std::vector<Snapshots::View> views;views.reserve(requests.size());
  std::vector<size_t> indices;indices.reserve(requests.size());
  for(size_t i=0;i<requests.size();++i) {
    const auto& request=requests[i];
    // Same readable probe as the original lazy capture, after invalidations
    // and original GPU-only classification. No unused/GPU-only view reaches us.
    if(!(int(guest_memory->GetPhysicalHeap()->QueryRangeAccess(request.address,request.address+request.bytes-1))&
         int(rex::memory::PageAccess::kReadOnly)))continue;
    const auto* data=guest_memory->TranslatePhysical(request.address);
    if(request.bytes<4096)out[i]=std::make_shared<const std::vector<uint8_t>>(data,data+request.bytes);
    else {views.push_back({request.address,{data,request.bytes}});indices.push_back(i);}
  }
  if(!views.empty()) {
    auto owners=live_snapshots.GetBatch(views);
    for(size_t i=0;i<owners.size();++i)out[indices[i]]=std::move(owners[i]);
  }
  return out;
}

void Finish(uint32_t reason) {
  if (!enabled.exchange(false)) return;
  // Footer: address 0, followed by finish reason (0=frames, 1=limit, 2=shutdown).
  Word(16); Word(0); Word(reason); Word(events);
  capture_file.write(reinterpret_cast<const char*>(capture_bytes.data()), capture_bytes.size());
  capture_file.close();
  REXLOG_INFO("Gears D3D boundary capture: {} events, {} bytes, finish reason {} (reference renderer)",
              events, capture_bytes.size(), reason);
  if (capture_file.fail()) REXLOG_ERROR("Failed writing Gears D3D boundary capture");
  capture_bytes.clear();
  blobs.clear();
}

void GeometryBlocks(uint8_t* base, const uint8_t* device, uint32_t function,
                    const std::array<uint32_t, 8>& args, uint32_t phase,
                    const gow3::renderer::ShaderRequirementMasks* required = nullptr) {
  const bool draw = (phase == 1||phase==3) && (function == profile::d3d::kDrawVertices || function == profile::d3d::kDrawIndexedVertices);
  for (uint32_t stream = 0; stream < 16; ++stream) {
    // Gears binds its sixteen streams to fetch constants 95 down to 80.
    // Gameplay capture 05 uses 95..92; do not interpret texture words as VBs.
    const uint32_t offset = profile::device::kFetchConstants + (95 - stream) * 8;
    const uint32_t first = BigWord(device + offset), second = BigWord(device + offset + 4);
    const uint32_t size = draw && (!required || required->HasVertex(95-stream)) &&
        (first & 3) == 3 ? ((second >> 2) & 0xFFFFFF) * 4 : 0;
    PhysicalBlock(14 + stream, first & ~3u, size);
  }
  const uint32_t index = BigWord(device + profile::device::kIndexBuffer);
  const bool indexed = draw && function == profile::d3d::kDrawIndexedVertices && Readable(index, 64);
  Block(base, 30, index, indexed ? 64 : 0);
  uint32_t physical = 0, size = 0;
  if (indexed) {
    const uint32_t width = (BigWord(base + index) & 0x80000000u) ? 4 : 2;
    const uint64_t start = uint64_t(BigWord(base + index + 0x18)) + uint64_t(args[2]) * width;
    const uint64_t bytes = uint64_t(args[3]) * width;
    if (start <= UINT32_MAX && bytes <= 8 * 1024 * 1024) {
      physical = guest_memory->GetPhysicalAddress(uint32_t(start));
      size = uint32_t(bytes);
    }
  }
  PhysicalBlock(31, physical, size);
}

void TextureBlocks(const uint8_t* device, uint32_t function, uint32_t phase,
                   const gow3::renderer::ShaderRequirementMasks* required = nullptr) {
  const bool draw = ((phase == 1||phase==3) && (function == profile::d3d::kDrawVertices || function == profile::d3d::kDrawIndexedVertices)) ||
                    (phase == 2 && (function == profile::d3d::kBeginVertices || function == profile::d3d::kBeginIndexedVertices));
  for (uint32_t slot = 0; slot < 32; ++slot) {
    std::array<uint32_t,6> fetch{};
    for (uint32_t word = 0; word < fetch.size(); ++word)
      fetch[word] = BigWord(device + profile::device::kFetchConstants + slot * 24 + word * 4);
    gow3::renderer::CaptureTextureExtents extents;
    if (draw && (!required || required->HasTexture(slot)) && (fetch[0] & 3) == 2) {
      extents = capture_texture_extents_enabled ?
          capture_texture_extents.Get(slot,fetch,gow3::renderer::DescribeCaptureTextureExtents) :
          gow3::renderer::DescribeCaptureTextureExtents(fetch);
    }
    // Pure fetch metadata never certifies guest readability or byte contents.
    // Keep both original physical probes at their original observation point.
    PhysicalBlock(32 + slot * 2, extents.base << 12, extents.base_size);
    PhysicalBlock(33 + slot * 2, extents.mips << 12, extents.mip_size);
  }
}

void ShaderBlocks(uint8_t* base, uint32_t address, bool pixel) {
  const uint32_t offset = pixel ? 0x28 : 0x368;
  uint32_t size = 0, physical_size = 0, physical = 0;
  if (Readable(address, offset + 36)) {
    const auto* container = base + address + offset;
    const uint32_t flags = BigWord(container);
    // 82AF9688 marks linked vertex containers with bit 6; bit 5 denotes
    // the additional vertex program variant handled by 82AF2910.
    // Bit 4 occurs on the startup vertex-only point draw. Preserve its bytes
    // for the native planner to validate rather than disguising this program
    // as a missing shader; its execution mode is validated separately.
    const uint32_t allowed = pixel ? 0u : 0x71u;
    if ((flags & ~allowed) == 0x102A1100u && bool(flags & 1) != pixel) {
      size = BigWord(container + 4);
      physical_size = BigWord(container + 8);
      physical = BigWord(base + address + (pixel ? 0x18 : 0x20));
      // Do not trust size fields from an as-yet-unverified runtime ABI.
      if (size < 36 || size > 128 * 1024) size = 0;
      if (physical_size > 128 * 1024) physical_size = 0;
    }
  }
  Block(base, pixel ? 4 : 2, address + offset, size);
  Block(base, pixel ? 5 : 3, physical, physical_size);
}


void DiscoverShader(uint8_t* base,uint32_t object,bool pixel) noexcept {
  if(!shader_discovery_enabled.load(std::memory_order_relaxed))return;
  try {
    std::lock_guard lock(capture_mutex);
    if(!shader_discovery_sink||!guest_memory)return;
    auto snapshot=gow3::renderer::SnapshotDiscoveredShader(object,pixel,
        [&](uint32_t address,uint32_t size)->std::span<const uint8_t> {
          if(!Readable(address,size))return {};
          return {base+address,size};
        });
    if(snapshot)shader_discovery_sink(std::move(snapshot),pixel,false);
  }catch(const std::exception& error) {
    // Speculation must not change guest calls, native ownership, or fallback.
    REXLOG_WARN("Native shader discovery skipped: {}",error.what());
  }catch(...) {}
}

void DiscoverShaderBlock(uint8_t* base,uint32_t address,uint32_t bytes,uint32_t selected) noexcept {
  if(!shader_block_discovery_enabled.load(std::memory_order_relaxed))return;
  try {
    std::lock_guard lock(capture_mutex);
    if(!shader_discovery_sink||!guest_memory)return;
    ++shader_block_stats.calls;
    if(!bytes||bytes>gow3::renderer::kMaxShaderDiscoveryBlockBytes||!Readable(address,bytes)) {
      ++shader_block_stats.refused;return;
    }
    const auto began=std::chrono::steady_clock::now();
    const std::span<const uint8_t> block(base+address,bytes);
    if(!Readable(selected,8)) {++shader_block_stats.ineligible_requests;return;}
    const auto* entry=base+selected;
    const uint32_t count=uint32_t(entry[2])<<8|entry[3],offset=BigWord(entry+4);
    if(count<36||offset>bytes||count>bytes-offset||BigWord(block.data()+offset)!=0x102A1100u||
       BigWord(block.data()+offset+16)!=0||
       uint64_t(BigWord(block.data()+offset+4))+BigWord(block.data()+offset+8)!=count) {
      // Do not start unrelated pixel jobs ahead of an immediately needed
      // vertex shader, whose final linked instructions are not available yet.
      ++shader_block_stats.ineligible_requests;return;
    }
    const auto selected_source=block.subspan(offset,count);
    if(gow3::renderer::shader_block_discovery_detail::PixelContainerBytes(selected_source)!=count) {
      ++shader_block_stats.ineligible_requests;return;
    }
    auto hash=gow3::renderer::ContentHash(block);
    if(std::find(discovered_shader_blocks.begin(),discovered_shader_blocks.end(),hash)!=discovered_shader_blocks.end()) {
      ++shader_block_stats.duplicates;
    } else {
      // This caller's nonvolatile R29 points at {uint16 block, uint16 size,
      // uint32 offset}. Put its immediate pixel shader ahead of speculation.
      // The decoder does not mutate that metadata and has not returned to its
      // caller yet, so the selected range and temporary block are both alive.
      shader_discovery_sink(std::make_shared<const std::vector<uint8_t>>(
          selected_source.begin(),selected_source.end()),true,false);
      ++shader_block_stats.prioritized;
      const auto result=gow3::renderer::DiscoverDecodedPixelShaders(block,[](auto source) {
        shader_discovery_sink(std::move(source),true,true);
      });
      ++shader_block_stats.blocks;shader_block_stats.bytes+=bytes;
      shader_block_stats.emitted+=result.emitted;
      shader_block_stats.emitted_bytes+=result.emitted_bytes;
      shader_block_stats.refused+=result.refused;
      shader_block_stats.limited+=result.limit_reached;
      discovered_shader_blocks[next_discovered_shader_block]=std::move(hash);
      next_discovered_shader_block=(next_discovered_shader_block+1)%discovered_shader_blocks.size();
    }
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    shader_block_stats.milliseconds+=elapsed;
    shader_block_stats.maximum_ms=std::max(shader_block_stats.maximum_ms,elapsed);
  }catch(...) {
    // Speculation does not alter the completed decompression or original RHI
    // initialization. Actual draw compilation retains its exact validation.
    try {std::lock_guard lock(capture_mutex);++shader_block_stats.errors;}catch(...) {}
  }
}

void Observe(uint32_t function, PPCContext& ctx, uint8_t* base, uint32_t phase = 0,
             const std::array<uint32_t, 8>* original = nullptr, uint32_t original_lr = 0,
             std::optional<gow3::renderer::GraphicsCommandSpan> span = {}) {
  last_observed_sequence=UINT64_MAX;
  if (!Observing()) return;
  std::lock_guard lock(capture_mutex);
  if (!Observing()) return;
  if (enabled.load(std::memory_order_relaxed) && waiting_for_trigger &&
      function == kPresent && phase == 0) {
    std::error_code error;
    if (std::filesystem::is_regular_file(trigger_file, error)) {
      waiting_for_trigger = false;
      first_frame = frame + (history_directory.empty() ? 1 : 2);
      if (capture_processor && !history_directory.empty())
        capture_processor->RequestFrameTraceAtFrame(history_directory, first_frame);
      REXLOG_INFO("Gears renderer capture triggered at frame {}", first_frame);
    }
  }
  writing_capture_event = phase!=3 && enabled.load(std::memory_order_relaxed) && !waiting_for_trigger &&
      frame >= first_frame && frame - first_frame < frame_count;
  const bool chunk_mode=command_spans_enabled.load(std::memory_order_relaxed);
  const bool draw = ((phase == 3||(phase==1&&!chunk_mode)) && (function == profile::d3d::kDrawVertices || function == profile::d3d::kDrawIndexedVertices)) ||
      (phase == 2 && (function == profile::d3d::kBeginVertices || function == profile::d3d::kBeginIndexedVertices));
  const bool observe_live = live_sink && (draw || (phase == 0 &&
      (function == kPresent || function == profile::d3d::kClearRect || function == profile::d3d::kResolve || function == profile::d3d::kSpecializedDraw)));
  if (writing_capture_event || observe_live) {
    std::array<uint32_t, 8> args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                               ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
    if (original) args = *original;
    gow3::renderer::GraphicsBoundaryEvent event;
    std::optional<gow3::renderer::DeferredTextureSnapshotScope> snapshot_scope;
    if (observe_live) {
      event.sequence = live_sequence++; event.function = function; event.phase = phase==3?1:phase;
      last_observed_sequence=event.sequence;
      if(span) {span->generation=++command_generation;span->api_sequence=event.sequence;event.command_span=std::move(span);}
      event.frame = frame; event.lr = original ? original_lr : ctx.lr;
      event.args = args; event.result = ctx.r3.u32; event.blocks.reserve(103);
      // Full API diagnostics remain eager and byte-complete. Live-only texture
      // reads are decided after the planner drains current CPU invalidations.
      if(!writing_capture_event) {
        snapshot_scope.emplace();
        event.materialize_texture=snapshot_scope->Bind(CaptureDeferredTexture);
        if(live_snapshots.batch_stats().enabled)
          event.materialize_textures=snapshot_scope->BindBatch(CaptureDeferredTextures);
      }
      live_event = &event;
    }
    const auto start = capture_bytes.size();
    try {
      if (writing_capture_event) {
        Word(0); Word(function); Word(phase); Word(frame); Word(original ? original_lr : ctx.lr);
        for (auto value : args) Word(value);
        Word(ctx.r3.u32); Word(103);
      }
      Block(base, 1, args[0], kDeviceBytes);
      if (Readable(args[0], kDeviceBytes)) {
        const auto* device = base + args[0];
        ShaderBlocks(base, BigWord(device + profile::device::kVertexShader), false);
        ShaderBlocks(base, BigWord(device + profile::device::kPixelShader), true);
        const auto required = draw && live_event && !writing_capture_event
            ? live_shader_inputs.Get(live_event->blocks) : std::nullopt;
        for (uint32_t i = 0; i < 5; ++i)
          Block(base, 6 + i, BigWord(device + profile::device::kSurfaces + i * 4), 64);
        bool vertex_header=false,vertex_code=false;
        if(thin_live_metadata_enabled&&required)for(const auto& block:live_event->blocks) {
          if(block.kind==2)vertex_header=!block.data().empty();
          if(block.kind==3)vertex_code=!block.data().empty();
        }
        const gow3::renderer::LiveMetadataCapturePolicy metadata_policy{
            thin_live_metadata_enabled,live_event!=nullptr,writing_capture_event,
            draw&&required.has_value()&&vertex_header&&vertex_code};
        MetadataBlock(base,11,BigWord(device+profile::device::kVertexShader),0x600,metadata_policy);
        MetadataBlock(base,12,BigWord(device+profile::device::kPixelShader),0x100,metadata_policy);
        MetadataBlock(base,13,BigWord(device+profile::device::kShaderMetadata),0x200,metadata_policy);
        GeometryBlocks(base, device, function, args, phase, required ? &*required : nullptr);
        TextureBlocks(device, function, phase, required ? &*required : nullptr);
        const bool up = phase == 2 && (function == profile::d3d::kBeginVertices || function == profile::d3d::kBeginIndexedVertices);
        const uint32_t vertices = up ? BigWord(device + profile::device::kUpVertexWords) : 0;
        const uint32_t indices = up && function == profile::d3d::kBeginIndexedVertices ? BigWord(device + profile::device::kUpIndexWords) : 0;
        Block(base, 96, BigWord(device + profile::device::kUpVertexPointer), vertices <= 2 * 1024 * 1024 ? vertices * 4 : 0);
        Block(base, 97, BigWord(device + profile::device::kUpIndexPointer), indices <= 2 * 1024 * 1024 ? indices * 4 : 0);
        const bool clear = function == profile::d3d::kClearRect && phase == 0;
        const bool resolve = function == profile::d3d::kResolve && phase == 0;
        Block(base, 98, args[2], clear || resolve ? 16 : 0); // Source/clear rectangle.
        Block(base, 99, clear ? args[3] : args[6], clear || resolve ? 16 : 0); // Float clear color.
        Block(base, 100, args[3], resolve ? 64 : 0); // Destination texture resource header.
        Block(base, 101, args[4], resolve ? 8 : 0); // Destination XY.
        std::array<uint8_t, 12> extras{};
        if (clear || resolve) {
          const uint64_t f1 = ctx.f1.u64;
          for (uint32_t i = 0; i < 8; ++i) extras[i] = uint8_t(f1 >> ((7 - i) * 8));
          // Resolve's final stack parameter is used by its optional clear.
          if (resolve && Readable(ctx.r1.u32 + 0x54, 4))
            std::memcpy(extras.data() + 8, base + ctx.r1.u32 + 0x54, 4);
        }
        DataBlock(102, 0, extras.data(), clear || resolve ? uint32_t(extras.size()) : 0);
        Block(base,103,args[1],function==kPresent&&phase==0?64:0);
      } else {
        for (uint32_t kind = 2; kind <= 103; ++kind) DataBlock(kind, 0, nullptr, 0);
      }
      if (writing_capture_event) {
        uint32_t size = capture_bytes.size() - start;
        for (int i = 0; i < 4; ++i) capture_bytes[start + i] = size >> (8 * i);
        ++events;
        if (capture_bytes.size() >= kCaptureLimit - 1024 * 1024) Finish(1);
      }
    } catch (const CaptureFull&) {
      capture_bytes.resize(start);
      Finish(1);
      // A diagnostic size limit must not feed a truncated live event.
      if (observe_live) {
        REXLOG_ERROR("Native renderer event interrupted by diagnostic capture limit");
        getdata_inline_selection.Reset(); live_sink = {}; live_enabled.store(false); DisableQueryPollYield();
      }
    }
    live_event = nullptr;
    writing_capture_event = false;
    if (observe_live && live_sink) {
      try { live_sink(std::move(event)); }
      catch (const std::exception& error) {
        REXLOG_ERROR("Native graphics boundary sink failed: {}", error.what());
        getdata_inline_selection.Reset(); live_sink = {}; live_enabled.store(false); DisableQueryPollYield();
      }
    }
  }
  writing_capture_event = false;
  if (function == kPresent && phase == 0) {
    ++frame;
    if (enabled.load(std::memory_order_relaxed) && !waiting_for_trigger &&
        frame >= first_frame && frame - first_frame >= frame_count) Finish(0);
  }
}

} // namespace

void Gow3StartGraphicsBridge(rex::memory::Memory* memory,rex::graphics::CommandProcessor* processor) {
  getdata_inline_selection.Reset();
  std::lock_guard lock(capture_mutex);
  getdata_inline_selection.Reset();
  live_snapshots.Clear();snapshot_memo.Reset();
  live_shader_inputs.ConfigureFingerprint({REXCVAR_GET(gow3_native_fast_snapshot_fingerprints)});
  guest_memory = memory;
  {
    std::lock_guard marker_lock(marker_observation_mutex);
    marker_observation_enabled.store(false,std::memory_order_release);
    marker_submission_enabled.store(false,std::memory_order_release);
    marker_emissions={};marker_emission_cursor=0;
    marker_observations={};marker_observation_memory=memory;
    marker_registrations=marker_revalidations=marker_invalidations=marker_refusals=0;
    if(memory&&((REXCVAR_GET(gpu_command_summary)&&REXCVAR_GET(gpu_native_marker_operands))||
               REXCVAR_GET(gpu_native_submit_completion_markers))) {
      constexpr std::array<std::array<uint32_t,2>,7> sites{{
        {0x82B03C64,0x3C809580},{0x82B03E48,0x7D8802A6},{0x82AF5F10,0x7D8802A6},
        {0x82AF6180,0x916A0004},{0x82B08120,0x7D8802A6},{0x82AF55BC,0x90CA0000},
        {0x82AF559C,0x94C30004}}};
      bool matched=true;for(const auto& site:sites)matched&=BigWord(memory->virtual_membase()+site[0])==site[1];
      marker_observation_enabled.store(matched,std::memory_order_release);
      marker_submission_enabled.store(matched&&REXCVAR_GET(gpu_native_submit_completion_markers),std::memory_order_release);
      REXLOG_INFO("Native marker title observation: enabled={} lifetime_authenticated=false allocator_mutations_observed=true immediate_pair_guard={}",matched,marker_submission_enabled.load());
    }
  }
  thin_live_metadata_enabled=REXCVAR_GET(gow3_native_thin_live_metadata);
  thin_live_metadata_stats={};thin_live_metadata_reported=false;
  capture_processor = processor;
  frame = events = 0;
  live_sequence = 0;
  command_generation = 0;
  auto path = REXCVAR_GET(gow3_renderer_capture);
  if (path.empty() && !live_sink) return;
  capture_processor=processor;history_directory=REXCVAR_GET(gow3_renderer_history);
  if(!history_directory.empty()&&!processor)throw std::runtime_error("GPU history requires a command processor");
  for (const auto& site : gow3::renderer::kUpCommitSites) {
    if (BigWord(memory->virtual_membase() + site[0]) != site[1])
      throw std::runtime_error("UP completion hook instruction mismatch; renderer observation disabled");
  }
  if(command_spans_enabled.load())for(const auto& site:gow3::renderer::kPacketCommitSites)
    if(BigWord(memory->virtual_membase()+site[0])!=site[1])
      throw std::runtime_error("Command ownership hook instruction mismatch");
  if (path.empty()) return;
  const auto count = REXCVAR_GET(gow3_renderer_capture_frames);
  if (!count || count > 120) throw std::runtime_error("Renderer capture frames must be 1-120");
  trigger_file = REXCVAR_GET(gow3_renderer_capture_trigger);
  waiting_for_trigger = !trigger_file.empty();
  if (waiting_for_trigger && std::filesystem::exists(trigger_file))
    throw std::runtime_error("Renderer capture trigger already exists; use a new trigger path");
  // Never overwrite a previous capture by accident.
  if (std::filesystem::exists(path)) throw std::runtime_error("Renderer capture file already exists: " + path);
  capture_file.open(path, std::ios::binary | std::ios::out);
  if (!capture_file) throw std::runtime_error("Cannot open renderer capture: " + path);
  guest_memory = memory;
  first_frame = REXCVAR_GET(gow3_renderer_capture_start);
  if(!waiting_for_trigger&&!history_directory.empty()) {
    if(first_frame<2)first_frame=2;
    capture_processor->RequestFrameTraceAtFrame(history_directory,first_frame);
  }
  frame_count = count;
  frame = events = 0;
  capture_bytes.clear();
  blobs.clear();
  capture_bytes.reserve(kCaptureLimit);
  constexpr std::array<uint8_t, 8> magic{'G','3','D','3','D','0','0','1'};
  capture_bytes.insert(capture_bytes.end(), magic.begin(), magic.end());
  Word(6); Word(kDeviceBytes);
  enabled.store(true);
  REXLOG_INFO("Gears D3D boundary observation enabled: frames {}..{}, limit 256 MiB; Xenia renders the game",
              first_frame, uint64_t(first_frame) + count - 1);
  if (waiting_for_trigger) REXLOG_INFO("Waiting for renderer capture trigger: {}", trigger_file.string());
}

void Gow3StopGraphicsBridge() {
  {
    marker_observation_enabled.store(false,std::memory_order_release);
    marker_submission_enabled.store(false,std::memory_order_release);
    std::lock_guard marker_lock(marker_observation_mutex);
    if(marker_registrations||marker_refusals)REXLOG_INFO("Native marker title observation final: registrations={} ring_revalidations={} invalidations={} refused={} lifetime_authenticated=false arbitrary_mutations_guarded=false",
        marker_registrations,marker_revalidations,marker_invalidations,marker_refusals);
    marker_observations={};marker_emissions={};marker_observation_memory=nullptr;
  }
  getdata_inline_selection.Reset();
  std::lock_guard lock(capture_mutex);
  getdata_inline_selection.Reset();
  ReportThinLiveMetadata();
  ReportSnapshotMetadata();
  const auto residency=live_snapshots.cache_stats();
  if(residency.peak_entries)REXLOG_INFO("Native snapshot cache: metadata_lru={} entries={} peak_entries={} retained_bytes={} budget_evictions={} metadata_clears={} metadata_clear_entries={} metadata_lru_evictions={}",
      residency.metadata_lru,residency.entries,residency.peak_entries,residency.retained_bytes,
      residency.budget_evictions,residency.metadata_clears,residency.metadata_clear_entries,residency.metadata_lru_evictions);
  const auto stable=live_snapshots.stable_certificate_stats();
  if(stable.enabled||stable.exact_hits||stable.exact_changes||stable.exact_fallbacks||stable.validation_attempts)REXLOG_INFO("Native stable certified snapshots: exact_hits={} exact_changes={} exact_fallbacks={} exact_fallback_bytes={} validation_attempts={} safe_hits={} safe_hit_bytes={} invalidations={} owner_refusals={} arm_attempts={} arms={} arm_refusals={} provider_errors={} cooldown_bypasses={} initial_hits={} maximum_hits={}",
      stable.exact_hits,stable.exact_changes,stable.exact_fallbacks,stable.exact_fallback_bytes,
      stable.validation_attempts,stable.safe_hits,stable.safe_hit_bytes,stable.invalidations,stable.owner_refusals,
      stable.arm_attempts,stable.arms,stable.arm_refusals,stable.provider_errors,stable.cooldown_bypasses,
      gow3::renderer::ImmutableSnapshots::kStableCertificateInitialHits,
      gow3::renderer::ImmutableSnapshots::kStableCertificateMaximumHits);
  if(stable.below_minimum_attempts)REXLOG_INFO(
      "Native active small snapshot validation: attempts={} hits={} hit_bytes={} refusals={}",
      stable.below_minimum_attempts,stable.below_minimum_hits,
      stable.below_minimum_hit_bytes,stable.below_minimum_refusals);
  const auto texture_batches=live_snapshots.batch_stats();
  if(texture_batches.enabled||texture_batches.calls)REXLOG_INFO("Native batch texture snapshots: enabled={} calls={} views={} unique_views={} provider_calls={} provider_views={} cohort_bytes={} scalar_fallback_calls={} provider_errors={}",
      texture_batches.enabled,texture_batches.calls,texture_batches.views,texture_batches.unique_views,
      texture_batches.provider_calls,texture_batches.provider_views,texture_batches.cohort_bytes,
      texture_batches.scalar_fallback_calls,texture_batches.provider_errors);
  if(snapshot_memo.hits||snapshot_memo.misses)REXLOG_INFO(
      "Native snapshot generation memo: hits={} misses={} stored={} verified={} mismatches={}",
      snapshot_memo.hits,snapshot_memo.misses,snapshot_memo.stored,snapshot_memo.verified,snapshot_memo.mismatches);
  const auto extent_stats=capture_texture_extents.statistics();
  if(extent_stats.hits||extent_stats.misses)REXLOG_INFO(
      "Native capture texture extents: hits={} misses={} slots=32 storage_bytes={}",
      extent_stats.hits,extent_stats.misses,sizeof(capture_texture_extents));
  const auto warm=live_snapshots.warm_batch_stats();
  if(warm.attempts)REXLOG_INFO("Native warm snapshot readability: attempts={} accepted={} accepted_views={} accepted_bytes={} provider_calls={} provider_errors={} provider_refusals={}; speculative_provider_hits_possible=true",
      warm.attempts,warm.accepted,warm.accepted_views,warm.accepted_bytes,
      warm.provider_calls,warm.provider_errors,warm.provider_refusals);
  live_snapshots.ConfigureCertifiedProvider({});
  if(guest_memory) {
    const auto stats=guest_memory->certified_physical_snapshot_stats();
    const auto cache=live_snapshots.certificate_stats();
    if(cache.attempts||cache.minimum_size_bypasses)REXLOG_INFO("Native certified snapshots: minimum_snapshot_bytes={} minimum_size_bypasses={} minimum_size_bypassed_bytes={} attempts={} accepted={} fallback={} hits={} captures={} rejected={} avoided_bytes={} armed_pages={} peak_armed_pages={} faults={} external_writes={} external_writer_pages={} protect_calls={} protect_failures={} metadata_bytes={} peak_metadata_bytes={} mutations={} faults_raw={} faults_a={} faults_c={} faults_e={} owned_fault_dispatches={}",
      REXCVAR_GET(gow3_native_certified_min_snapshot_bytes),cache.minimum_size_bypasses,cache.minimum_size_bypassed_bytes,
      cache.attempts,cache.accepted,cache.fallback,stats.hits,stats.captures,stats.rejected,stats.avoided_bytes,
      stats.armed_pages,stats.peak_armed_pages,stats.faults,stats.external_writes,stats.external_writer_pages,
      stats.protect_calls,stats.protect_failures,stats.metadata_bytes,stats.peak_metadata_bytes,stats.mutations,
      stats.alias_faults[0],stats.alias_faults[1],stats.alias_faults[2],stats.alias_faults[3],stats.owned_fault_dispatches);
    if(cache.attempts)REXLOG_INFO("Native certified queries: requested_batch={} requested_diagnostics={} point_queries={} batch_queries={} batch_failures={} pages={} cold_ms={} max_cold_ms={} metadata_ms={} query_ms={} capture_protect_ms={} copy_ms={}",
      REXCVAR_GET(gow3_native_certified_batch_queries),REXCVAR_GET(gow3_native_certified_diagnostics),stats.query_calls,stats.batch_queries,
      stats.batch_query_failures,stats.queried_pages,double(stats.cold_ns)/1e6,
      double(stats.maximum_cold_ns)/1e6,double(stats.metadata_ns)/1e6,double(stats.query_ns)/1e6,
      double(stats.capture_protect_ns)/1e6,double(stats.copy_ns)/1e6);
    guest_memory->SetCertifiedPhysicalSnapshotsEnabled(false);
    guest_memory->SetCertifiedPhysicalFaultFastPath(false);
    guest_memory->SetCertifiedPhysicalSnapshotBatchQueries(false);
    guest_memory->SetCertifiedPhysicalSnapshotDiagnostics(false);
  }
  DisableQueryPollYield();
  live_snapshots.ConfigureComparisonHelpers(0);
  live_snapshots.Clear();snapshot_memo.Reset();
  live_shader_inputs.Clear();
  Finish(2);
  live_enabled.store(false);
  live_sink = {};
  shader_discovery_enabled.store(false);
  shader_block_discovery_enabled.store(false);
  shader_discovery_sink = {};
  guest_memory = nullptr;
}


gow3::renderer::ObservedMarkerCounterAllocation gow3::renderer::ObserveMarkerCounterAllocation(uint32_t physical_target) noexcept {
  if(!marker_observation_enabled.load(std::memory_order_acquire))return {};
  try {
    std::lock_guard lock(marker_observation_mutex);
    if(!marker_observation_enabled.load(std::memory_order_relaxed))return {};
    MarkerAllocation result{};
    for(const auto& observed:marker_observations) {
      const auto& value=observed.value;
      if(!value.available||physical_target<value.physical_counter||
         uint64_t(physical_target)+4>uint64_t(value.physical_counter)+value.logical_bytes)continue;
      if(result.available)return {}; // Ambiguous overlapping observations are unknown.
      result=value;
      result.live=result.live&&observed.allocation&&observed.allocation->current();
    }
    return result;
  } catch(...) {return {};}
}
std::shared_ptr<void> gow3::renderer::AcquireNativeImmediateMarkerTargetGuard(
    const rex::graphics::NativeCompletionMarkerPair& pair) {
  if(!marker_submission_enabled.load(std::memory_order_acquire))return {};
  auto guard=std::make_shared<ImmediateMarkerTargetGuard>(marker_observation_mutex);
  if(!marker_submission_enabled.load(std::memory_order_relaxed)||!marker_observation_memory)return {};
  if((pair.counter&3)||uint64_t(pair.counter)+96>0x20000000ull||
     (pair.command_address&3)||pair.command_address<pair.command_buffer||
     uint64_t(pair.command_address)+32>uint64_t(pair.command_buffer)+pair.command_capacity||
     uint64_t(pair.command_buffer)+pair.command_capacity>0x20000000ull||
     pair.words[0]!=0xC0025800u||pair.words[1]!=3||pair.words[2]!=((pair.counter+4)|2u)||
     pair.words[4]!=0xC0025800u||pair.words[5]!=3||pair.words[6]!=(pair.counter|2u))return {};
  MarkerObservation* owner=nullptr;
  for(auto& candidate:marker_observations)if(candidate.value.available&&candidate.value.live&&
      candidate.value.physical_counter==pair.counter) {
    if(owner)return {};owner=&candidate;
  }
  if(!owner||!owner->allocation||!owner->allocation->current()||owner->ring_reset)return {};
  const auto& value=owner->value;
  if(uint64_t(pair.command_address)<uint64_t(value.allocation_base)+value.allocation_bytes&&
     uint64_t(value.allocation_base)<uint64_t(pair.command_address)+32)return {};
  MarkerEmission* match=nullptr;
  for(auto& emission:marker_emissions)if(emission.valid&&emission.source==pair.command_address&&
      emission.device==value.device&&emission.generation==value.registration_generation&&
      emission.epoch==value.command_epoch&&emission.words==pair.words) {
    if(match)return {};match=&emission;
  }
  if(!match)return {};
  match->valid=false; // Consumed only after full identity and current allocator checks.
  return guard;
}
void Gow3ObserveMarkerPairEmitted(rex::ppc::Register& device,rex::ppc::Register& last_word,
                                  rex::ppc::Register& counter,rex::ppc::Register& position,
                                  rex::ppc::Register& serial) {
  if(!marker_submission_enabled.load(std::memory_order_acquire))return;
  try {
    auto memory_lock=rex::thread::global_critical_region::AcquireDirect();
    std::lock_guard lock(marker_observation_mutex);
    if(!marker_submission_enabled.load(std::memory_order_relaxed)||!marker_observation_memory||
       last_word.u32<28||(last_word.u32&3))return;
    const uint32_t source=marker_observation_memory->GetPhysicalAddress(last_word.u32-28);
    if(source==UINT32_MAX||uint64_t(source)+32>0x20000000ull||
       marker_observation_memory->GetPhysicalAddress(last_word.u32)!=source+28)return;
    for(auto& owner:marker_observations)if(owner.value.device==device.u32) {
      const auto& value=owner.value;
      if(!value.live||value.virtual_counter!=counter.u32||!owner.allocation||!owner.allocation->current()||
         owner.ring_reset||(owner.emitted&&serial.u32<=owner.last_serial))return;
      // Do not certify serial wrap/repeated emission. A real ring reset creates
      // a fresh command epoch. Bounded eviction is refusal, never substitution.
      owner.last_serial=serial.u32;owner.emitted=true;
      for(auto& old:marker_emissions)if(old.valid&&uint64_t(old.source)<uint64_t(source)+32&&
          uint64_t(source)<uint64_t(old.source)+32)old.valid=false;
      auto& output=marker_emissions[marker_emission_cursor++%marker_emissions.size()];
      output={source,device.u32,{0xC0025800u,3,(value.physical_counter+4)|2u,position.u32,
                                0xC0025800u,3,value.physical_counter|2u,serial.u32},
              value.registration_generation,value.command_epoch,true};
      return;
    }
  } catch(...) {marker_submission_enabled.store(false,std::memory_order_release);}
}
void Gow3ObserveMarkerCounterInitialized(rex::ppc::Register& device,rex::ppc::Register& counter) {ObserveMarkerRegistration(device.u32,counter.u32,false);}
void Gow3ObserveMarkerCounterRingReady(rex::ppc::Register& device,rex::ppc::Register& counter) {ObserveMarkerRegistration(device.u32,counter.u32,true);}
void Gow3ObserveMarkerCounterDestroyed(rex::ppc::Register& device) {InvalidateMarkerObservation(device.u32,false);}
void Gow3ObserveMarkerCounterRingReset(rex::ppc::Register& device) {InvalidateMarkerObservation(device.u32,true);}
void Gow3ObserveMarkerCounterRecovery(rex::ppc::Register& device) {InvalidateMarkerObservation(device.u32,false);}
void Gow3ObserveMarkerCounterCpuDirect(rex::ppc::Register& device) {InvalidateMarkerObservation(device.u32,false);}

void Gow3SetGraphicsBoundarySink(gow3::renderer::GraphicsBoundarySink sink) {
  getdata_inline_selection.Reset();
  std::lock_guard lock(capture_mutex);
  getdata_inline_selection.Reset();
  live_shader_inputs.Clear();
  if(!sink){
    ReportThinLiveMetadata();
    DisableQueryPollYield();live_snapshots.ConfigureCertifiedProvider({});live_snapshots.Clear();snapshot_memo.Reset();
    if(guest_memory){
      guest_memory->SetCertifiedPhysicalSnapshotsEnabled(false);
      guest_memory->SetCertifiedPhysicalFaultFastPath(false);
      guest_memory->SetCertifiedPhysicalSnapshotBatchQueries(false);
      guest_memory->SetCertifiedPhysicalSnapshotDiagnostics(false);
    }
  }
  if (sink && guest_memory) {
    for (const auto& site : gow3::renderer::kUpCommitSites) {
      if (BigWord(guest_memory->virtual_membase() + site[0]) != site[1])
        throw std::runtime_error("UP completion hook instruction mismatch; native renderer disabled");
    }
    if(command_spans_enabled.load())for(const auto& site:gow3::renderer::kPacketCommitSites)
      if(BigWord(guest_memory->virtual_membase()+site[0])!=site[1])
        throw std::runtime_error("Command ownership hook instruction mismatch");
  }
  live_snapshots.ConfigureMetadataEvictionObserver(nullptr);
  if(sink) {
    capture_texture_extents.Clear();
    thin_live_metadata_enabled=REXCVAR_GET(gow3_native_thin_live_metadata);
    thin_live_metadata_stats={};thin_live_metadata_reported=false;
    snapshot_metadata_observed=0;snapshot_metadata_reported=false;
    if(REXCVAR_GET(gow3_native_certified_diagnostics))
      live_snapshots.ConfigureMetadataEvictionObserver(ObserveSnapshotMetadata);
  }
  live_snapshots.ConfigureMetadataLru(REXCVAR_GET(gow3_native_snapshot_metadata_lru));
  capture_texture_extents_enabled=bool(sink)&&REXCVAR_GET(gow3_native_cache_capture_texture_extents);
  warm_snapshot_readability_enabled=bool(sink)&&REXCVAR_GET(gow3_native_warm_snapshot_readability);
  snapshot_memo.Reset();
  snapshot_memo.enabled=bool(sink)&&REXCVAR_GET(gow3_native_snapshot_generation_memo);
  snapshot_memo.verify=REXCVAR_GET(gow3_native_snapshot_generation_memo_verify);
  live_snapshots.ConfigureCertifiedProvider({});
  if(sink&&guest_memory&&REXCVAR_GET(gow3_native_certified_snapshots)) {
    guest_memory->SetCertifiedPhysicalFaultFastPath(REXCVAR_GET(gow3_native_certified_fault_fastpath));
    guest_memory->SetCertifiedPhysicalSnapshotBatchQueries(REXCVAR_GET(gow3_native_certified_batch_queries));
    guest_memory->SetCertifiedPhysicalSnapshotDiagnostics(REXCVAR_GET(gow3_native_certified_diagnostics));
    const bool active=guest_memory->SetCertifiedPhysicalSnapshotsEnabled(true);
    REXLOG_INFO("Native certified physical snapshots: {}, minimum_snapshot_bytes={}",
        active?"enabled":"unsupported; exact fallback",REXCVAR_GET(gow3_native_certified_min_snapshot_bytes));
    if(active) {
      auto capture=[](uint32_t address,size_t size,const std::shared_ptr<const void>& old) {
        const auto prior=std::static_pointer_cast<const rex::memory::CertifiedPhysicalSnapshot>(old);
        auto captured=guest_memory->TryGetCertifiedPhysicalSnapshot(address,uint32_t(size),prior);
        return captured?gow3::renderer::ImmutableSnapshots::Certified{captured->bytes(),std::move(captured)}
                       :gow3::renderer::ImmutableSnapshots::Certified{};
      };
      const auto minimum=size_t(REXCVAR_GET(gow3_native_certified_min_snapshot_bytes));
      if(REXCVAR_GET(gow3_native_stable_certified_snapshots)) {
        live_snapshots.ConfigureStableCertifiedProvider(std::move(capture),
          [](uint32_t address,size_t size,const std::shared_ptr<const void>& old) {
            const auto prior=std::static_pointer_cast<const rex::memory::CertifiedPhysicalSnapshot>(old);
            auto valid=guest_memory->ValidatePhysicalSnapshot(address,uint32_t(size),prior);
            return valid?gow3::renderer::ImmutableSnapshots::Certified{valid->bytes(),std::move(valid)}
                        :gow3::renderer::ImmutableSnapshots::Certified{};
          },minimum);
        if(REXCVAR_GET(gow3_native_batch_texture_snapshots)) {
          live_snapshots.ConfigureBatchCertifiedProvider([](std::span<const gow3::renderer::ImmutableSnapshots::BatchRequest> views) {
            std::vector<rex::memory::PhysicalSnapshotBatchRequest> requests;requests.reserve(views.size());
            for(const auto& view:views)requests.push_back({view.address,uint32_t(view.size),
              std::static_pointer_cast<const rex::memory::CertifiedPhysicalSnapshot>(view.previous),
              view.validate_only?rex::memory::PhysicalSnapshotBatchRequest::Mode::ValidateOnly:
                                 rex::memory::PhysicalSnapshotBatchRequest::Mode::Capture});
            auto snapshots=guest_memory->TryGetCertifiedPhysicalSnapshots(requests);
            std::vector<gow3::renderer::ImmutableSnapshots::Certified> out;out.reserve(snapshots.size());
            for(auto& snapshot:snapshots)out.push_back(snapshot?
              gow3::renderer::ImmutableSnapshots::Certified{snapshot->bytes(),std::move(snapshot)}:
              gow3::renderer::ImmutableSnapshots::Certified{});
            return out;
          });
        }
      } else live_snapshots.ConfigureCertifiedProvider(std::move(capture),minimum);
      REXLOG_INFO("Native stable certificate policy: {}",REXCVAR_GET(gow3_native_stable_certified_snapshots)?"enabled":"disabled");
    }
  }
  live_snapshots.ConfigureActiveBelowMinimum(bool(sink)&&REXCVAR_GET(gow3_native_validate_active_small_snapshots));
  live_sink = std::move(sink);
  // The capture lock excludes an accepted comparison while its pool changes.
  // Optional helpers never keep guest pointers after the caller returns.
  try {
    const auto helpers=std::min(unsigned(std::max(int32_t(0),REXCVAR_GET(gow3_native_snapshot_workers))),
        gow3::renderer::ParallelSnapshotCompare::HardwareHelperLimit());
    live_snapshots.ConfigureComparisonHelpers(live_sink?helpers:0);
  } catch(const std::exception& error) {
    live_snapshots.ConfigureComparisonHelpers(0);
    REXLOG_WARN("Native snapshot helpers unavailable: {}",error.what());
  }
  if(live_sink) {
    REXLOG_INFO("Native exact snapshot comparison helpers: {}",live_snapshots.comparison_helpers());
    REXLOG_INFO("Native thin live metadata: {}",thin_live_metadata_enabled?"enabled":"disabled");
    REXLOG_INFO("Native batch texture snapshots startup: requested={} enabled={} maximum_views=64 maximum_requested_bytes=8388608",
        REXCVAR_GET(gow3_native_batch_texture_snapshots),live_snapshots.batch_stats().enabled);
    REXLOG_INFO("Native warm snapshot readability startup: {}",warm_snapshot_readability_enabled);
    REXLOG_INFO("Native snapshot generation memo startup: enabled={} verify={}",snapshot_memo.enabled,snapshot_memo.verify);
    REXLOG_INFO("Native capture texture extents startup: {}",capture_texture_extents_enabled);
    REXLOG_INFO("Native active small snapshot validation startup: requested={} enabled={}",
        REXCVAR_GET(gow3_native_validate_active_small_snapshots),
        REXCVAR_GET(gow3_native_validate_active_small_snapshots)&&live_snapshots.stable_certificate_stats().enabled);
  }
  live_enabled.store(bool(live_sink), std::memory_order_release);
}

void Gow3SetShaderDiscoverySink(gow3::renderer::ShaderDiscoverySink sink) {
  std::lock_guard lock(capture_mutex);
  if(shader_block_discovery_enabled.exchange(false)) {
    const auto& s=shader_block_stats;
    REXLOG_INFO("Native shader block prewarm final: calls={}, duplicates={}, blocks={}, bytes={}, emitted={}, emitted_bytes={}, prioritized={}, ineligible_requests={}, refused={}, limited={}, errors={}, scan_ms={}, maximum_scan_ms={}",
        s.calls,s.duplicates,s.blocks,s.bytes,s.emitted,s.emitted_bytes,s.prioritized,s.ineligible_requests,s.refused,s.limited,s.errors,s.milliseconds,s.maximum_ms);
  }
  if(sink) {
    if(!guest_memory)throw std::runtime_error("Shader discovery requires the loaded guest image");
    for(const auto& site:gow3::renderer::kShaderDiscoverySites) {
      if(!Readable(site[0],4)||BigWord(guest_memory->virtual_membase()+site[0])!=site[1])
        throw std::runtime_error("Shader discovery instruction mismatch; speculative preparation disabled");
    }
  }
  shader_discovery_sink=std::move(sink);
  shader_discovery_enabled.store(bool(shader_discovery_sink),std::memory_order_release);
  if(shader_discovery_sink&&REXCVAR_GET(gow3_native_block_shader_prewarm)) {
    bool valid=true;
    for(const auto& site:kShaderBlockDiscoverySites)
      valid&=Readable(site[0],4)&&BigWord(guest_memory->virtual_membase()+site[0])==site[1];
    if(valid) {
      discovered_shader_blocks={};next_discovered_shader_block=0;shader_block_stats={};
      shader_block_discovery_enabled.store(true,std::memory_order_release);
      REXLOG_INFO("Native shader block prewarm enabled: max_block_bytes={}, pixel_only=true, block_history=64",
          gow3::renderer::kMaxShaderDiscoveryBlockBytes);
    } else REXLOG_WARN("Native shader block prewarm refused: instruction witness mismatch");
  }
}

void Gow3EnableNativeQueryPollYield(bool enable) {
  std::lock_guard lock(capture_mutex);
  if(!enable) {DisableQueryPollYield();return;}
  if(!live_sink||!guest_memory)throw std::runtime_error("Native query poll yield requires the live renderer");
  for(const auto& site:gow3::renderer::kQueryPollYieldSites)
    if(!Readable(site[0],4)||BigWord(guest_memory->virtual_membase()+site[0])!=site[1])
      throw std::runtime_error("Native query poll yield instruction mismatch");
  const auto previous=query_poll_epoch.load(std::memory_order_relaxed);
  if(previous&1)return;
  // Never recycle a previously observed epoch, even in the theoretical wrap case.
  if(previous>=UINT64_MAX-2)throw std::runtime_error("Native query poll yield epoch exhausted");
  query_poll_epoch.store(previous+3,std::memory_order_release);
  REXLOG_INFO("Native query poll yield enabled: interval={}, retry caller=0x{:08X}",
      gow3::renderer::kQueryPollYieldInterval,gow3::renderer::kQueryPollRetryCaller);
}

static std::optional<gow3::renderer::PublishedQueryHeader> PublishedHeader(uint32_t query,uint8_t* base) {
  if(!Readable(query,0x98))return {};
  const auto* data=base+query;
  return gow3::renderer::PublishedQueryHeader{BigWord(data),BigWord(data+24),BigWord(data+28),
      BigWord(data+148),BigWord(data+4),data[20]};
}
void Gow3SetPublishedQueryResults(std::shared_ptr<gow3::renderer::PublishedQueryResults> results) {
  published_queries_enabled.store(false,std::memory_order_release);
  std::lock_guard lock(capture_mutex);
  published_queries.reset();
  if(!results)return;
  if(!getdata_inline_selection.enabled()||!live_sink||!guest_memory||!live_enabled.load())
    throw std::runtime_error("Published-query consumer requires authenticated Native GetData");
  for(const auto& site:gow3::renderer::kQueryPollYieldSites)
    if(!Readable(site[0],4)||BigWord(guest_memory->virtual_membase()+site[0])!=site[1])
      throw std::runtime_error("Published-query consumer retry caller mismatch");
  published_queries=std::move(results);
  published_queries_enabled.store(true,std::memory_order_release);
}
bool Gow3NativeQueryResultAvailable(uint32_t query,uint32_t device,uint32_t serial,uint8_t flags,uint8_t* base) noexcept {
  // The wrapper selects this body only every1024 blocking retries. The common
  // retry executes the existing exact inline leaf without this host callback.
  try {
    std::lock_guard lock(capture_mutex);
    if(!published_queries||!live_enabled.load()||!getdata_inline_selection.enabled())return false;
    const auto header=PublishedHeader(query,base);
    if(!header||header->device!=device||header->serial!=serial||header->flags!=flags)return false;
    published_query_checks.fetch_add(1,std::memory_order_relaxed);
    const bool ready=published_queries->Ready(query,*header);
    if(ready) {published_query_accepts.fetch_add(1,std::memory_order_relaxed);published_query_gate_used=true;}
    return ready;
  }catch(...){return false;}
}

void Gow3SetQueryReadinessProbe(Gow3QueryReadinessProbe probe) {
  std::lock_guard lock(capture_mutex);
  query_readiness_enabled.store(false,std::memory_order_release);
  query_readiness_probe=std::move(probe);
  query_readiness_enabled.store(bool(query_readiness_probe),std::memory_order_release);
}
Gow3QueryReadinessStatistics Gow3ReadQueryReadinessStatistics() {
  return {readiness_samples.load(),readiness_tracked.load(),readiness_published_before.load(),
      readiness_pending_after_published.load(),readiness_completed_after_published.load(),
      readiness_changed_during_call.load(),readiness_errors.load()};
}
static std::optional<gow3::renderer::QueryReadinessReceipt> ReadQueryReadiness(uint32_t query,uint8_t* base) noexcept {
  try {
    std::lock_guard lock(capture_mutex);
    if(!query_readiness_probe||!live_enabled.load()||!Readable(query,0x98))return {};
    const auto* data=base+query;
    // Only the already-supported single-report type9 occlusion query. Check
    // the current header against its exact producer-side generation receipt.
    if(BigWord(data+4)!=9||!(data[20]&0x40)||BigWord(data+0x94)!=1)return {};
    const auto report=BigWord(data+0x1c);
    const auto address=(report&0x1fffffffu)+(((report>>20)+0x200)&0x1000);
    auto receipt=query_readiness_probe(query);
    if(!receipt||!report||receipt->address!=address)return {};
    return receipt;
  } catch(...) {readiness_errors.fetch_add(1,std::memory_order_relaxed);return {};}
}

void Gow3EnableNativeGetDataInline(bool enable) {
  // Reset before acquiring any bridge lock or performing fallible validation.
  getdata_inline_selection.Reset();
  std::lock_guard lock(capture_mutex);
  getdata_inline_selection.Reset();
  if(!enable)return;
  const bool native=live_sink&&guest_memory&&live_enabled.load(std::memory_order_acquire)&&
      ParseGow3RendererMode(REXCVAR_QUERY(std::string,gow3_renderer))==Gow3RendererMode::Native;
  try {
    const bool available=Gow3GetDataExactInlineLeafAvailable();
    uint32_t last_address=0,last_value=0;bool last_readable=false;
    const bool selected=getdata_inline_selection.TryEnable(true,native,available,
        [&](uint32_t address,uint32_t& value) {
          last_address=address;last_readable=Readable(address,4);
          if(!last_readable)return false;
          value=BigWord(guest_memory->virtual_membase()+address);last_value=value;return true;
        });
    if(!selected) {
      REXLOG_WARN("Native GetData inline capability refused: native={}, available={}, last_address=0x{:08X}, readable={}, last_word=0x{:08X}",
          native,available,last_address,last_readable,last_value);
      throw std::runtime_error("GetData helper inline refused: Native sink, trace/profile, helper aliases or full instruction spans do not match");
    }
    REXLOG_INFO("Native GetData exact register-helper inline enabled: full GetData864B/save32B/restore36B validated; readiness/yield unchanged");
  } catch(...) {getdata_inline_selection.Reset();throw;}
}

void Gow3EnableGraphicsCommandSpans(bool enable) {
  std::lock_guard lock(capture_mutex);
  if(live_sink)throw std::runtime_error("Command span mode must be selected before installing the renderer sink");
  command_spans_enabled.store(enable,std::memory_order_release);
}

static gow3::renderer::GraphicsCommandSpan CaptureCommands(uint32_t device,uint32_t end) {
  diagnostic_command_span.reset();diagnostic_command_args={device,end,0,0,0,0,0,0};
  if(!Readable(device+48,4))throw std::runtime_error("Missing native command-pointer state");
  const auto* base=guest_memory->virtual_membase();
  const uint32_t begin=BigWord(base+device+48)+4;
  diagnostic_command_args[2]=begin;
  const uint64_t finish=uint64_t(end)+4;
  if(finish<=begin||finish-begin>1024*1024||!Readable(begin,uint32_t(finish-begin)))
    throw std::runtime_error("Invalid finalized native command span");
  gow3::renderer::GraphicsCommandSpan span;
  span.generation=1; // Temporary validation identity; assigned under capture lock.
  span.address=guest_memory->GetPhysicalAddress(begin);
  const uint32_t bytes=uint32_t(finish-begin);
  if(guest_memory->GetPhysicalAddress(uint32_t(finish-1))!=uint64_t(span.address)+bytes-1)
    throw std::runtime_error("Noncontiguous native command allocation");
  span.bytes.assign(base+begin,base+begin+bytes);
  span.api_sequence=command_owner.sequence;
  diagnostic_command_span=span;
  gow3::renderer::ParseCommandSpan(span);
  return span;
}

static void RejectCommandSpan(const std::exception& error) {
  std::lock_guard lock(capture_mutex);
  REXLOG_ERROR("Native command ownership capture failed: {}",error.what());
  if(live_sink) {
    // A missing span is an explicit planner error. Deliver it through the
    // normal failure path, releasing queued-frame backpressure, rather than
    // unwinding the guest instruction stream after a diagnostic hook failed.
    gow3::renderer::GraphicsBoundaryEvent invalid;
    invalid.sequence=live_sequence++;invalid.frame=frame;invalid.span_only=true;
    invalid.command_span=std::move(diagnostic_command_span);invalid.args=diagnostic_command_args;
    invalid.capture_error=error.what();
    try {live_sink(std::move(invalid));}catch(const std::exception&) {}
  }
  getdata_inline_selection.Reset();live_sink={};live_enabled.store(false);DisableQueryPollYield();
}

void Gow3CaptureOwnedCommands(rex::ppc::Register& device,rex::ppc::Register& command_end) {
  if(!command_spans_enabled.load(std::memory_order_relaxed)||!live_enabled.load(std::memory_order_relaxed)||
     command_owner.sequence==UINT64_MAX||command_owner.device!=device.u32)return;
  // Some state helpers commit an unchanged cursor when they emitted nothing.
  // This is an empty span, not an allocation jump or an owned draw packet.
  if(Readable(device.u32+48,4)&&BigWord(guest_memory->virtual_membase()+device.u32+48)==command_end.u32)return;
  try {
    auto span=CaptureCommands(device.u32,command_end.u32);
    if(gow3::renderer::ParseCommandSpan(span).draws.empty())return;
    std::lock_guard lock(capture_mutex);
    if(!live_sink)return;
    span.generation=++command_generation;span.api_sequence=command_owner.sequence;
    gow3::renderer::GraphicsBoundaryEvent event;
    event.sequence=live_sequence++;event.frame=frame;event.span_only=true;event.command_span=std::move(span);
    live_sink(std::move(event));
  }catch(const std::exception& error){RejectCommandSpan(error);}
}

static void CaptureChunk(uint32_t function,rex::ppc::Register& device,rex::ppc::Register& topology,
                         rex::ppc::Register& first,rex::ppc::Register& count,rex::ppc::Register& command_end) {
  if(!command_spans_enabled.load(std::memory_order_relaxed)||!live_enabled.load(std::memory_order_relaxed))return;
  try {
    auto span=CaptureCommands(device.u32,command_end.u32);
    if(gow3::renderer::ParseCommandSpan(span).draws.size()!=1)
      throw std::runtime_error("Native regular draw chunk is not one raster packet");
    PPCContext context{};context.r3.u32=device.u32;
    const std::array<uint32_t,8> args{device.u32,topology.u32,first.u32,count.u32,0,0,0,0};
    Observe(function,context,guest_memory->virtual_membase(),3,&args,0,std::move(span));
  }catch(const std::exception& error){RejectCommandSpan(error);}
}
void Gow3CapturePrimitiveChunk(rex::ppc::Register& device,rex::ppc::Register& topology,
    rex::ppc::Register& first,rex::ppc::Register& count,rex::ppc::Register& command_end) {
  CaptureChunk(profile::d3d::kDrawVertices,device,topology,first,count,command_end);
}
void Gow3CaptureIndexedChunk(rex::ppc::Register& device,rex::ppc::Register& topology,
    rex::ppc::Register& first,rex::ppc::Register& count,rex::ppc::Register& command_end) {
  CaptureChunk(profile::d3d::kDrawIndexedVertices,device,topology,first,count,command_end);
}

// Called immediately after the game's inline EndVertices commit stores.
// Begin only allocates; taking bytes here observes the caller's final writes.
void Gow3CaptureUpEnd(rex::ppc::Register& device,rex::ppc::Register& command_end) {
  if (!Observing() || pending_up.function == 0 ||
      device.u32 != pending_up.args[0]) return;
  const auto pending = pending_up;
  pending_up.function = 0;
  if (!guest_memory) return;
  PPCContext context{};
  context.r3.u32 = pending.result;
  std::optional<gow3::renderer::GraphicsCommandSpan> span;
  try {
    if(command_spans_enabled.load(std::memory_order_relaxed)) {
      span=CaptureCommands(device.u32,command_end.u32);
      if(gow3::renderer::ParseCommandSpan(*span).draws.size()!=1)
        throw std::runtime_error("Finalized UP allocation is not one raster packet");
    }
  }catch(const std::exception& error){RejectCommandSpan(error);span.reset();}
  Observe(pending.function, context, guest_memory->virtual_membase(), 2, &pending.args, pending.lr,std::move(span));
}

void Gow3RememberUp(uint32_t function, const std::array<uint32_t, 8>& args, uint32_t lr, uint32_t result) {
  if (!Observing()) return;
  pending_up = {};
  if ((function == profile::d3d::kBeginVertices && !result) || (function == profile::d3d::kBeginIndexedVertices && result)) return;
  pending_up.function = function; pending_up.args = args; pending_up.lr = lr; pending_up.result = result;
}

// Invalidate before any new Issue, then bind the actual post-Issue header to the
// newly captured End. An exception cannot bind an old generation. The original
// routine remains solely responsible for all guest writes and command emission.
REX_EXTERN(__imp__sub_82AFA458);
REX_EXTERN(sub_82AFA458) {
  std::shared_ptr<gow3::renderer::PublishedQueryResults> results;
  const auto query=ctx.r3.u32;
  const bool end=(ctx.r4.u32&3)==1;
  if(published_queries_enabled.load(std::memory_order_acquire)) {
    std::lock_guard lock(capture_mutex);
    results=published_queries;
    if(results)results->Invalidate(query);
  }
  __imp__sub_82AFA458(ctx,base);
  if(results&&end) {
    std::lock_guard lock(capture_mutex);
    if(results==published_queries)if(const auto header=PublishedHeader(query,base))
      if(results->Bind(query,*header))published_query_binds.fetch_add(1,std::memory_order_relaxed);
  }
}

// Default path preserves every original guest instruction. The optional Native
// result path changes only readiness of proven, published occlusion samples;
// report decoding and all global completion markers retain their own logic.
std::atomic<uint64_t> query_waits{0},query_wait_nanoseconds{0};
thread_local uint32_t query_wait_id=0;
thread_local std::chrono::steady_clock::time_point query_wait_start;
struct QueryWaitScope {
  PPCContext& context;
  const bool retry;
  const uint32_t query;
  explicit QueryWaitScope(PPCContext& c)
      : context(c),retry(c.lr==gow3::renderer::kQueryPollRetryCaller),query(c.r3.u32) {
    if(retry&&query!=query_wait_id) {query_wait_id=query;query_wait_start=std::chrono::steady_clock::now();}
  }
  ~QueryWaitScope() {
    if(!retry||context.r3.u32!=0||query_wait_id!=query)return;
    const auto waited=std::chrono::steady_clock::now()-query_wait_start;
    query_wait_nanoseconds.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count()),
                                     std::memory_order_relaxed);
    query_waits.fetch_add(1,std::memory_order_relaxed);
    query_wait_id=0;
  }
};


REX_EXTERN(__imp__sub_82AFA708);

// Direct-execution occlusion results, keyed by guest query object.
namespace {
struct DirectQueryState {
  std::mutex mutex;
  struct Entry {uint64_t generation=0;bool ended=false,ready=false,waited=false;uint32_t samples=0;
               std::chrono::steady_clock::time_point ended_at;};
  std::unordered_map<uint32_t,Entry> queries;
  std::unordered_map<uint64_t,uint32_t> generations;
};
std::atomic<bool> direct_queries_enabled{false};
std::atomic<bool> direct_query_waiting{false};
// Bumped on every published result: a spinning GetData re-checks the table
// only when something new arrived, instead of contending with the publisher.
std::atomic<uint64_t> direct_results_published{0};
// Blocking GetData retries: publishers notify only while someone waits.
std::mutex direct_publish_mutex;
std::condition_variable direct_publish_cv;
std::atomic<uint32_t> direct_publish_waiters{0};
std::atomic<int32_t> direct_getdata_wait_us{0};
thread_local uint32_t direct_pending_query=0;
thread_local uint64_t direct_pending_version=UINT64_MAX;
std::atomic<uint64_t> direct_results{0},direct_latency_ns{0},direct_waits{0},direct_backlog{0};
DirectQueryState& DirectQueries() {static DirectQueryState state;return state;}
} // namespace


void Gow3EnableDirectQueries(bool enabled) {
  auto& state=DirectQueries();
  std::lock_guard lock(state.mutex);
  state.queries.clear();state.generations.clear();
  direct_getdata_wait_us.store(enabled?REXCVAR_GET(gow3_native_direct_getdata_wait_us):0,std::memory_order_relaxed);
  direct_queries_enabled.store(enabled,std::memory_order_release);
}
void Gow3DirectQueryBegun(uint32_t query) {
  auto& state=DirectQueries();
  std::lock_guard lock(state.mutex);
  if(auto it=state.queries.find(query);it!=state.queries.end()) {
    state.generations.erase(it->second.generation);
    it->second={};
  } else state.queries.emplace(query,DirectQueryState::Entry{});
}
void Gow3DirectQueryEnded(uint32_t query,uint64_t generation) {
  auto& state=DirectQueries();
  std::lock_guard lock(state.mutex);
  auto& entry=state.queries[query];
  if(entry.ended)state.generations.erase(entry.generation);
  entry={generation,true,false,false,0,std::chrono::steady_clock::now()};
  state.generations[generation]=query;
}
bool Gow3TakeDirectQueryWaiting() {
  return direct_query_waiting.exchange(false,std::memory_order_acq_rel);
}
void Gow3DirectQueryResult(uint64_t generation,uint64_t samples) {
  auto& state=DirectQueries();
  std::lock_guard lock(state.mutex);
  const auto it=state.generations.find(generation);
  if(it==state.generations.end())return;  // Superseded by a newer use of the query.
  auto& entry=state.queries[it->second];
  entry.ready=true;entry.samples=uint32_t(std::min<uint64_t>(samples,UINT32_MAX));
  direct_results.fetch_add(1,std::memory_order_relaxed);
  direct_latency_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-entry.ended_at).count()),std::memory_order_relaxed);
  state.generations.erase(it);
  direct_results_published.fetch_add(1,std::memory_order_release);
  if(direct_publish_waiters.load(std::memory_order_acquire)) {
    {std::lock_guard publish_lock(direct_publish_mutex);}
    direct_publish_cv.notify_all();
  }
}
// Type-9 occlusion GetData for direct execution. Returns false to defer to the
// original body (other query types, or queries direct mode never saw).
static bool DirectGetData(PPCContext& ctx,uint8_t* base) {
  const uint32_t query=ctx.r3.u32,data=ctx.r4.u32;
  if(!Readable(query,0x98))return false;
  uint8_t* object=base+query;
  if(BigWord(object+4)!=9||!(object[20]&0x40))return false;
  auto& state=DirectQueries();
  const uint64_t version=direct_results_published.load(std::memory_order_acquire);
  if(query==direct_pending_query&&version==direct_pending_version) {
    // Nothing published since this query was found pending. Keep asking the
    // executor to flush: its work may not have been executed at the last ask.
    direct_query_waiting.store(true,std::memory_order_release);
    if(const int32_t wait_us=direct_getdata_wait_us.load(std::memory_order_relaxed);
       wait_us>0&&ctx.lr==gow3::renderer::kQueryPollRetryCaller) {
      // Sleep until the next publication (or the bound) instead of spinning;
      // the caller retries either way, so a missed wakeup only costs the bound.
      direct_publish_waiters.fetch_add(1,std::memory_order_acq_rel);
      {
        std::unique_lock wait_lock(direct_publish_mutex);
        direct_publish_cv.wait_for(wait_lock,std::chrono::microseconds(wait_us),[version]{
          return direct_results_published.load(std::memory_order_acquire)!=version;});
      }
      direct_publish_waiters.fetch_sub(1,std::memory_order_acq_rel);
    }
    ctx.r3.u64=1;return true;
  }
  uint32_t samples=0;
  {
    std::lock_guard lock(state.mutex);
    const auto it=state.queries.find(query);
    if(it==state.queries.end()||(!it->second.ended&&!it->second.ready))return false;
    if(!it->second.ready) {  // S_FALSE: still pending; ask the executor to flush.
      if(!it->second.waited) {
        it->second.waited=true;
        direct_waits.fetch_add(1,std::memory_order_relaxed);
        direct_backlog.fetch_add(state.generations.size(),std::memory_order_relaxed);
      }
      direct_query_waiting.store(true,std::memory_order_release);
      direct_pending_query=query;direct_pending_version=version;
      ctx.r3.u64=1;return true;
    }
    samples=it->second.samples;
  }
  if(data&&Readable(data,4)) {
    uint8_t* out=base+data;
    out[0]=uint8_t(samples>>24);out[1]=uint8_t(samples>>16);out[2]=uint8_t(samples>>8);out[3]=uint8_t(samples);
  }
  object[20]=uint8_t((object[20]&0x7f)|0x80);
  ctx.r3.u64=0;
  return true;
}

Gow3QueryWaitStats Gow3GetQueryWaitStats() {
  return {query_waits.load(std::memory_order_relaxed),query_wait_nanoseconds.load(std::memory_order_relaxed),
          direct_results.load(std::memory_order_relaxed),direct_latency_ns.load(std::memory_order_relaxed),
          direct_waits.load(std::memory_order_relaxed),direct_backlog.load(std::memory_order_relaxed)};
}
REX_EXTERN(sub_82AFA708) {
  const QueryWaitScope wait_scope(ctx);
  if(direct_queries_enabled.load(std::memory_order_acquire)&&DirectGetData(ctx,base))return;
  const bool selected=getdata_inline_selection.enabled();
  const auto invoke=[selected](PPCContext& context,uint8_t* memory) {
    const bool sample=query_readiness_enabled.load(std::memory_order_relaxed)&&
        context.lr==gow3::renderer::kQueryPollRetryCaller&&((++readiness_calls&1023)==0);
    const auto query=context.r3.u32;
    const auto before=sample?ReadQueryReadiness(query,memory):std::nullopt;
    if(selected&&context.lr==gow3::renderer::kQueryPollRetryCaller&&
       published_queries_enabled.load(std::memory_order_relaxed)&&
       ((++published_query_retry_count&1023)==0)) {
      published_query_gate_used=false;
      Gow3GetDataPublishedQueryLeaf(context,memory);
      if(published_query_gate_used) {
        if(context.r3.u32==0)published_query_successes.fetch_add(1,std::memory_order_relaxed);
        else if(context.r3.u32==1)published_query_pending.fetch_add(1,std::memory_order_relaxed);
        else published_query_errors.fetch_add(1,std::memory_order_relaxed);
      }
    } else gow3::renderer::InvokeGetDataSelected(context,memory,selected,__imp__sub_82AFA708,Gow3GetDataExactInlineLeaf);
    if(sample) {
      readiness_samples.fetch_add(1,std::memory_order_relaxed);
      if(before)readiness_tracked.fetch_add(1,std::memory_order_relaxed);
      if(before&&before->published) {
        readiness_published_before.fetch_add(1,std::memory_order_relaxed);
        const auto after=ReadQueryReadiness(query,memory);
        if(after!=before)readiness_changed_during_call.fetch_add(1,std::memory_order_relaxed);
        else if(context.r3.u32==1)readiness_pending_after_published.fetch_add(1,std::memory_order_relaxed);
        else if(context.r3.u32==0)readiness_completed_after_published.fetch_add(1,std::memory_order_relaxed);
      }
    }
  };
  const auto epoch=query_poll_epoch.load(std::memory_order_acquire);
  if(!(epoch&1)) {invoke(ctx,base);query_poll_policy.Reset();return;}
  gow3::renderer::InvokeQueryPollOriginal(ctx,base,invoke,
      [epoch](uint32_t query,uint64_t caller,uint32_t status) noexcept {
    if(epoch!=query_poll_epoch.load(std::memory_order_acquire)) {query_poll_policy.Reset();return;}
    using Decision=gow3::renderer::QueryPollDecision;
    const auto decision=query_poll_policy.Observe(epoch,query,caller,status);
    if(decision==Decision::Pending||decision==Decision::Yield)
      query_poll_pending.fetch_add(1,std::memory_order_relaxed);
    if(decision==Decision::Completed)query_poll_completed.fetch_add(1,std::memory_order_relaxed);
    if(decision==Decision::Yield) {
      query_poll_yields.fetch_add(1,std::memory_order_relaxed);
      std::this_thread::yield();
    }
  });
}

// The query-report emitter has a separate live event; diagnostic capture v6
// stays unchanged. The simple path emits physical(query[0x1c] + record*32).
// Record one begins an occlusion query; record zero ends it. Multi-tile reports
// are retained in the header and rejected explicitly by the native planner.
REX_EXTERN(__imp__sub_82AFF740);
REX_EXTERN(sub_82AFF740) {
  if (live_enabled.load(std::memory_order_relaxed)) {
    std::lock_guard lock(capture_mutex);
    if (live_sink && guest_memory) {
      gow3::renderer::GraphicsBoundaryEvent event;
      event.sequence=live_sequence++; event.function=profile::d3d::kOcclusionQueryReport; event.frame=frame;
      event.lr=ctx.lr; event.args={ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,
          ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
      for (const auto [kind,address,size] : {
          std::array<uint32_t,3>{1,ctx.r3.u32,kDeviceBytes},
          std::array<uint32_t,3>{104,ctx.r4.u32,0x98}}) {
        gow3::renderer::GraphicsBoundaryBlock block;block.kind=kind;block.address=address;
        if (Readable(address,size)) {
          block.bytes.assign(base+address,base+address+size);
        }
        event.blocks.push_back(std::move(block));
      }
      try { live_sink(std::move(event)); }
      catch (const std::exception& error) {
        REXLOG_ERROR("Native query event failed: {}",error.what());
        getdata_inline_selection.Reset();live_sink={};live_enabled.store(false);DisableQueryPollYield();
      }
    }
  }
  __imp__sub_82AFF740(ctx,base);
}

// Strong symbols replace only the generated weak aliases. All guest side
// effects, return values, and GPU commands still execute via __imp__.
#define GOW3_OBSERVE(name, address) \
  REX_EXTERN(__imp__##name); \
  REX_EXTERN(name) { Observe(address, ctx, base); __imp__##name(ctx, base); }
GOW3_OBSERVE(sub_82AF0790, profile::d3d::kSpecializedDraw) // Specialized draw (ABI under investigation)
GOW3_OBSERVE(sub_82AF99B0, profile::d3d::kInitAuxiliaryCommandBuffer) // Initialize auxiliary draw command buffer
GOW3_OBSERVE(sub_82AECC98, profile::d3d::kSetRenderTarget) // SetRenderTarget
GOW3_OBSERVE(sub_82AED028, profile::d3d::kSetDepthStencilSurface) // SetDepthStencilSurface
#undef GOW3_OBSERVE
#define GOW3_OBSERVE_OWNED(name,address) \
  REX_EXTERN(__imp__##name); \
  REX_EXTERN(name) { \
    const uint32_t device=ctx.r3.u32; \
    Observe(address,ctx,base); \
    struct Scope {CommandOwner previous;~Scope(){command_owner=previous;}} scope{command_owner}; \
    command_owner={last_observed_sequence,device}; \
    __imp__##name(ctx,base); \
  }
GOW3_OBSERVE_OWNED(sub_82AF34A8,profile::d3d::kResolve)
GOW3_OBSERVE_OWNED(sub_82AF16D8,profile::d3d::kClearRect)
#undef GOW3_OBSERVE_OWNED
REX_EXTERN(__imp__sub_82AF6B10);
namespace {
// Runs on the game's render thread right after Present(frame) returns, which is
// just before the game simulates the next frame and reads its input.
void PaceAfterPresent(uint32_t frame) {
  static rex::graphics::FrameLimiter limiter;
  static bool limiter_on_guest = false;
  const bool low_latency = REXCVAR_GET(gow3_low_latency);
  if (capture_processor && low_latency != limiter_on_guest) {
    capture_processor->SetGuestThreadFrameLimiter(low_latency);
    limiter_on_guest = low_latency;
  }
  if (low_latency) {
#if GOW3_HAS_NATIVE_RENDERER
    // One frame in flight in the renderer while this one is simulated.
    if (frame) Gow3WaitForNativeFrame(frame - 1, std::chrono::milliseconds(50));
#endif
    limiter.Wait(REXCVAR_GET(framerate_limit));
  }
  if (REXCVAR_GET(present_anti_lag)) {
    if (capture_processor) capture_processor->UpdateAntiLag(false, uint64_t(frame) + 1);
  }
}
}  // namespace

REX_EXTERN(sub_82AF6B10) {
  if(!Observing()){__imp__sub_82AF6B10(ctx,base);return;}
  uint32_t marker;
  {std::lock_guard lock(capture_mutex);marker=frame;}
  struct Scope {
    uint32_t previous;
    ~Scope(){rex::graphics::ExchangeApiFrameMarker(previous);}
  } scope{rex::graphics::ExchangeApiFrameMarker(marker)};
  Observe(kPresent,ctx,base);__imp__sub_82AF6B10(ctx,base);
  PaceAfterPresent(marker);
}

// Observe completed D3D draws: shader linking and dirty-state flushes have
// resolved the physical vertex formats by this point. Guest work is unchanged.
#define GOW3_OBSERVE_DRAW(name, address) \
  REX_EXTERN(__imp__##name); \
  REX_EXTERN(name) { \
    if (!Observing()) { __imp__##name(ctx, base); return; } \
    const std::array<uint32_t, 8> args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, \
      ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32}; \
    const uint32_t lr = ctx.lr; \
    __imp__##name(ctx, base); \
    Observe(address, ctx, base, 1, &args, lr); \
  }
GOW3_OBSERVE_DRAW(sub_82AEFF00, profile::d3d::kDrawVertices)
GOW3_OBSERVE_DRAW(sub_82AF0318, profile::d3d::kDrawIndexedVertices)
#undef GOW3_OBSERVE_DRAW

// UP allocations precede the caller filling their memory. After-call records
// locate those allocations; their bytes are NOT yet a replayable vertex stream.
#define GOW3_OBSERVE_UP(name, address) \
  REX_EXTERN(__imp__##name); \
  REX_EXTERN(name) { \
    if (!Observing()) { __imp__##name(ctx, base); return; } \
    const std::array<uint32_t, 8> args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, \
      ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32}; \
    const uint32_t lr = ctx.lr; \
    Observe(address, ctx, base); \
    __imp__##name(ctx, base); \
    Observe(address, ctx, base, 1, &args, lr); \
    Gow3RememberUp(address, args, lr, ctx.r3.u32); \
  }
GOW3_OBSERVE_UP(sub_82AEF500, profile::d3d::kBeginVertices)
GOW3_OBSERVE_UP(sub_82AEF9C0, profile::d3d::kBeginIndexedVertices)
#undef GOW3_OBSERVE_UP

// These functions initialize/link existing shader objects. Save the original
// object pointer, execute every original guest effect, then snapshot the final
// exact source. Linking can change physical instructions and the source hash.
#define GOW3_DISCOVER_SHADER(name,pixel) \
  REX_EXTERN(__imp__##name); \
  REX_EXTERN(name) { \
    const uint32_t object=ctx.r3.u32; \
    __imp__##name(ctx,base); \
    DiscoverShader(base,object,pixel); \
  }
GOW3_DISCOVER_SHADER(sub_82AF2628,true)
GOW3_DISCOVER_SHADER(sub_82AF2910,false)
GOW3_DISCOVER_SHADER(sub_82AF9688,false)
#undef GOW3_DISCOVER_SHADER

// Only the shader-cache decoder caller owns this ABI. The original decoder
// runs exactly once with unmodified arguments; inspect successful output while
// its caller still owns the buffer, before it copies one shader and frees it.
REX_EXTERN(__imp__sub_82302670);
REX_EXTERN(sub_82302670) {
  const bool inspect=ctx.lr==profile::game::kShaderCacheDecoderReturn&&ctx.r3.u32==20&&shader_block_discovery_enabled.load(std::memory_order_relaxed);
  const uint32_t output=ctx.r4.u32,bytes=ctx.r5.u32,selected=ctx.r29.u32;
  __imp__sub_82302670(ctx,base);
  if(inspect&&ctx.r3.u32==1)DiscoverShaderBlock(base,output,bytes,selected);
}

std::array<uint64_t,6> Gow3ReadPublishedQueryStatistics() {
  return {published_query_checks.load(),published_query_accepts.load(),published_query_binds.load(),published_query_successes.load(),published_query_pending.load(),published_query_errors.load()};
}
