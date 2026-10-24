#pragma once
#include "native_overflow_prewarm.h"
#include "live_draw.h"
#include "plume_render_interface.h"
#include "state_transfer.h"
#include "query_tracker.h"
#include "owned_payload.h"
#include "pipeline_prewarm.h"
#include "fsi_state_specialization_recipe.h"
#include "fsi_immutable_state_hoist.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace rex::ui::vulkan {struct TemporalUpscaleInputs;}
namespace gow3::renderer {
class LiveOperation;
class NativeBatchCompletionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct CompletedNativeQuery {
    uint32_t frame;
    uint64_t generation,end_event;
    NativeQueryTracker::Result result;
};
enum class NativeDeferredQueryControl : uint8_t { Begin, End };
struct NativeStencilMaterialProgram;
struct NativeEarlyDepthProgram;
struct NativeRetainedOpaqueProgram;
struct NativeRetainedOpaqueResourceWitness;
struct NativeRetainedOpaqueCatalog;
struct NativeRetainedOpaquePhaseCatalog;
struct OpaqueColorOverwriteProgram;
struct ForwardControlFlowProgram;
struct ForwardVertexProgram;
struct ForwardPixelCatalog;
struct FsiStateSpecializationCatalog;
struct SceneExecutorConfig {
    // Explicit complete Native diagnostic input; never enabled by live rendering.
    bool native_completed_frame_capture=false;
    std::string native_frame_capture_sha256;
    // Offline replay diagnostics on a different GPU: log captured-frame state
    // mismatches and continue instead of stopping at the first one.
    bool report_frame_capture_mismatch=false;
    // Offline replay: retire captured query ends without draining all work.
    bool replay_lazy_query_retirement=false;
    // Legacy providers return private bytes. Immutable live snapshots may use
    // payload_snapshot instead; it takes precedence when both are supplied.
    // Draw consumes CPU spans before Execute returns and copies GPU inputs into
    // upload buffers retained through the draw fence. No guest pointer escapes.
    std::function<std::vector<uint8_t>(const std::string&)> payload;
    std::function<OwnedPayload::Snapshot(const std::string&)> payload_snapshot;
    std::function<std::vector<uint8_t>(const std::string&)> shader;
    std::function<std::vector<uint8_t>(const std::string&)> fsi_shader;
    std::function<std::vector<uint8_t>()> fsi_depth_shader;
    // A typed live record owns source-container bytes. Compile only on the
    // same module-cache miss as legacy SPIR-V callbacks; never on cache hits.
    std::function<std::vector<uint8_t>(const CanonicalResourceRef&,bool)> shader_snapshot;
    // Async shaders (optional): false means this shader is still compiling and
    // the draw is skipped; the provider starts or promotes its compilation.
    std::function<bool(const CanonicalResourceRef&,bool fsi)> shader_ready;
    // Async pipelines: ordinary FSI PSOs build on worker threads while their
    // draws are skipped; past the budget the draw waits for the build.
    bool async_pipelines=false;
    std::chrono::milliseconds async_pipeline_budget{250};
    // Best-effort observed recipes; never supplies or authorizes rendering.
    std::function<void(ObservedNativePipeline)> observe_pipeline;
    // Read for each draw so graphics settings apply without discarding images.
    std::function<int32_t()> anisotropy_override;
    uint32_t max_sampler_anisotropy=16; // Zero means the host feature is disabled.
    // GPU writes and the SHADER_READ transition have completed. The recipient
    // must retain the image until its presentation submission completes.
    std::function<void(uint32_t,std::shared_ptr<plume::RenderTexture>,std::array<uint32_t,6>)> present;
    // Jitter scene draws and produce temporal upscaling inputs while the
    // presenter asks for them (see temporal_inputs.h). `temporal` receives a
    // frame's inputs, or nullptr, right before `present` for that frame.
    bool temporal_inputs=false;
    // Diagnostics: log the draws after the last scene-view draw for this many
    // frames in which no HUD was found.
    uint32_t temporal_trace_frames=0;
    // Use the game's velocity pass for per-object motion vectors.
    bool temporal_object_motion=true;
    std::function<void(std::shared_ptr<const rex::ui::vulkan::TemporalUpscaleInputs>)> temporal;
    std::filesystem::path capture,compiled,output;
    std::filesystem::path texture_probe_directory;
    std::string texture_probe_hashes;
    bool diagnostic_zero_history=false;
    bool debug_raw_images=false;
    // File-backed replay keeps complete provenance. Live mode opts in to
    // informational histories; failures, queries, counters and probes stay on.
    bool detailed_reports=false;
    // Opt-in metadata for exact fully overlaid CPU texture misses. Bounded
    // first-seen records never change decoding, ownership or GPU scheduling.
    bool diagnose_full_cpu_texture_overlays=false;
    // OriginalRGBA32F integer endian copy for the exact fully-owned64KiB row.
    bool raw_gpu_float_cpu_textures=false;
    bool defer_cpu_texture_drains=false;
    bool cpu_image_slot_lookup=false;
    // Share immutable CPU image storage across LOD bias/mip-filter variants;
    // original fetch remains the independent view/sampler binding identity.
    bool share_cpu_texture_sampling_variants=false;
    // Pure exact-fetch metadata only; texture bytes and coherence stay live.
    bool cache_texture_layouts=false;
    // Optional same-budget four-way replacement for pure fetch metadata.
    bool associative_texture_layouts=false;
    // Exact sampled page union only; byte ownership and write serial stay live.
    bool compact_cpu_texture_version_pages=false;
    bool coalesced_texture_ranges=false;
    bool reuse_defined_ranges=false;
    bool block_page_versions=false;
    bool range_texture_versions=false;
    // Exact GPU-alias definedness/version memo, scoped to the physical serial.
    bool cache_gpu_alias_states=false;
    // Copy image/sampler writes into a bounded stack batch, then bind the
    // complete consecutive graphics sets once. Descriptor ownership is unchanged.
    bool batch_draw_descriptors=false;
    // Omit only sets absent from both selected SPIR-V modules.
    bool skip_unused_descriptor_sets=false;
    // Exact borrowed-iterator index; ordered ownership and eviction remain unchanged.
    bool shader_lookup_lookaside=false;
    bool pipeline_lookup_lookaside=true;
    bool cpu_image_lookup_lookaside=true;
    bool msaa_single_sample_fallback=true;
    bool vertex_lookup_lookaside=true;
    // Reuse exact already-installed contents of a fully retired draw slot.
    // Requires batching; immutable resource/set generations prevent handle ABA.
    bool cache_draw_descriptors=false;
    // Compare every float bit against a CPU mirror of this already-fenced slot.
    // Shared constants/full mapped-range flush and upload reservations stay exact.
    bool delta_constant_uploads=false;
    bool pinned_null_constant_uploads=false;
    // Experimental reset of only a retired draw slot's private one-buffer pool.
    bool reset_draw_command_pool=false;
    // Optional CPU instrumentation; disabled mode adds no recording clock reads.
    bool time_draw_recording=false;
    // Current default preserved pending qualification. Detailed recording/census
    // enables these pure CPU stage timers even when this switch is disabled.
    bool time_cpu_stages=true;
    // Reuse only fully retired RGBA32F 2D resolve outputs. Idle retention is
    // bounded separately to 16 images / 128 MiB of actual device allocations.
    bool recycle_resolve_images=false;
    bool recycle_resolve_descriptors=false; // Fence-retired compute descriptor leases, bounded per memory instance.
    bool recycle_pass_transfer_descriptors=false; // Separate fence-retired graphics transfer descriptor backings.
    bool recycle_pass_transfer_render_passes=false; // Exact immutable descriptions; framebuffers and views stay fresh.
    bool reuse_resolve_identity_views=false; // Owned load outputs only; stores retain explicit views.
    bool diagnose_texture_eviction_reloads=false; // Fixed diagnostic tombstones/frame timings only.
    // Only exact-description vertex buffers evicted after the full frame fence.
    // Idle allocations are bounded separately to 128 buffers/16 MiB actual bytes.
    bool recycle_vertex_buffers=false;
    // Omit unreachable decoded channels while retaining float32 precision.
    bool compact_resolve_channels=false;
    // Enabled-device proof for borrowed devices (rg32f storage shader only).
    bool storage_image_extended_formats_enabled=false;
    std::vector<std::array<uint32_t,2>> debug_pixels;
    bool native_fsi=false;
    bool retained_fp24_depth_writer=false; // DefaultOFF; pinned depth-only FP24 writes.
    bool retained_opaque_writer=false;
    bool retained_opaque_catalog=false;
    bool retained_opaque_phase=false;
    bool retained_opaque_phase_render_pass_groups=false;
    // Phase-only experiment: one immutable conversion scratch lease on its FIFO.
    bool share_retained_opaque_phase_scratch=false;
    bool share_retained_opaque_phase_attachments=false;
    // Independent phase-only experiment; never waits or changes image admission.
    bool poll_retained_opaque_phase_pressure=false;
    // Experimental fence-retired attachments and one immutable conversion lease.
    bool reuse_retained_opaque_resources=false;
    // Explicit standalone fixture only; no observers retain GPU owners or poll.
    bool test_retained_resource_pool_diagnostics=false;
    bool queue_retained_exports_for_draws=false;
    // Experimental: audited nullPS queries directly read this V2 generation.
    bool retained_shared_depth_queries=false;
    // Independent defaultOFF composition keeps the strong writer+D32 baseline
    // available with retentionOFF. Both paired controls must set this true.
    bool retained_d32_composition=false;
    // Experimental: retain primary authority across an exact disjoint base1440
    // readonly query. Its D32 image remains a separate canonical generation.
    bool retain_primary_across_disjoint_queries=false;
    // Fixture-only preaccept framebuffer allocation refusal; never a GPU hook.
    std::function<void()> test_retained_shared_depth_before_framebuffer;
    std::shared_ptr<const NativeRetainedOpaqueCatalog> retained_opaque_catalog_programs;
    std::shared_ptr<const NativeRetainedOpaquePhaseCatalog> retained_opaque_phase_programs;
    std::shared_ptr<const NativeRetainedOpaqueProgram> retained_opaque_program;
    // Experimental exact full-surface depth+stencil zero fill; other clears use the Native image path.
    bool packed_zero_depth_clear=false;
    // Experimental FP24 normalization + rectangle stencil clear from fully
    // defined packed primary depth; only absent/stale clean host state qualifies.
    bool packed_primary_stencil_clear=false;
    // Separate exact640x360 FP24 source atbase1440. Padding and UNORM24 atlas
    // aliases retain canonical byte/version ownership; current hosts refuse.
    bool packed_secondary_stencil_clear=false;
    // Exact observed UNORM24 atlas: shrink source restoration only. Destination
    // resolve/packing/history and sampled-generation ownership stay canonical.
    bool atlas_depth_region_restores=false;
    // Closed atlas depth-one clear; preserve pre-store host floats in an owned receipt.
    bool fused_atlas_depth_clear=false;
    // Full canonical RGBA depth resolve source; current host images stay Native D32.
    bool direct_atlas_depth_resolves=false;
    // Diagnostic fused packed decode/resolve MRT; keeps physical store unchanged.
    bool fused_packed_atlas_resolves=false;
    // Optional full physical store plus exact post-pack sampled-image output.
    // The immutable receipt is usable only while its physical version matches.
    bool canonical_resolve_stores=false;
    // Diagnostic exact UNORM10 primary color resolves; stale packed sources
    // bypass only the identity RGBA32F host copy, with no host token promotion.
    bool direct_packed_color_resolves=false;
    bool skip_full_color_clear_restore=false;
    // Diagnostic module variant. Immutable for this Scene; persistent compiler
    // output stays unchanged. Requires the audited coherent native FSI epilogue.
    bool implicit_fsi_barriers=false;
    // Separate optional provider for exact FSI storage operations. Null-PS
    // builtins stay canonical so the proven D32 path keeps its original key.
    // Optional separately compiled non-null FSI epilogue; canonical nullPS is unchanged.
    NativeFsiHoistMode fsi_hoist_mode=NativeFsiHoistMode::Disabled;
    std::string fsi_hoist_version;
    bool exact_fsi_stores=false;
    std::string exact_fsi_stores_version;
    bool specialize_fsi_pipeline_state=false;
    // Unqueried materials only; canonical depth/query sources keep priority.
    bool fsi_state_material_composition=false;
    // Default-off admission of exact canonical pairs from the disabled early catalogue.
    bool fsi_state_inactive_early_catalog=false;
    // Pinned forward guest CFG composed with exact FSI state; requires state mode.
    bool fsi_state_forward_cfg=false;
    std::shared_ptr<const FsiStateSpecializationCatalog> fsi_state_programs;
    // First-seen tuple only. Returned stable atomic counters belong to the
    // runtime cache; resident hits neither call this callback nor copy sources.
    std::function<std::atomic<uint64_t>*(const NativeFsiStateRecipe&)> observe_fsi_state;
    // Pinned unqueried depth-only GE/write variant; dynamic proof per draw.
    bool builtin_atomic_depth=false;
    // Optional exact null-PS read-only query builtin. Startup variants only;
    // gameplay misses use ordinary Native. Canonical shader/recipe caches stay unchanged.
    bool builtin_readonly_depth_queries=false;
    bool native_early_depth_catalog=false; // Additional independently certified material pairs; requires shipping aid.
    bool native_early_depth=false; // DefaultOFF; immutable lower-bound rejection aid, canonical packed output remains.
    // V1 only: retain an immutable guard across existing completion/query-control
    // work only when its exact primary token remains unchanged after that work.
    bool reuse_early_depth_readonly_boundaries=false;
    // DefaultOFF: retain the audited material guard across proved monotonic depth writes.
    bool native_early_depth_material_retention=false;
    bool native_readonly_depth_attachment=false; // DefaultOFF; exact quantized readonly query only.
    bool host_query_reset=false; // DefaultOFF; reset only fence-retired query ranges on the host.
    bool native_readonly_depth_read_only_pass=false; // DefaultOFF; preserve D32 with LOAD+STORE_NONE/DEPTH_READ.
    bool native_readonly_depth_lower_bound_query=false; // DefaultOFF; hardware rejection plus exact packed discard, late queries.
    bool diagnostic_readonly_query_groups=false; // Offline replay only; synthesize queries around captured readonly VS groups.
    bool native_readonly_depth_conservative=false; // DefaultOFF; exact half-depth bound for the same D32 query.
    bool native_readonly_depth_no_write_metadata=false; // DefaultOFF; preserve packed versions for audited no-write queries.
    bool native_readonly_depth_skip_edram_barrier=false; // DefaultOFF; audited selected D32 has no packed-buffer access.
    // Startup-only compiled, separately keyed experiment. Empty program or
    // missing prewarmed resident pipeline always uses the ordinary Native path.
    bool opaque_color_overwrite=false;
    std::shared_ptr<const OpaqueColorOverwriteProgram> opaque_color_overwrite_program;
    // Whole material and FSI epilogue retained; only the audited forward guest
    // PC dispatcher changes. Startup prewarm and resident-only draw selection.
    bool forward_control_flow=false;
    std::shared_ptr<const ForwardControlFlowProgram> forward_control_flow_program;
    // Pinned straight-line VS dispatcher; complete outputs/fetch code retained.
    bool forward_vertex_control_flow=false;
    std::shared_ptr<const ForwardVertexProgram> forward_vertex_program;
    // Bounded color-only source catalog; original vertex and late FSI retained.
    bool forward_pixel_catalog=false;
    std::shared_ptr<const ForwardPixelCatalog> forward_pixel_catalog_programs;
    // Combine only adjacent dummy-attachment and packed-EDRAM dependencies.
    // Sampled-image barriers keep their original preparation ordering.
    bool coalesce_draw_barriers=false;
    // Omit only already-readable immutable sampled images. All initial or
    // intervening write/compute/copy transitions retain the original barrier.
    bool skip_readonly_sample_barriers=false;
    // Narrow only prior FSI fragment/attachment dependencies; copy/compute,
    // unknown metadata and active render passes retain full barriers.
    bool precise_fsi_barriers=false;
    // The FSI attachment carries no color/depth data. Reuse a larger raster
    // extent (at most 32 MiB) while clipping with the original guest extent.
    bool retain_fsi_raster_extent=false;
    // Scene-only dummy color attachment: retain all barriers/pass boundaries,
    // discard unobserved attachment contents. Non-FSI framebuffers ignore it.
    bool discard_dummy_fsi_attachment=false;
    // Optional attachmentless host rasterization. Packed EDRAM, fragment
    // interlock, queries and original pass boundaries remain authoritative.
    bool attachmentless_fsi=false;
    // Fixture-only preaccept framebuffer-allocation refusal. A thrown error
    // must choose the ordinary Native attachment before recording this draw.
    std::function<void()> test_attachmentless_fsi_before_create;
    // Proof of enabled features for a borrowed device, not hardware support.
    bool ordered_pixel_interlock_enabled=false;
    // One retains synchronous diagnostic behavior. Packed EDRAM draws may be
    // grouped into up to 64 ordered command lists with one completion fence.
    uint32_t max_draw_batch_size=1;
    // Opt-in physical command-buffer grouping; logical draw/query/FIFO owners
    // retain their existing64/128 bounds and one exact serial per draw.
    bool multi_draw_command_buffers=false;
    // Closed canonical null-writer cohort: keep ordered-interlock draws in one
    // subpass. Logical accepted owners and original target16 remain unchanged.
    bool packed_depth_render_pass_groups=false;
    // Any consecutive FSI draws whose EDRAM planes keep one injective
    // pixel/word mapping share an ordered-interlock subpass without barriers.
    bool fsi_draw_pass_groups=false;
    // Also admit occlusion-query draws (requires host query reset).
    bool fsi_draw_pass_queries=false;
    // Pinned unchanged primary material draws, disjoint packed color/depth
    // planes and immutable sampled resources. Query/transfer boundaries remain.
    bool packed_color_render_pass_groups=false;
    // Hardware render targets: grouped draws into the same framebuffer keep
    // one render pass instance open while both attachments stay in their
    // attachment layouts. On by default since 2026-10-22; the 2026-10-18 hang
    // predates the framebuffer cache fix (2026-10-20).
    bool hardware_render_pass_continuation=true;
    // Separate exact alpha/additive variant8 cohort; parent grouping required.
    bool packed_color_blend_pass_groups=false;
    // Separate exact action V0/V8 pair; requires the existing packed parent.
    bool packed_color_action_pass_groups=false;
    // Exact immutable D32 queries, with fence-retired host resets. Query
    // identities and logical FIFO owners remain separate inside the pass.
    bool readonly_query_render_pass_groups=false;
    // Fixture-only fault after a fully recorded grouped draw, before logical
    // admission. Its parameter is the number of older accepted group children.
    std::function<void(uint32_t)> test_grouped_draw_before_enqueue;
    // Persistent exact-key pipeline/module caches. Smaller limits are useful
    // for pressure fixtures; production cannot exceed these hard bounds.
    uint32_t max_pipeline_cache_entries=2048;
    // Optional bounded headroom for material specialization. It changes cache
    // residency only, never the admission limit of specialized modules/PSOs.
    bool fsi_state_cache_headroom=false;
    uint32_t max_shader_cache_entries=1024;
    uint64_t max_shader_cache_bytes=64u*1024*1024;
    // Bounded extra residency for resources absent from the current frame.
    // Zero budget restores one-unused-frame eviction. Current-frame owners
    // still live through the existing full frame fence.
    // Textures stay resident longer than vertices: re-uploading them after a
    // camera pan flushes pending draws and may read back GPU-owned bytes.
    uint64_t max_dormant_texture_bytes=128u*1024*1024;
    uint32_t max_dormant_texture_entries=512;
    uint32_t max_dormant_texture_epochs=120;
    uint64_t max_dormant_vertex_bytes=64u*1024*1024;
    uint32_t max_dormant_vertex_entries=2048;
    uint32_t max_dormant_cache_epochs=120;
    // Opt in only with bounded SDK publication tickets under native authority.
    // Replay and shadow callers retain synchronous query-end behavior.
    bool defer_native_queries=false;
    // Publish an exact completed FIFO prefix even while later GPU work remains.
    // Requires deferred queries and the unified queue; legacy defaults remain.
    bool retire_completed_query_prefixes=false;
    // Logical metadata/tickets are separate from physical GPU slots.
    uint32_t max_deferred_queries=64;
    // Combine GPU-only stages within each fenced clear/resolve operation.
    bool batch_transfers=false;
    // Opt-in FIFO shared by owned raster and GPU-only transfer operations.
    bool unified_gpu_queue=false;
    // Bounded stacked GPU aliases decode and assemble on the original FIFO.
    // Exact RGBA32F output; unsupported shapes retain synchronous native load.
    bool queued_volume_alias_loads=false;
    // Experimental immutable page updates share the FIFO. Query-report
    // overlaps, larger updates, memory_copy and CPU readbacks remain eager.
    bool queue_cpu_memory_writes=false;
    bool diagnose_marker_query_cohorts=false; // Pure metadata census under gpu_command_summary.
    // One checked, fully fenced 4 KiB CPU upload owner; scheduling is unchanged.
    bool reuse_cpu_write_upload=false;
    // Exact completed CPU-upload pages may satisfy a whole native overlay.
    bool forward_cpu_upload_receipts=false;
    // Diagnostic overlap defaults to two fences and 64 total owners.
    bool async_gpu_submissions=false;
    // Checked driver calls overlap recording; original GPU ownership/fences stay bounded.
    bool submission_worker=false;
    // Optional 128 owners permit two 64-list submissions. Synchronous mode
    // retains its existing 64-owner limit regardless of this preference.
    uint32_t async_submission_capacity=64;
    // Optional four-fence overlap changes the scheduling window only. Physical
    // draw/query/list and scratch capacities and per-batch targets stay exact.
    uint32_t max_inflight_batches=2;
    // Diagnostic GPU intervals and separate host phases; no extra fence waits.
    bool gpu_timestamps=false;
    // Diagnostic batch composition only; never splits mixed GPU intervals.
    bool gpu_batch_attribution=false;
    // Sample whole closed submissions; does not enable old batch/draw timestamps.
    bool gpu_physical_timestamps=false;
    uint32_t gpu_physical_first=0,gpu_physical_end=6000,gpu_physical_interval=30;
    // Optional inline timestamp pairs on every draw of API frames divisible by16.
    bool gpu_draw_timestamps=false;
    // Offline capture diagnostics only; live runtime leaves sparse sampling unchanged.
    bool gpu_draw_timestamps_every_frame=false;
    // Standalone replay target only; gameplay builds contain no census hooks.
    bool replay_fragment_invocations=false;
    // Optional startup-prepared material shader; no fragment counters.
    bool native_stencil_material_cutoff=false;
    std::shared_ptr<const NativeStencilMaterialProgram> native_stencil_material_program;
    // Counter diagnostics compile only into the standalone replay target.
    uint32_t replay_stencil_material_mode=0;
    std::shared_ptr<const NativeStencilMaterialProgram> replay_stencil_material_program;
    bool restore_depth_stencil_export=false;
    bool shader_stencil_export_enabled=false; // Borrowed device's enabled extension, not hardware support.
    // Old canonical image/view generations stay owned by their consuming draw
    // fences. Pressure fences all work before exceeding this extra byte budget.
    uint64_t max_stale_alias_bytes=128u*1024*1024;
};

// Ordered native rendering with history retained between frames. The current
// executor retains every pending draw's resources until its batch fence. All calls
// must be serialized. A borrowed device/queue must outlive this executor.
class SceneExecutor {
public:
    explicit SceneExecutor(SceneExecutorConfig config,plume::RenderDevice* device=nullptr,
                           plume::RenderCommandQueue* queue=nullptr);
    ~SceneExecutor();
    SceneExecutor(const SceneExecutor&)=delete;
    SceneExecutor& operator=(const SceneExecutor&)=delete;
    void Execute(const nlohmann::json& operation);
    void Execute(const TypedLiveDraw& operation);
    void Execute(const TypedLiveQuery& operation);
    // Startup only, before any guest GPU mutation. Populates this real Scene's
    // exact caches without creating draws, history, query samples or ownership.
    // Optional exact startup variant, called after generic prewarm. No live
    // shader compilation on misses; missing source/program uses ordinary Native.
    bool PrewarmStencilMaterialPipeline(const NativeFsiPipelineRecipe&,std::span<const uint32_t> vertex,
        std::span<const uint32_t> pixel);
    void PrewarmFsiPipeline(const NativeFsiPipelineRecipe&,std::span<const uint32_t> vertex,
        std::span<const uint32_t> pixel);
    // Startup-only canonical PSO with local missing shader modules. Persistent
    // shader membership/accounting and the original optional owners stay exact.
    NativeOverflowPrewarmResult PrewarmFsiOverflowPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex,std::span<const uint32_t> pixel);
    // Optional startup alternative; no source compilation or GPU mutation.
    bool PrewarmRetainedSharedDepthQueryPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex,std::span<const uint32_t> original_builtin);
    bool PrewarmRetainedFp24DepthPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex,std::span<const uint32_t> original_builtin);
    bool PrewarmNativeEarlyDepthPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> canonical_vertex,std::span<const uint32_t> canonical_pixel,
        std::shared_ptr<const NativeEarlyDepthProgram>);
    bool PrewarmNativeEarlyDepthCatalogPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> canonical_vertex,std::span<const uint32_t> canonical_pixel,
        std::shared_ptr<const NativeEarlyDepthProgram>);
    bool PrewarmNativeReadonlyDepthPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex,std::span<const uint32_t> original_builtin);
    bool PrewarmReadonlyDepthQueryPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex,std::span<const uint32_t> pixel);
    bool PrewarmOpaqueColorOverwritePipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex);
    bool PrewarmForwardControlFlowPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex);
    bool PrewarmForwardVertexPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex);
    bool PrewarmFsiStatePipeline(const NativeFsiStateRecipe&,std::span<const uint32_t> vertex);
    bool PrewarmForwardPixelCatalogPipeline(const NativeFsiPipelineRecipe&,
        std::span<const uint32_t> vertex);
    void InitializeMemory(uint32_t address,std::span<const uint8_t> bytes);
    void InitializeEdram(std::span<const uint8_t> bytes);
    // Capture the actual reference EDRAM before native rendering takes ownership.
    // This is a full same-device transfer, not a synthesized zero initialization.
    void InitializeEdramFromBuffer(VkBuffer source);
    // Commit dirty attachment interpretations into packed EDRAM without
    // copying ownership ranges. Call before a new transaction's mutation
    // baseline; ExportState additionally snapshots ranges for state handback.
    void FlushCommittedState();
    // Before a non-draw operation; hardware render targets skip image commits.
    void FlushCommittedStateAtOperation();
    void PrepareCommittedStateForDraw(const TypedLiveDraw&);
    bool PrepareCommittedStateForDeferredQuery(NativeDeferredQueryControl);
    bool PrewarmRetainedOpaquePhasePipeline(const NativeFsiPipelineRecipe&,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel);
    bool PrewarmRetainedOpaquePipeline(const NativeFsiPipelineRecipe&,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel);
    NativeRetainedOpaqueResourceWitness RetainedResourcePoolWitnessForFixture()const;
    // Standalone fixture only: retains the displaced real owner until Scene destruction.
    // No runtime setting or gameplay caller may invoke this mutation.
    struct RetainedOpaquePhaseOwnerWitness {
        bool removed_pipeline=false,distinct_vulkan_modules=false,certified_bytes=false;
        bool dirty=false,pipeline_certificate_unchanged=false;
        uintptr_t old_shader=0,new_shader=0,old_pipeline=0;
        uintptr_t certified_vertex=0,certified_pixel=0;
        uint64_t accepted_work=0,completed_work=0,generation=0,color_version=0,depth_version=0;
    };
    RetainedOpaquePhaseOwnerWitness MutateRetainedOpaquePhaseOwnerForFixture(
        const NativeFsiPipelineRecipe&,bool remove_pipeline=false);

    // Explicit standalone diagnostic only. The displaced module stays alive;
    // the actual old PSO and its creation-time certificate are never rewritten.
    struct PackedColorVariant8OwnerWitness {
        uintptr_t old_shader=0,new_shader=0,pipeline=0;
        uint64_t accepted_work=0,completed_work=0,mutation_serial=0,displaced_bytes=0;
        bool distinct_vulkan_modules=false,old_module_certified=false,new_module_certified=false;
        bool pipeline_certified_before=false,pipeline_certified_after=false;
        bool pipeline_unchanged=false,frontier_unchanged=false,mutation_unchanged=false;
    };
    PackedColorVariant8OwnerWitness ReplacePackedColorVariant8ModuleForFixture(
        const NativeFsiPipelineRecipe&,const NativeFsiStateSpecialization&,std::span<const uint32_t> replacement);
    PackedColorVariant8OwnerWitness ReplacePackedColorBlendVariant8ModuleForFixture(
        const NativeFsiPipelineRecipe&,const NativeFsiStateSpecialization&,std::span<const uint32_t> replacement);

    void FlushPendingDraws();
    bool HasPendingDraws() const;
    bool HasPendingGpuWork() const;
    void FlushGpuWork();
    // Read-only diagnostic POD: no submission, polling, reporting or clocks.
    struct QueueCompletionSnapshot {
        bool available=false,timings_enabled=false;
        uint64_t waits=0;
        double fence_wait_ms=0;
    };
    QueueCompletionSnapshot ObserveQueueCompletion() const noexcept;
    // CP-only completed-frame observation. Reads existing EndFrame snapshots
    // and queue counters; never seals, polls, serializes, allocates or clocks.
    // Completion is legacy fence wait + queue-entry retirement, not GPU time;
    // it excludes standalone transfers/presentation and the Scene callback.
    struct CompletedFrameSnapshot {
        bool available=false;uint32_t frame=0;
        uint64_t draws=0;double completion_ms=0;
        uint64_t cpu_uploads=0,cpu_payload_bytes=0,gpu_overlay_bytes=0;
        uint64_t overlay_copy_regions=0,overlay_readback_bytes=0,gpu_alias_loads=0;
        bool queue_available=false;
        uint64_t queue_submissions=0,queue_waits=0,queue_polls=0,queue_poll_retirements=0;
    };
    CompletedFrameSnapshot ObserveCompletedFrame(uint32_t expected_frame) const noexcept;
    struct AcceptedWorkSnapshot {
        uint64_t generation=0,draws=0,transfers=0,accepted=0,completed=0;
        bool available=false;
    };
    AcceptedWorkSnapshot ObserveAcceptedWork() const noexcept;
    // Pure CP diagnostic: no collection, polling, result readback or publication.
    struct MarkerQueryReadiness {uint32_t closed=0,ready=0,unknown=0;bool active=false,available=false;};
    MarkerQueryReadiness ObserveMarkerQueryReadiness() const noexcept;
    enum class QueryWorkStage : uint8_t {Unknown,Staged,Submitted,Completed,Count};
    struct QueryWorkObservation {
        QueryWorkStage stage=QueryWorkStage::Unknown;
        uint64_t required=0,accepted=0,submitted=0,completed=0;
    };
    // No queue action or result readback. Identity includes the actual report.
    QueryWorkObservation ObserveQueryWork(uint32_t frame,uint64_t generation,uint32_t address) const noexcept;
    // QueryPressure only: full accepted-work fence, without exposing canonical
    // packed memory. All CPU/read/resolve/export/public strong drains stay strong.
    // Requires retain_primary_across_disjoint_queries; never a partial fence.
    void CompleteGpuWorkForQueryCompletion();
    // Seal/submit and poll owned work without requiring its newest fence.
    // Existing bounded pressure may retire an older actual submission.
    void SubmitGpuWork();
    // Includes sealed records whose GPU work already completed. The optional
    // prefix mode uses each end's all-work frontier; otherwise requires a drain.
    uint32_t DeferredQueryCount() const;
    // Nonblocking polling neither submits nor waits for a newer batch.
    void PollCompletedQueries();
    // Logical-capacity pressure: submit existing staging if necessary, then
    // wait only through the oldest unconsumed query's covering fence.
    void FlushOldestQuery();
    std::vector<CompletedNativeQuery> TakeCompletedQueries();
    // Synchronous committed state, valid while this executor/device live.
    // Export flushes any dirty attachment interpretations into packed EDRAM.
    NativeRendererState ExportState();
    // Changes before authoritative writes; an exception after it changes must
    // not replay that operation through a fallback renderer.
    uint64_t mutation_serial() const;
    std::vector<uint8_t> ReadPhysical(uint32_t address,uint32_t bytes);
    // Explicit offscreen diagnostic only; requires fully defined10MiB history.
    std::vector<uint8_t> ReadFullEdram();
    std::optional<NativeQueryTracker::Result> ActiveQuery() const;
    std::optional<NativeQueryTracker::Metadata> ActiveQueryMetadata() const;
    // Refreshes typed diagnostic counters without submitting or waiting for GPU
    // work. Repeated observations with no intervening work reuse the snapshot.
    const nlohmann::json& report() const;
    nlohmann::json GpuBatchAttributionReport(bool details=false) const;
    // Cold observation only: never seals, submits, polls or waits.
    nlohmann::json GpuPhysicalTimingReport(bool details=false) const;
    nlohmann::json GpuDrawTimingReport(bool details=false) const;
    nlohmann::json FragmentInvocationReport() const;
    // File-backed diagnostic driver; uses the same Execute path as live input.
    void RunCapture(uint32_t last_event=UINT32_MAX);
    // Diagnostic whole-capture oracle. The caller retains every operation and
    // its immutable frame resources until return. Event/type/frame identity must
    // exactly match scene.json; history, issues and outputs use the normal path.
    void RunLiveCapture(std::span<const LiveOperation* const> operations,
        uint32_t last_event=UINT32_MAX,bool json_adapter=false,bool wrap_frame_queries=false);
    // Explicit diagnostic lifetime witness: initializes the exact supplied capture
    // history, executes a bounded typed prefix, and leaves accepted retained work
    // for the normal owned Scene destructor. No frame-query wrapping is allowed.
    void RunLiveCapturePartialForRetainedFixture(std::span<const LiveOperation* const>,uint32_t last_event);
private:
    struct State;
    const uint64_t diagnostic_generation_;
    std::unique_ptr<State> state_;
};
}
