#include "renderer/game_profile.h"
#include "published_query_results.h"
#include "runtime_bridge.h"
#include "gow3_renderer_mode.h"
#include "renderer/gears_bridge.h"
#include "renderer/native/live_device.h"
#include "renderer/native/live_plan.h"
#include "renderer/native/completed_frame_disposer.h"
#include "renderer/native/memory_coherence.h"
#include "renderer/native/memory_export_copy.h"
#include "renderer/native/cpu_write_policy.h"
#include "renderer/native/packet_ownership.h"
#include "renderer/native/scene_executor.h"
#include "renderer/native/query_comparison.h"
#include "renderer/native/shader_compiler.h"
#include "renderer/native/fsi_exact_stores_compiler.h"
#include "renderer/native/fsi_immutable_state_hoist_compiler.h"
#include "renderer/native/retained_opaque_compiler.h"
#include "renderer/native/retained_opaque_writer.h"
#include "renderer/native/retained_opaque_catalog.h"
#include "renderer/native/retained_fp24_depth.h"
#include "renderer/native/native_early_depth_shader.h"
#include "renderer/native/native_early_depth_catalog.h"
#include "renderer/native/retained_opaque_catalog_compiler.h"
#include "renderer/native/retained_opaque_phase_catalog.h"
#include "renderer/native/retained_opaque_phase_compiler.h"
#include "renderer/native/opaque_color_overwrite_compiler.h"
#include "renderer/native/stencil_material_cutoff_compiler.h"
#include "renderer/native/forward_control_flow_compiler.h"
#include "renderer/native/forward_vertex_compiler.h"
#include "renderer/native/forward_pixel_catalog_compiler.h"
#include "renderer/native/fsi_state_specialization_compiler.h"
#include "renderer/native/fsi_state_material_composition.h"
#include "renderer/native/fsi_material_shader_cache_headroom.h"
#include "renderer/native/fsi_state_forward_cfg_compiler.h"
#include "renderer/native/fsi_state_forward_cfg.h"
#include "renderer/native/fsi_state_recipe_cache.h"
#include "renderer/native/shader_prewarm_queue.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/pipeline_prewarm.h"
#include "renderer/native/fsi_action_state_source_lookup.h"
#include "renderer/native/native_frame_capture.h"
#include "renderer/native/frame_journal.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/pipeline_startup_task.h"
#include <rex/graphics/shader_storage_progress.h>
#include <future>
#include <map>
#include <set>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <unordered_set>

REXCVAR_DECLARE(bool, gpu_command_summary);
REXCVAR_DECLARE(bool, gow3_native_block_shader_prewarm);
REXCVAR_DECLARE(bool, native_direct_execution);
REXCVAR_DECLARE(bool, gpu_native_marker_operands);

REXCVAR_DEFINE_BOOL(gow3_native_hardware_render_targets, true, "Graphics",
                   "Render into real GPU render targets with hardware depth, stencil and blending instead of emulating Xbox EDRAM in shaders")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_direct_execution, true, "Graphics",
                   "Native renderer executes the game's D3D calls directly, without emulating the Xbox GPU command processor")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_direct_dependency_bound, true, "Renderer diagnostics",
                   "Direct execution: also cut batches before they exceed the flat prefix dependency bound")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_async_planner, true, "Renderer diagnostics",
                   "Plan direct-execution draws on a dedicated thread; texture snapshots, memory watches and query registration stay at the API point")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_direct_batch_operations, 32, "Renderer diagnostics",
                    "Direct execution: hand planned operations to the executor in batches of at least this many")
    .range(1,1024).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_published_query_results, true, "Renderer diagnostics",
                   "Allow authenticated blocking occlusion consumers to use actual fenced query results before unrelated serial completion")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_native_query_readiness_census, false, "Renderer diagnostics",
                   "Observe pending GetData calls after verified current-generation query publication; never changes results")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_native_query_wait_census, false, "Renderer diagnostics",
                   "Observe authenticated query polls and their GPU submission frontier without changing results or scheduling")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_native_shared_providers, false, "Renderer diagnostics",
                   "Retain immutable native callback holders without copying callables for each packet")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_batch_draw_descriptors, false, "Renderer diagnostics",
                   "Batch native per-draw texture descriptor writes and consecutive graphics set bindings")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_skip_unused_descriptor_sets, false, "Renderer diagnostics",
                   "Omit bindings absent from both selected Native SPIR-V modules; unknown declarations retain all sets")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_pipeline_lookup_lookaside, true, "Renderer diagnostics",
                   "Reuse exact pipeline map lookups through a hashed slot index")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_cpu_image_lookup_lookaside, true, "Renderer diagnostics",
                   "Reuse exact CPU image map lookups through a hashed slot index")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_msaa_single_sample_fallback, true, "Renderer diagnostics",
                   "Store 2x/4x multisampled render targets as single-sample instead of stopping the native renderer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_shader_lookup_lookaside, true, "Renderer diagnostics",
                   "Use a bounded exact-key iterator index for resident Native shader lookups")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_cache_draw_descriptors, false, "Renderer diagnostics",
                   "Reuse exact installed native draw-slot descriptors with immutable binding generations")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_pinned_null_constant_uploads, false, "Renderer diagnostics",
                   "Omit unread float constants only for two certified canonical null-pixel depth families")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_delta_constant_uploads, true, "Renderer diagnostics",
                   "Write only changed exact float-constant blocks in fully retired native draw slots")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_reset_draw_command_pool, false, "Renderer diagnostics",
                   "Reset only the retired native draw slot's private command pool")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_packed_zero_depth_clear, false, "Renderer diagnostics",
                   "Fill proven full-surface packed depth/stencil zero clears directly")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_packed_primary_stencil_clear, true, "Renderer diagnostics",
                   "Normalize and clear stencil from packed primary FP24 only when the host depth image is absent or stale")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_packed_secondary_stencil_clear, false, "Renderer diagnostics",
                   "Normalize and clear stencil from packed secondary FP24 only when the host image is absent or stale")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fused_atlas_depth_clear, true, "Renderer diagnostics",
                   "Fuse the exact atlas depth-one clear while preserving original host depth and stencil")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_direct_atlas_depth_resolves, true, "Renderer diagnostics",
                   "Resolve the exact depth atlas from canonical RGBA while preserving depth and stencil packing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fused_packed_atlas_resolves, false, "Renderer diagnostics",
                   "Fuse canonical packed UNORM24 atlas decode and resolve with a private stencil MRT")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_canonical_resolve_stores, false, "Renderer diagnostics",
                   "Produce exact immutable sampled images alongside eligible physical resolve stores")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_direct_packed_color_resolves, false, "Renderer diagnostics",
                   "Resolve exact primary UNORM10 color directly from a full canonical packed decode")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_atlas_depth_region_restores, false, "Renderer diagnostics",
                   "Limit canonical depth atlas source restoration to an exact resolve rectangle")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_skip_full_color_clear_restore, true, "Renderer diagnostics",
                   "Skip stale working-image restores before complete color-only clears")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_verbose_frame_reports, true, "Renderer diagnostics",
                   "Log detailed Native counters during gameplay; disabling keeps interval summaries and final health counters")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_frame_journal, "", "Renderer diagnostics",
                     "Save bounded existing Native frame intervals and counters on orderly shutdown")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(gow3_native_frame_capture, "", "Renderer diagnostics",
                     "Save two complete Native frames with actual GPU history to a new directory")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_frame_capture_trigger, "", "Renderer diagnostics",
                     "Arm a Native frame capture when this file exists")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_frame_capture_pixel, "", "Renderer diagnostics",
                     "Arm after a completed frame uses this exact pixel shader source digest")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_native_time_cpu_stages, false, "Renderer diagnostics",
                   "Record CPU draw, texture and operation stages; disabling preserves health and completion timers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_native_time_draw_recording, false, "Renderer diagnostics",
                   "Measure native command recording and submission CPU costs")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_attachmentless_fsi, false, "Renderer diagnostics",
                   "Rasterize Native packed EDRAM draws without an unused host color attachment")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_discard_dummy_fsi_attachment, true, "Renderer diagnostics",
                   "Discard only unobserved Native FSI dummy color attachment load/store contents")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_hardware_render_pass_continuation, true, "Renderer diagnostics",
                   "Hardware render targets: keep one render pass open across grouped draws into the same framebuffer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_packed_color_render_pass_groups, false, "Renderer diagnostics",
                   "Keep pinned unchanged primary material draws in a bounded ordered-interlock subpass")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_packed_color_blend_pass_groups, false, "Renderer diagnostics",
                   "Group only byte-certified exact alpha/additive materials in packed passes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_packed_color_action_pass_groups, false, "Renderer diagnostics",
                   "Group only byte-certified exact action materials in packed passes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_packed_depth_render_pass_groups, false, "Renderer diagnostics",
                   "Keep certified canonical packed depth writers in one bounded ordered-interlock subpass")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fsi_draw_pass_groups, true, "Renderer diagnostics",
                   "Keep consecutive FSI draws with one injective EDRAM mapping in one ordered-interlock subpass")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fsi_draw_pass_queries, false, "Renderer diagnostics",
                   "Let occlusion-query draws share FSI draw passes (host query reset only)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_async_shaders, false, "Graphics",
                   "Skip a draw while its shader is still compiling instead of stalling (objects may appear a few frames late)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_async_pipelines, false, "Graphics",
                   "Skip a draw while its GPU pipeline is being built instead of stalling (objects may appear a few frames late)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_async_shader_budget_ms, 250, "Graphics",
                     "Async shaders: after this long waiting for one shader, block on it like the synchronous path")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_multi_draw_command_buffers, false, "Renderer diagnostics",
                   "Record consecutive resident native draws in bounded shared command buffers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_stencil_material_cutoff, false, "Renderer diagnostics",
                   "Use one startup-prepared exact packed stencil cutoff without shader counters")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_opaque_color_overwrite, false, "Renderer diagnostics",
                   "Use startup-prepared opaque format3 complete packed-color overwrite")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_forward_control_flow, false, "Renderer diagnostics",
                   "Use startup-prepared audited forward-only guest shader control flow")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_forward_vertex_control_flow, false, "Renderer diagnostics",
                   "Use startup-prepared pinned straight-line guest vertex shader control flow")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_forward_pixel_catalog, false, "Renderer diagnostics",
                   "Use startup-prepared bounded color-only forward shader catalog")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_specialize_fsi_pipeline_state, false, "Renderer diagnostics",
                   "Use resident startup-prepared Native FSI PSOs specialized by exact packed raster state")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fsi_state_material_composition, false, "Renderer diagnostics",
                   "Specialize eligible unqueried Native materials while preserving canonical depth and query paths")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fsi_state_inactive_early_catalog, false, "Renderer diagnostics",
                   "Admit the two exact canonical material pairs only while their early-depth catalogue is disabled")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fsi_action_state_overflow_lookup, false, "Renderer diagnostics",
                   "Prepare only the exact observed action material state from its successfully prepared overflow generic")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fsi_state_cache_headroom, false, "Renderer diagnostics",
                   "Reserve bounded shader-cache headroom for Native material specialization")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fsi_state_forward_cfg, false, "Renderer diagnostics",
                   "Use resident pinned forward guest CFG composed with exact Native FSI state")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_readonly_depth_read_only_pass, false, "Renderer diagnostics",
                   "Preserve certified Native D32 query depth using LOAD+STORE_NONE and read-only attachment access.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_readonly_depth_lower_bound_query, false, "Renderer diagnostics",
                   "Use conservative hardware depth rejection while retaining exact packed comparison and late query counts")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_readonly_depth_skip_edram_barrier, false, "Renderer diagnostics",
                   "Skip packed EDRAM barrier only for audited selected D32 queries")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retained_fp24_depth_writer, false, "Renderer diagnostics",
                   "Use pinned retained FP24 depth-only hardware writers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retained_opaque_writer, false, "Renderer diagnostics",
                   "Retain audited packed-color and exact D32 opaque hardware attachments")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_queue_retained_exports_for_draws, false, "Renderer diagnostics",
                   "Queue retained packed exports before ordinary typed GPU draws without an eager host drain")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_reuse_retained_opaque_resources, false, "Renderer diagnostics",
                   "Reuse actual fence-retired retained attachments and immutable conversion resources")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retained_d32_composition, false, "Renderer diagnostics",
                   "Compose one exact primary writer with canonical D32 queries; query completion remains strong")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retain_primary_across_disjoint_queries, false, "Renderer diagnostics",
                   "Retain exact primary attachments across disjoint deferred queries and full query-pressure fences")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retained_shared_depth_queries, false, "Renderer diagnostics",
                   "Read audited visibility queries directly from a dirty retained V2 depth generation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retained_opaque_catalog, false, "Renderer diagnostics",
                   "Retain three audited packed-color and exact D32 material families")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retained_opaque_phase, false, "Renderer diagnostics",
                   "Retain the separately certified captured opaque phase using startup-resident hardware attachments")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retained_opaque_phase_render_pass_groups, false, "Renderer diagnostics",
                   "Continue certified hardware phase draws within one immutable attachment generation and render pass")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_share_retained_opaque_phase_scratch, false, "Renderer diagnostics",
                   "Reuse one immutable conversion buffer and descriptor within the retained phase's ordered Native GPU queue")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_poll_retained_opaque_phase_pressure, false, "Renderer diagnostics",
                   "Poll completed Native GPU work once before a retained phase image allocation under actual budget pressure")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_share_retained_opaque_phase_attachments, false, "Renderer diagnostics",
                   "Reuse one immutable phase attachment set through accepted Native GPU generations")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_early_depth, true, "Renderer diagnostics",
                   "Use resident hardware early-depth rejection for the audited Native material")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_reuse_early_depth_readonly_boundaries, false, "Renderer diagnostics",
                   "Retain the V1 immutable early-depth guard across exact unchanged completion/query boundaries")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_early_depth_catalog, false, "Renderer diagnostics",
                   "Experimental resident two-material Native early-depth catalogue")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_early_depth_material_retention, false, "Renderer diagnostics",
                   "Retain audited material early-depth guards across proved monotonic depth writes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_readonly_depth_no_write_metadata, false, "Renderer diagnostics",
                   "Preserve packed versions for exact readonly D32 queries while invalidating overlapping host precision")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_readonly_depth_conservative, false, "Renderer diagnostics",
                   "Use the proved half-depth conservative bound for resident exact D32 queries")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_readonly_depth_attachment, true, "Renderer diagnostics",
                   "Use resident exact quantized D32 depth for audited readonly Native queries")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_builtin_readonly_depth_queries, false, "Renderer diagnostics",
                   "Use resident audited read-only depth-query builtin without unnecessary interlock")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_builtin_atomic_depth, false, "Renderer diagnostics",
                   "Use audited atomic packed depth maximum for unqueried depth-only geometry")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_fsi_hoist_mode, 0, "Renderer diagnostics",
                   "Separate Native FSI epilogue: 0 disabled, 1 immutable state, 2 state and incoming packed depth")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_exact_fsi_stores, false, "Renderer diagnostics",
                   "Omit certified identity depth stores and unused full-word color reads in separate Native FSI shaders")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_implicit_fsi_barriers, false, "Renderer diagnostics",
                   "Use audited implicit coherent FSI shader barriers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_coalesce_draw_barriers, false, "Renderer diagnostics",
                   "Combine adjacent native attachment and EDRAM barriers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_skip_readonly_sample_barriers, false, "Renderer diagnostics",
                   "Reuse established read-only texture states without redundant barriers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_precise_fsi_barriers, false, "Renderer diagnostics",
                   "Scope proven native fragment dependencies to fragment and attachment stages")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_compact_resolve_channels, false, "Renderer diagnostics",
                   "Omit unreachable decoded texture channels while preserving float32 precision")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_pipeline_prewarm, true, "Renderer diagnostics",
                   "Record local observed native pipelines and prepare known combinations before guest launch")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_prewarm_overflow_pipelines, true, "Renderer diagnostics",
                   "Prepare omitted canonical PSOs with bounded temporary shader modules after original startup")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_query_poll_yield, false, "Renderer diagnostics",
                   "Yield after 128 real pending native query polls at the blocking retry caller")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_inline_getdata_register_helpers, true, "Renderer diagnostics",
                   "Inline exact GetData register helpers after Native code/alias authentication; readiness unchanged")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_shader_prewarm, true, "Renderer diagnostics",
                   "Prepare exact native shader sources at object creation and linking on bounded CPU workers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retain_fsi_raster_extent, true, "Renderer diagnostics",
                   "Reuse a bounded larger dummy raster attachment with exact guest clipping")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_cache_texture_layouts, true, "Renderer diagnostics",
                   "Cache exact immutable texture layout metadata without skipping coherence checks")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_queued_volume_alias_loads, false, "Renderer diagnostics",
                   "Queue bounded volume alias slices and assembly under the original transfer fence")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_cache_gpu_alias_states, false, "Renderer diagnostics",
                   "Reuse exact GPU alias definedness and page version only while physical memory serial is unchanged")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_block_page_versions, true, "Renderer diagnostics",
                   "Track exact physical page versions with bounded uniform block summaries")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_range_texture_versions, true, "Renderer diagnostics",
                   "Reuse exact CPU texture versions across writes outside conservative base/mip ranges")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_reuse_defined_ranges, false, "Renderer diagnostics",
                   "Preserve already covered physical memory intervals while updating every write version")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_coalesced_texture_ranges, true, "Renderer diagnostics",
                   "Compute exact whole-tile and linear-row texture byte ranges without enumerating every block")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_compact_cpu_texture_version_pages, true, "Renderer diagnostics",
                   "Scan the exact sampled texture page union without duplicate page visits")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_associative_texture_layouts, false, "Renderer diagnostics",
                   "Use same-budget four-way pure texture fetch metadata cache")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_recycle_resolve_descriptors, true, "Renderer diagnostics",
                   "Reuse fence-retired native compute descriptors within bounded pools")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_recycle_pass_transfer_descriptors, true, "Renderer diagnostics",
                   "Reuse fence-retired native resolve and depth-restore graphics descriptor backings")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_recycle_pass_transfer_render_passes, false, "Renderer diagnostics",
                   "Reuse exact immutable PassTransfer Vulkan render-pass descriptions")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_reuse_resolve_identity_views, false, "Renderer diagnostics",
                   "Reuse equivalent owned identity image views for native resolve loads")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_diagnose_texture_eviction_reloads, false, "Renderer diagnostics",
                   "Record bounded immutable texture eviction/reload causes and host timings")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_recycle_resolve_images, false, "Renderer diagnostics",
                   "Reuse fully retired native resolve images within fixed allocation budgets")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_fast_snapshot_fingerprints, true, "Renderer diagnostics",
                   "Use XXH3 lookup hints while preserving full snapshot comparisons and canonical SHA-256 identities")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_direct_resource_refs, true, "Renderer diagnostics",
                   "Copy typed canonical digest/owner directly from the validated producer bank entry")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_lazy_state_identities, true, "Renderer diagnostics",
                   "Compute canonical native draw state identities only when needed")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_shader_prewarm_threads, 0, "Renderer diagnostics",
                    "Native shader preparation workers; zero uses available hardware threads, at most32")
    .range(0,32).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_draw_batch_size, 16, "Renderer diagnostics",
                    "Maximum accepted native draws per fenced batch; one enables per-draw diagnostics")
    .range(1,64).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_query_capacity, 64, "Renderer diagnostics",
                    "Maximum deferred logical queries; independent of physical GPU draw/query slots")
    .range(1,128).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_host_query_reset, false, "Renderer diagnostics",
                   "Reset completed Native visibility-query ranges on the CPU after their GPU fence")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_readonly_query_render_pass_groups, false, "Renderer diagnostics",
                   "Group audited immutable D32 visibility draws while retaining separate query results")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_submission_capacity, 64, "Renderer diagnostics",
                    "Physical async GPU draw/query/list capacity: 64 or 128; synchronous mode uses 64")
    .range(64,128).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_deferred_queries, true, "Renderer diagnostics",
                   "Batch independent native visibility queries until an ordered completion boundary")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retire_query_prefixes, true, "Renderer diagnostics",
                   "Publish actual completed native query prefixes while later GPU work remains")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_batch_transfers, true, "Renderer diagnostics",
                   "Combine native surface transfer stages behind one completion fence per operation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_unified_gpu_queue, true, "Renderer diagnostics",
                   "Share ordered native GPU submissions between draws and surface transfers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_async_gpu_submissions, true, "Renderer diagnostics",
                   "Overlap native GPU execution with command recording using two bounded submission batches")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_submission_worker, true, "Renderer diagnostics",
                   "Overlap checked Native driver submissions with draw preparation; restart required");
REXCVAR_DEFINE_BOOL(gow3_native_four_inflight_batches, true, "Renderer diagnostics",
                   "Four native GPU fences with unchanged physical list, query and scratch bounds")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_queue_cpu_memory_writes, false, "Renderer diagnostics",
                   "Queue immutable page-sized CPU history updates; query report overlaps keep eager completion")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_reuse_cpu_write_upload, false, "Renderer diagnostics",
                   "Reuse one fully fenced 4 KiB synchronous CPU upload buffer without changing submission order")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_forward_cpu_upload_receipts, false, "Renderer diagnostics",
                   "Forward exact completed CPU-upload pages into fully covered overlays; retain native ownership and original flushes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_diagnose_cpu_write_cohorts, false, "Renderer diagnostics",
                   "Count bounded immutable CPU-write cohorts before existing Native prefix execution")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_diagnose_completion_causes, false, "Renderer diagnostics",
                   "Attribute outer native completion wall time and existing queue waits by caller cause")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_cache_coherence_waits, false, "Renderer diagnostics",
                   "Keep native cache-only register waits GPU ordered without an eager CPU fence")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_stencil_export_restores, true, "Renderer diagnostics",
                   "Restore native depth/stencil attachments in one draw when the device enabled stencil export")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_gpu_timestamps, false, "Renderer diagnostics",
                   "Measure inclusive native GPU batch intervals and separate host submission/wait/retirement phases")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_gpu_batch_report, "", "Renderer diagnostics",
                     "Write bounded native GPU batch composition and timing diagnostics at shutdown; enables GPU timestamps")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_gpu_physical_report, "", "Renderer diagnostics",
                     "Sample existing physical GPU lists and save at shutdown; preserves render groups")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_native_gpu_physical_first, 0, "Renderer diagnostics", "First sampled API frame")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_native_gpu_physical_end, 6000, "Renderer diagnostics", "Exclusive sampled API frame limit")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_native_gpu_physical_interval, 30, "Renderer diagnostics", "API frame sampling interval")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_gpu_draw_report, "", "Renderer diagnostics",
                     "Write bounded inclusive draw-list timings at shutdown; samples every 16th API frame without adding submissions or waits")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_detailed_reports, false, "Renderer diagnostics",
                   "Record detailed native draw and texture provenance; normal gameplay keeps essential counters and errors")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_raw_gpu_float_cpu_textures, false, "Renderer diagnostics",
                   "Integer-only GPU copy of the fully-owned64KiB linear RGBA32F CPU texture row")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_defer_cpu_texture_drains, false, "Renderer diagnostics",
                   "Drain CPU texture misses only when exact GPU-owned sampled bytes need readback")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_share_cpu_texture_sampling_variants, false, "Renderer diagnostics",
                   "Share exact CPU texture image storage across LOD bias and mip-filter sampler variants")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_cpu_image_slot_lookup, false, "Renderer diagnostics",
                   "Reuse exact CPU image map lookups through 32 borrowed texture-slot hints")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_diagnose_full_cpu_texture_overlays, false, "Renderer diagnostics",
                   "Record bounded metadata for fully GPU-overlaid CPU texture misses; rendering is unchanged")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retain_unused_textures, true, "Renderer diagnostics",
                   "Retain a bounded set of recently used immutable CPU textures")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_native_dormant_texture_entries, 512, "Renderer diagnostics",
                     "Extra dormant CPU texture entry budget")
    .range(0u,4096u).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_native_dormant_texture_mib, 128, "Renderer diagnostics",
                     "Extra dormant CPU texture byte budget in MiB")
    .range(0u,2048u).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(gow3_native_dormant_texture_frames, 120, "Renderer diagnostics",
                     "Frames an unused CPU texture stays resident before age eviction")
    .range(1u,7200u).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_recycle_vertex_buffers, false, "Renderer diagnostics",
                   "Reuse exact-description fully retired vertex buffers within 128 idle allocations/16 MiB")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_retain_unused_vertices, false, "Renderer diagnostics",
                   "Retain a bounded set of unused vertex buffers for residency comparisons")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_live, false, "Renderer diagnostics",
                   "Legacy diagnostic flag, ignored; select Native or Xenia with gow3_renderer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_present, false, "Renderer diagnostics",
                   "Legacy diagnostic flag, ignored; Native always owns its final image")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_ownership, false, "Renderer diagnostics",
                   "Legacy diagnostic flag, ignored; Native always owns all scene rendering")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_raster, false, "Renderer diagnostics",
                   "Legacy diagnostic flag, ignored; Native has exclusive authority without fallback")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_test_fallback_after_packets, 0, "Renderer diagnostics",
                    "Force a fatal Native error before a packet after this many completed native packets; zero disables")
    .range(0,100000000).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_test_fallback_query_draws, 0, "Renderer diagnostics",
                    "Force a fatal Native error after this many accepted draws in an active query; zero disables")
    .range(0,100000000).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_native_test_fallback_before_query_draw, false, "Renderer diagnostics",
                   "Force a fatal Native error before the first draw of an active query")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gow3_native_test_fallback_after_deferred_queries, 0, "Renderer diagnostics",
                    "Force a fatal Native error inside the Nth deferred query end; zero disables")
    .range(0,100000000).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_failure_directory, "", "Renderer diagnostics",
                     "Save unsupported native events and bounded query mismatch diagnostics here; empty disables files")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_texture_probe_directory, "", "Renderer diagnostics",
                     "New directory for bounded post-overlay CPU texture diagnostics; requires selected payload hashes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_native_texture_probe_hashes, "", "Renderer diagnostics",
                     "Comma-separated SHA-256 base payloads to probe, at most 32; empty disables diagnostics")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
using namespace gow3::renderer;
using HostDevice = rex::ui::vulkan::VulkanDevice;
using Processor = rex::graphics::vulkan::VulkanCommandProcessor;
bool NativeRasterRequested() {
  return ParseGow3RendererMode(REXCVAR_QUERY(std::string,gow3_renderer)) == Gow3RendererMode::Native;
}
struct NativeTransactionFatal : std::runtime_error { using std::runtime_error::runtime_error; };

struct QueueLease final : LiveQueueLease {
  HostDevice::Queue::Acquisition acquisition;
  explicit QueueLease(HostDevice* device)
      : acquisition(device->AcquireQueue(device->queue_family_graphics_compute(), 0)) {}
};

LiveVulkanDeviceDesc DescribeDevice(HostDevice* device) {
  LiveVulkanDeviceDesc desc;
  desc.instance = device->vulkan_instance()->instance();
  desc.physical_device = device->physical_device();
  desc.device = device->device();
  desc.queue_family = device->queue_family_graphics_compute();
  desc.queue = device->queue_families().at(desc.queue_family).queues.at(0)->queue;
  const auto& p = device->properties();
  desc.api_version = std::min(device->vulkan_instance()->api_version(), p.apiVersion);
  // Properties report enabled host features, rather than merely hardware
  // support. Never advertise a feature absent from vkCreateDevice's chain.
#define COPY_FEATURE(name) desc.features.name = p.name
  COPY_FEATURE(robustBufferAccess);
  COPY_FEATURE(fullDrawIndexUint32);
  COPY_FEATURE(independentBlend);
  COPY_FEATURE(geometryShader);
  COPY_FEATURE(tessellationShader);
  COPY_FEATURE(sampleRateShading);
  COPY_FEATURE(depthClamp);
  COPY_FEATURE(fillModeNonSolid);
  COPY_FEATURE(samplerAnisotropy);
  COPY_FEATURE(occlusionQueryPrecise);
  COPY_FEATURE(vertexPipelineStoresAndAtomics);
  COPY_FEATURE(fragmentStoresAndAtomics);
  COPY_FEATURE(shaderClipDistance);
  COPY_FEATURE(shaderCullDistance);
  COPY_FEATURE(shaderInt64);
  COPY_FEATURE(shaderSampledImageArrayDynamicIndexing);
#undef COPY_FEATURE
#define COPY_FEATURE(name) desc.features12.name = p.name
  COPY_FEATURE(samplerMirrorClampToEdge);
  COPY_FEATURE(scalarBlockLayout);
  COPY_FEATURE(hostQueryReset);
  COPY_FEATURE(bufferDeviceAddress);
  COPY_FEATURE(runtimeDescriptorArray);
  COPY_FEATURE(descriptorBindingPartiallyBound);
  COPY_FEATURE(descriptorBindingVariableDescriptorCount);
  COPY_FEATURE(descriptorBindingSampledImageUpdateAfterBind);
  COPY_FEATURE(shaderSampledImageArrayNonUniformIndexing);
#undef COPY_FEATURE
  desc.null_descriptor = p.nullDescriptor;
  desc.ordered_pixel_interlock = p.fragmentShaderPixelInterlock;
  desc.shader_stencil_export = device->extensions().ext_EXT_shader_stencil_export;
  desc.acquire_queue = [device] { return std::make_unique<QueueLease>(device); };
  return desc;
}

struct RuntimeState : std::enable_shared_from_this<RuntimeState> {
  Processor* processor;
  rex::memory::Memory* memory;
  NativeMemoryCoherence memory_coherence;
  // An unconfirmed arm or in-flight OS write cannot promise a coherent export.
  // Producer publishes this before failed; CP must stop rather than hand back.
  std::atomic<bool> coherence_unsafe{false};
  void* memory_callback=nullptr;
  LiveVulkanDeviceDesc description;
  const bool preview;
  const bool ownership;
  const bool native_raster;
  const bool deferred_queries;
  const uint32_t query_capacity;
  static_assert(NativeQueryTracker::kMaxDeferredQueries<=rex::graphics::NativeQueryTickets::kCapacity);
  const bool unified_gpu_queue;
  const bool queue_cpu_memory_writes;
  const bool diagnose_cpu_write_cohorts;
  const bool diagnose_completion_causes;
  const bool time_cpu_stages;
  const bool verbose_frame_reports;
  const bool retire_query_prefixes;
  // Direct execution: no PM4 packets. Planned work is posted to the CP thread
  // in API order and executed there; GetData reads direct query results.
  const bool direct=REXCVAR_GET(native_direct_execution);
  uint32_t direct_idle_polls=0;
  const size_t direct_batch_operations=size_t(REXCVAR_GET(gow3_native_direct_batch_operations));
  uint64_t direct_api_memory_copies=0;
  const bool cache_coherence_waits;
  const bool fsi_state_material_composition_enabled;
  const bool fsi_state_inactive_early_catalog_enabled;
  const bool fsi_action_state_overflow_lookup_enabled;
  const bool retained_d32_composition_enabled;
  const bool retain_primary_across_disjoint_queries_enabled;
  const bool readonly_depth_attachment_enabled;
  const bool early_depth_enabled;
  const bool early_depth_catalog_enabled;
  const bool reuse_early_depth_readonly_boundaries_enabled;
  const bool retained_shared_depth_queries_enabled;
  const bool retained_fp24_depth_writer_enabled;
  const NativeFsiHoistMode fsi_hoist_mode;
  const bool exact_fsi_stores_enabled;
  const std::filesystem::path failure_directory;
  const std::filesystem::path gpu_batch_report_path;
  const std::filesystem::path gpu_draw_report_path;
  const std::filesystem::path gpu_physical_report_path;
  std::atomic<bool> stopping{false}, failed{false};
  std::mutex pending_mutex;
  std::condition_variable pending_changed;
  std::deque<std::shared_ptr<LiveRenderFrame>> pending;
  std::deque<LiveQueryPrefix> pending_prefixes;
  struct QueuedPacket { LivePacketPrefix prefix; uint64_t captured_write_epoch=0; };
  std::deque<QueuedPacket> pending_packets;
  std::deque<LivePacketPrefix> deferred_packets;
  PacketOwnership packet_ownership;
  Processor::NativeDrawDisposition span_disposition=Processor::NativeDrawDisposition::ReferenceCoherent;
  std::atomic<uint64_t> next_write_epoch{0};
  std::array<std::atomic<uint64_t>,NativeMemoryCoherence::kCapacity/NativeMemoryCoherence::kPage> write_epochs{};
  std::array<uint64_t,NativeMemoryCoherence::kCapacity/NativeMemoryCoherence::kPage> executed_write_epochs{};
  // Direct execution only.
  const bool async_planner=REXCVAR_GET(gow3_native_async_planner)&&REXCVAR_GET(native_direct_execution);
  // Only the guest boundary thread uses the planner, or only the planner
  // thread when async_planner is set. Capture's bridge lock serializes guest
  // callbacks. GPU state below is used only by the CP thread.
  GearsLivePlan planner;
  // Async planner: the guest thread keeps API-point duties (CPU invalidation
  // snapshots, GPU-ownership watches, texture snapshots, memory-export copies,
  // query registration) using this replica and queues the event in order.
  struct PlannerItem {
    GraphicsBoundaryEvent event;
    bool invalidation=false;
    uint32_t address=0,bytes=0;std::vector<uint8_t> data;uint64_t coherence_epoch=0;
    uint64_t captured_write_epoch=0;
    bool query_end=false;uint64_t query_generation=0;
    std::optional<GearsLivePlan::MemoryCopy> memory_copy;
    std::function<void()> swap;  // Direct swap, posted after its frame's work.
    std::chrono::steady_clock::time_point enqueued{};
  };
  static constexpr size_t kPlannerQueueLimit=4096;
  std::unique_ptr<GearsLiveOwnership> ownership_replica;
  std::mutex planner_mutex;
  std::condition_variable planner_ready,planner_space;
  std::deque<PlannerItem> planner_queue;
  bool planner_idle=false,planner_failure_reported=false;
  std::thread planner_thread;
  std::atomic<uint64_t> planner_items{0},planner_producer_waits{0};
  size_t planner_peak_queue=0;
  // Query-end latency split: planner queue, planning, GPU Commands queue; and
  // time the planner thread blocks for the previous frame's presentation.
  std::atomic<uint64_t> planner_query_ends{0},planner_queue_ns{0},planner_plan_ns{0},planner_cp_ns{0},
      planner_pending_wait_ns{0},planner_pending_waits{0};
  NativeShaderCompiler compiler;
  std::unique_ptr<NativeFsiExactStoresCompiler> exact_fsi_compiler;
  std::unique_ptr<NativeFsiHoistCompiler> fsi_hoist_compiler;
  std::shared_ptr<const NativeRetainedOpaqueProgram> retained_opaque_program;
  std::shared_ptr<const NativeRetainedOpaqueCatalog> retained_opaque_catalog_programs;
  std::shared_ptr<const NativeRetainedOpaquePhaseCatalog> retained_opaque_phase_programs;
  std::shared_ptr<const OpaqueColorOverwriteProgram> opaque_overwrite_program;
  std::shared_ptr<const NativeStencilMaterialProgram> stencil_material_program;
  std::shared_ptr<const ForwardControlFlowProgram> forward_control_flow_program;
  std::shared_ptr<const ForwardVertexProgram> forward_vertex_program;
  std::shared_ptr<const ForwardPixelCatalog> forward_pixel_catalog;
  std::shared_ptr<const FsiStateSpecializationCatalog> fsi_state_programs;
  std::shared_ptr<NativeFsiStateRecipeCache> fsi_state_recipes;
  std::filesystem::path fsi_state_shader_cache;
  std::shared_ptr<NativeShaderPrewarmQueue> shader_prewarm;
  // Async shaders. finished: keys whose background compile ended (prewarm
  // workers write it); the rest is used only on the command thread.
  bool async_shaders=false;
  std::chrono::milliseconds async_shader_budget{250};
  std::filesystem::path native_shader_cache;
  std::mutex async_finished_mutex;
  std::unordered_set<std::string> async_finished;
  std::unordered_set<std::string> async_ready;
  std::unordered_map<std::string,std::chrono::steady_clock::time_point> async_waiting;
  uint64_t async_not_ready=0,async_budget_blocks=0,async_enqueued=0;
  // Cold shader-provider calls only. Keep bounded records in memory and format
  // them at shutdown, never while recording ordinary draw commands. The
  // compiler and its cache identity are deliberately unchanged by this probe.
  struct SlowShaderPreparation {
    std::string key;
    uint32_t frame=UINT32_MAX;
    uint64_t prefix_first_event=0,prefix_last_event=0,requested_ns=0,returned_ns=0;
    double milliseconds=0;
    bool original_compilation_disk_cache_hit=false;
    NativeShaderPrewarmQueue::JobInspection before{},after{};
  };
  std::array<SlowShaderPreparation,128> slow_shader_preparations{};
  size_t slow_shader_preparation_count=0;
  uint64_t shader_provider_requests=0,slow_shader_preparation_dropped=0;
  std::shared_ptr<NativePipelineRecipeCache> pipeline_recipes;
  std::unique_ptr<LiveVulkanDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<SceneExecutor> renderer;
  std::shared_ptr<LiveRenderFrame> active_frame;
  CompletedFrameDisposer completed_frame_disposer;
  std::shared_ptr<plume::RenderTexture> output;
  std::array<uint32_t, 6> output_fetch{};
  uint32_t output_frame = UINT32_MAX;
  std::atomic<uint64_t> completed_frames{0};
  std::atomic<uint32_t> last_completed_api_frame{UINT32_MAX};
  uint64_t query_frames = 0;
  uint64_t complex_frames = 0;
  uint64_t large_frames = 0;
  unsigned query_diagnostics = 0;
  uint32_t executing_frame = UINT32_MAX;
  size_t executed_operations = 0;
  uint64_t completed_query_prefixes = 0;
  struct PendingQuery {
    Processor::NativeQueryBoundary boundary;
    uint32_t frame;
    uint64_t end_event;
  };
  std::deque<PendingQuery> pending_query_publications;
  // Retain completed samples until their SDK publication succeeds, including
  // on a publication exception. Neither queue owns guest-memory pointers.
  // CP-owned diagnostic state. Disabled captures add no per-draw work and
  // never consult the guest bridge lock or a reference renderer.
  const std::filesystem::path frame_capture_directory{REXCVAR_GET(gow3_native_frame_capture)};
  const std::filesystem::path frame_capture_trigger{REXCVAR_GET(gow3_native_frame_capture_trigger)};
  const std::string frame_capture_pixel{REXCVAR_GET(gow3_native_frame_capture_pixel)};
  std::unique_ptr<NativeFrameCaptureWriter> frame_capture_writer;
  uint32_t frame_capture_first=0,frame_capture_count=0;
  bool frame_capture_finished=false;
  std::vector<CompletedNativeQuery> ready_query_publications;
  size_t next_ready_query=0;
  std::optional<PendingQuery> current_query_end;
  std::shared_ptr<QueryReadinessCensus> query_readiness = REXCVAR_GET(gow3_native_query_readiness_census)
      ? std::make_shared<QueryReadinessCensus>() : nullptr;
  std::shared_ptr<PublishedQueryResults> published_results = REXCVAR_GET(gow3_native_published_query_results)
      ? std::make_shared<PublishedQueryResults>(8192,REXCVAR_GET(gow3_native_query_wait_census)) : nullptr;
  uint64_t observed_query_poll_sequence=0,observed_query_poll_generation=0,query_wait_generation_changes=0;
  std::array<uint64_t,size_t(SceneExecutor::QueryWorkStage::Count)> query_wait_stages{};
  uint64_t latest_executed_query_generation=0,query_wait_before_execution=0;
  uint64_t query_wait_generation_distance=0,query_wait_max_generation_distance=0;
  uint64_t deferred_query_prefixes=0;
  size_t peak_deferred_queries=0;
  struct QueryPressureTiming {uint64_t drains=0,gpu_drains=0;double milliseconds=0;};
  QueryPressureTiming runtime_query_pressure,sdk_query_pressure;
  struct CpuWriteQueueDecisions {
    uint64_t considered=0,queued=0,disabled=0,bounds_refused=0,query_report_refused=0,null_payloads=0;
  } cpu_write_queue_decisions;
  enum class CpuWriteCohortPrefix : uint8_t {Packet,Query,Presentation,Count};
  struct CpuWriteCohortStatistics {
    static constexpr size_t kMaximumRun=64;
    std::array<uint64_t,kMaximumRun+1> run_lengths{},run_members{},run_requested_bytes{},run_null_payloads{},run_non_null_payloads{},run_malformed_members{};
    std::array<std::array<uint64_t,kMaximumRun+1>,size_t(CpuWriteCohortPrefix::Count)> run_lengths_by_prefix{};
    std::array<uint64_t,size_t(CpuWriteCohortPrefix::Count)> prefixes{},cohorts{},members{};
    uint64_t prefixes_with_writes=0,requested_bytes=0,null_payloads=0,non_null_payloads=0;
    uint64_t malformed_members=0,unknown_operation_kinds=0,bounds_candidates=0,bounds_refused=0;
    uint64_t exact_page_uploads=0,query_report_overlap_at_prefix_start=0;
    uint64_t oversized_cohorts=0,maximum_run_length=0;
    bool saturated=false;
    void Add(uint64_t& value,uint64_t amount=1) noexcept {
      if(amount>std::numeric_limits<uint64_t>::max()-value) {
        value=std::numeric_limits<uint64_t>::max();saturated=true;
      } else value+=amount;
    }
  } cpu_write_cohorts;
  uint64_t replaced_copy_packets = 0,matched_draw_packets=0,unmatched_draw_packets=0;
  bool edram_initialized=false;
  const bool reference_retired=false; // Kept in diagnostic schema; Native never hands back.
  uint64_t native_raster_packets=0,proven_no_effect_packets=0;
  uint64_t active_capture_epoch=0;
  double execution_milliseconds = 0;
  // CP-thread-only POD diagnostics. Each CompletePending call has one direct
  // cause; nested Scene/transfer drains are observed only through its queue
  // counter delta and never timed again by this diagnostic.
  enum class CompletionCause : uint8_t {
    Unclassified, CpuWrite, PresentOperation, MemoryCopy, OtherOperation,
    RuntimeQueryPressure, SdkQueryPressure, ImmediateQueryBoundary,
    PresentationTail, FailureCleanup, SdkCommandPacket, SdkScratchWrite,
    SdkWait, SdkPrimaryBufferEnd, SdkPresentation, SdkReferenceDraw,
    SdkProviderRemoval, SdkShutdown, Count
  };
  static constexpr std::array<const char*,size_t(CompletionCause::Count)> completion_cause_names{
    "unclassified", "cpu_write", "present_operation", "memory_copy", "other_operation",
    "runtime_query_pressure", "sdk_query_pressure", "immediate_query_boundary",
    "presentation_tail", "failure_cleanup", "sdk_command_packet", "sdk_scratch_write",
    "sdk_wait", "sdk_primary_buffer_end", "sdk_presentation", "sdk_reference_draw",
    "sdk_provider_removal", "sdk_shutdown"};
  struct CompletionCauseCounters {
    uint64_t calls=0,no_renderer_calls=0,no_op_calls=0,pending_work_calls=0,
        query_only_calls=0,completed_calls=0,failed_calls=0,
        queue_observed_completions=0,queue_timed_completions=0,
        queue_wait_call_completions=0,queue_waits=0;
    double completion_wall_ms=0,maximum_completion_wall_ms=0,queue_fence_wait_ms=0;
  };
  std::array<CompletionCauseCounters,size_t(CompletionCause::Count)> completion_causes{};
  uint64_t excluded_retained_query_completion_calls=0;
  struct OperationTiming { uint64_t count=0; double milliseconds=0; };
  std::array<OperationTiming,7> operation_timings{};
  double transaction_flush_milliseconds=0;
  double batch_completion_milliseconds=0;
  uint64_t batch_completion_count=0;
  std::optional<std::chrono::steady_clock::time_point> previous_presentation;
  std::vector<double> presentation_intervals;
  Gow3QueryWaitStats previous_query_wait{},previous_journal_query_wait{};
  uint32_t presentation_interval_first_frame=0;
  NativeFrameJournal frame_journal; // CP-owned; exported only after final shared owner release.

  bool EffectiveNativeReadonlyDepthAttachment() const {
    // Default admission is restricted to the validated normal Native path.
    // Explicit alternative experiments retain their own requested behavior.
    // Freeze the result once; startup counts/prewarm and Scene config must agree.
    return REXCVAR_GET(gow3_native_readonly_depth_attachment)&&native_raster&&deferred_queries&&unified_gpu_queue&&
        description.ordered_pixel_interlock&&description.features.fragmentStoresAndAtomics&&
        REXCVAR_GET(gow3_native_async_gpu_submissions)&&
        !REXCVAR_GET(gow3_native_attachmentless_fsi)&&
        (retained_d32_composition_enabled||(!REXCVAR_GET(gow3_native_retained_fp24_depth_writer)&&
         !REXCVAR_GET(gow3_native_retained_opaque_writer)&&!REXCVAR_GET(gow3_native_retained_opaque_catalog)))&&
        !REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)&&!REXCVAR_GET(gow3_native_fsi_state_forward_cfg)&&
        !REXCVAR_GET(gow3_native_implicit_fsi_barriers)&&(!REXCVAR_GET(gow3_native_builtin_atomic_depth)||REXCVAR_GET(gow3_native_packed_depth_render_pass_groups))&&
        !REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)&&!REXCVAR_GET(gow3_native_opaque_color_overwrite)&&
        !REXCVAR_GET(gow3_native_forward_control_flow)&&!REXCVAR_GET(gow3_native_forward_vertex_control_flow)&&
        !REXCVAR_GET(gow3_native_forward_pixel_catalog);
  }

  RuntimeState(Processor* processor_, rex::memory::Memory* memory_, LiveVulkanDeviceDesc description_, bool preview_,
               const std::filesystem::path& cache)
      : processor(processor_), memory(memory_), description(std::move(description_)), preview(preview_),
        ownership(true),native_raster(true),
        deferred_queries(native_raster&&REXCVAR_GET(gow3_native_deferred_queries)),
        query_capacity(uint32_t(std::clamp(REXCVAR_GET(gow3_native_query_capacity),1,128))),
        unified_gpu_queue(native_raster&&REXCVAR_GET(gow3_native_batch_transfers)&&REXCVAR_GET(gow3_native_unified_gpu_queue)),
        queue_cpu_memory_writes(unified_gpu_queue&&REXCVAR_GET(gow3_native_queue_cpu_memory_writes)),
        diagnose_cpu_write_cohorts(native_raster&&REXCVAR_GET(gow3_native_diagnose_cpu_write_cohorts)),
        diagnose_completion_causes(native_raster&&REXCVAR_GET(gow3_native_diagnose_completion_causes)),
        time_cpu_stages(REXCVAR_GET(gow3_native_time_cpu_stages)||REXCVAR_GET(gow3_native_time_draw_recording)||
                        REXCVAR_GET(gow3_native_diagnose_texture_eviction_reloads)),
        verbose_frame_reports(REXCVAR_GET(gow3_native_verbose_frame_reports)),
        retire_query_prefixes(deferred_queries&&unified_gpu_queue&&REXCVAR_GET(gow3_native_retire_query_prefixes)),
        cache_coherence_waits(native_raster&&unified_gpu_queue&&description.ordered_pixel_interlock&&
                              description.features.fragmentStoresAndAtomics&&REXCVAR_GET(gow3_native_async_gpu_submissions)&&
                              REXCVAR_GET(gow3_native_cache_coherence_waits)),
        fsi_state_material_composition_enabled(REXCVAR_GET(gow3_native_fsi_state_material_composition)),
        fsi_state_inactive_early_catalog_enabled(REXCVAR_GET(gow3_native_fsi_state_inactive_early_catalog)),
        fsi_action_state_overflow_lookup_enabled(REXCVAR_GET(gow3_native_fsi_action_state_overflow_lookup)),
        retained_d32_composition_enabled(REXCVAR_GET(gow3_native_retained_d32_composition)),
        retain_primary_across_disjoint_queries_enabled(REXCVAR_GET(gow3_native_retain_primary_across_disjoint_queries)),
        readonly_depth_attachment_enabled(EffectiveNativeReadonlyDepthAttachment()),
        early_depth_enabled(REXCVAR_GET(gow3_native_early_depth)),
        early_depth_catalog_enabled(REXCVAR_GET(gow3_native_early_depth)&&REXCVAR_GET(gow3_native_early_depth_catalog)),
        reuse_early_depth_readonly_boundaries_enabled(early_depth_enabled&&!early_depth_catalog_enabled&&
            REXCVAR_GET(gow3_native_reuse_early_depth_readonly_boundaries)),
        retained_shared_depth_queries_enabled(REXCVAR_GET(gow3_native_retained_shared_depth_queries)),
        retained_fp24_depth_writer_enabled(REXCVAR_GET(gow3_native_retained_fp24_depth_writer)),
        fsi_hoist_mode(static_cast<NativeFsiHoistMode>(REXCVAR_GET(gow3_native_fsi_hoist_mode))),
        exact_fsi_stores_enabled(REXCVAR_GET(gow3_native_exact_fsi_stores)),
        failure_directory(REXCVAR_GET(gow3_native_failure_directory)),
        gpu_batch_report_path(REXCVAR_GET(gow3_native_gpu_batch_report)),
        gpu_draw_report_path(REXCVAR_GET(gow3_native_gpu_draw_report)),
        gpu_physical_report_path(REXCVAR_GET(gow3_native_gpu_physical_report)),
        planner([this](uint32_t address,uint32_t bytes){if(!async_planner)memory_coherence.Watch(address,bytes);}, true,
                REXCVAR_GET(gow3_native_lazy_state_identities),REXCVAR_GET(gow3_native_direct_resource_refs),
                REXCVAR_GET(gow3_native_fast_snapshot_fingerprints)),
        compiler(cache / "native-shaders") {
    native_shader_cache=cache/"native-shaders"/NativeShaderCompiler::CacheVersion();
    async_shaders=REXCVAR_GET(gow3_native_async_shaders);
    async_shader_budget=std::chrono::milliseconds(std::max(0,REXCVAR_GET(gow3_native_async_shader_budget_ms)));
    try {
      // An existing journal is kept; this session's goes next to it with a
      // start-time suffix. The writer itself still never truncates a path.
      std::filesystem::path journal_path=std::string(REXCVAR_GET(gow3_native_frame_journal));
      if(!journal_path.empty()&&std::filesystem::exists(journal_path)) {
        const std::time_t now=std::time(nullptr);char stamp[32]{};
        std::strftime(stamp,sizeof(stamp),"-%Y%m%d-%H%M%S",std::localtime(&now));
        journal_path.replace_filename(journal_path.stem().string()+stamp+journal_path.extension().string());
        REXLOG_INFO("Native frame journal: configured path exists; writing {}",journal_path.string());
      }
      frame_journal.Start(journal_path.string());
      if(frame_journal.InitializationFailed())REXLOG_WARN("Native frame journal initialization failed; rendering unchanged");
    }catch(...) {
      try {REXLOG_WARN("Native frame journal configuration unavailable; rendering unchanged");}catch(...) {}
    }
    if(!frame_capture_directory.empty()) {
      if(!deferred_queries||(frame_capture_trigger.empty()&&frame_capture_pixel.empty()))
        throw std::invalid_argument("Native frame capture requires deferred query tracking and an explicit trigger or pixel source");
      if(std::filesystem::exists(frame_capture_directory))
        throw std::invalid_argument("Native frame capture directory already exists");
      if(!frame_capture_pixel.empty()&&(frame_capture_pixel.size()!=64||
          frame_capture_pixel.find_first_not_of("0123456789abcdef")!=std::string::npos))
        throw std::invalid_argument("Invalid Native capture pixel source digest");
    } else if(!frame_capture_trigger.empty()||!frame_capture_pixel.empty())
      throw std::invalid_argument("Native capture trigger requires an output directory");
    if(REXCVAR_GET(gow3_native_fsi_state_cache_headroom)&&!fsi_state_material_composition_enabled)
      throw std::invalid_argument("FSI shader cache headroom requires material composition");
    if(fsi_state_inactive_early_catalog_enabled&&!fsi_state_material_composition_enabled)
      throw std::invalid_argument("Inactive early catalogue admission requires material composition");
    if(fsi_action_state_overflow_lookup_enabled&&!fsi_state_material_composition_enabled)
      throw std::invalid_argument("FSI action state overflow lookup requires material composition");
    if(REXCVAR_GET(gow3_native_retained_opaque_phase)&&
       (!fsi_state_material_composition_enabled||REXCVAR_GET(gow3_native_early_depth_material_retention)||
        REXCVAR_GET(gow3_native_reuse_early_depth_readonly_boundaries)))
      throw std::invalid_argument("Retained opaque phase requires canonical FSI material composition without the early-depth retention experiment");
    if(REXCVAR_GET(gow3_native_retained_opaque_phase_render_pass_groups)&&
       (!REXCVAR_GET(gow3_native_retained_opaque_phase)||!fsi_state_material_composition_enabled||
        !native_raster||!deferred_queries||!unified_gpu_queue||
        !REXCVAR_GET(gow3_native_async_gpu_submissions)||!REXCVAR_GET(gow3_native_batch_transfers)||
        REXCVAR_GET(gow3_native_multi_draw_command_buffers)))
      throw std::invalid_argument("Retained phase pass groups require canonical asynchronous phase rendering without generic grouping");
    if(REXCVAR_GET(gow3_native_share_retained_opaque_phase_scratch)&&
       (!REXCVAR_GET(gow3_native_retained_opaque_phase)||!fsi_state_material_composition_enabled||
        !native_raster||!deferred_queries||!unified_gpu_queue||
        !REXCVAR_GET(gow3_native_async_gpu_submissions)||!REXCVAR_GET(gow3_native_batch_transfers)||
        REXCVAR_GET(gow3_native_reuse_retained_opaque_resources)))
      throw std::invalid_argument("Shared retained phase scratch requires an explicit canonical asynchronous phase without legacy resource pooling");
    if(REXCVAR_GET(gow3_native_poll_retained_opaque_phase_pressure)&&
       (!REXCVAR_GET(gow3_native_retained_opaque_phase)||!fsi_state_material_composition_enabled||
        !native_raster||!deferred_queries||!unified_gpu_queue||
        !REXCVAR_GET(gow3_native_async_gpu_submissions)||!REXCVAR_GET(gow3_native_batch_transfers)||
        REXCVAR_GET(gow3_native_reuse_retained_opaque_resources)))
      throw std::invalid_argument("Retained phase pressure polling requires an explicit canonical asynchronous phase without legacy resource pooling");
    if(REXCVAR_GET(gow3_native_share_retained_opaque_phase_attachments)&&
       (!REXCVAR_GET(gow3_native_retained_opaque_phase)||
        !REXCVAR_GET(gow3_native_share_retained_opaque_phase_scratch)||!fsi_state_material_composition_enabled||
        !native_raster||!deferred_queries||!unified_gpu_queue||
        !REXCVAR_GET(gow3_native_async_gpu_submissions)||!REXCVAR_GET(gow3_native_batch_transfers)||
        REXCVAR_GET(gow3_native_reuse_retained_opaque_resources)))
      throw std::invalid_argument("Shared retained phase attachments require an explicit canonical asynchronous phase with shared scratch and no legacy resource pooling");
    // This separate policy keeps the normal null-PS depth/query and early-depth
    // source families. It never silently disables their existing settings.
    if(fsi_state_material_composition_enabled) {
      if(!native_raster||!description.ordered_pixel_interlock||!description.features.fragmentStoresAndAtomics||
         !deferred_queries||!unified_gpu_queue||!REXCVAR_GET(gow3_native_async_gpu_submissions)||
         !REXCVAR_GET(gow3_native_batch_transfers)||
         REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)||REXCVAR_GET(gow3_native_fsi_state_forward_cfg)||
         REXCVAR_GET(gow3_native_multi_draw_command_buffers)||
         REXCVAR_GET(gow3_native_attachmentless_fsi)||
         REXCVAR_GET(gow3_native_implicit_fsi_barriers)||REXCVAR_GET(gow3_native_builtin_atomic_depth)||
         REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)||REXCVAR_GET(gow3_native_opaque_color_overwrite)||
         REXCVAR_GET(gow3_native_forward_control_flow)||REXCVAR_GET(gow3_native_forward_vertex_control_flow)||
         REXCVAR_GET(gow3_native_forward_pixel_catalog)||REXCVAR_GET(gow3_native_stencil_material_cutoff)||
         exact_fsi_stores_enabled||fsi_hoist_mode!=NativeFsiHoistMode::Disabled||
         REXCVAR_GET(gow3_native_retained_opaque_writer)||REXCVAR_GET(gow3_native_retained_opaque_catalog)||
         retained_fp24_depth_writer_enabled||retained_shared_depth_queries_enabled||retained_d32_composition_enabled||
         retain_primary_across_disjoint_queries_enabled||REXCVAR_GET(gow3_native_reuse_retained_opaque_resources)||
         (REXCVAR_GET(gow3_native_queue_retained_exports_for_draws)&&!REXCVAR_GET(gow3_native_retained_opaque_phase)))
        throw std::invalid_argument("FSI material composition requires canonical asynchronous Native rendering without other material variants");
      REXLOG_INFO("Native FSI material composition: policy={}, readonly_depth={}, early_depth={}, early_catalog={}, historical_lookup=true",
          kFsiStateMaterialCompositionPolicy,readonly_depth_attachment_enabled,early_depth_enabled,early_depth_catalog_enabled);
    }
    if(REXCVAR_GET(gow3_native_stencil_material_cutoff)&&
       (!native_raster||!description.ordered_pixel_interlock||!description.features.fragmentStoresAndAtomics||
        !unified_gpu_queue||!REXCVAR_GET(gow3_native_async_gpu_submissions)||
        REXCVAR_GET(gow3_native_attachmentless_fsi)||REXCVAR_GET(gow3_native_implicit_fsi_barriers)||
        REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)||REXCVAR_GET(gow3_native_fsi_state_forward_cfg)||
        exact_fsi_stores_enabled||fsi_hoist_mode!=NativeFsiHoistMode::Disabled||
        REXCVAR_GET(gow3_native_retained_opaque_writer)||REXCVAR_GET(gow3_native_retained_opaque_catalog)||
        retained_fp24_depth_writer_enabled||REXCVAR_GET(gow3_native_opaque_color_overwrite)||
        REXCVAR_GET(gow3_native_forward_control_flow)||REXCVAR_GET(gow3_native_forward_vertex_control_flow)||
        REXCVAR_GET(gow3_native_forward_pixel_catalog)))
      throw std::invalid_argument("Native stencil material cutoff requires the canonical asynchronous packed Native provider");
    if(retain_primary_across_disjoint_queries_enabled&&!retained_d32_composition_enabled)
      throw std::invalid_argument("Disjoint retained queries require explicit retained D32 composition");
    if(retained_d32_composition_enabled) {
      const bool opaque=REXCVAR_GET(gow3_native_retained_opaque_writer)||REXCVAR_GET(gow3_native_retained_opaque_catalog);
      if(!readonly_depth_attachment_enabled||!deferred_queries||!unified_gpu_queue||
         !REXCVAR_GET(gow3_native_async_gpu_submissions)||!REXCVAR_GET(gow3_native_batch_transfers)||
         opaque==retained_fp24_depth_writer_enabled||
         (REXCVAR_GET(gow3_native_retained_opaque_writer)&&REXCVAR_GET(gow3_native_retained_opaque_catalog))||
         retained_shared_depth_queries_enabled||REXCVAR_GET(gow3_native_readonly_depth_skip_edram_barrier)||
         fsi_hoist_mode!=NativeFsiHoistMode::Disabled||exact_fsi_stores_enabled)
        throw std::invalid_argument("Retained D32 composition requires one exact primary writer and canonical deferred D32 Native queries");
    }
    if(!NativeFsiHoistModeKnown(fsi_hoist_mode))
      throw std::invalid_argument("Native FSI hoist mode must be 0, 1 or 2");
    if(fsi_hoist_mode!=NativeFsiHoistMode::Disabled) {
      if(exact_fsi_stores_enabled||!description.ordered_pixel_interlock||!description.features.fragmentStoresAndAtomics||
         REXCVAR_GET(gow3_native_implicit_fsi_barriers)||REXCVAR_GET(gow3_native_builtin_atomic_depth)||
         REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)||REXCVAR_GET(gow3_native_opaque_color_overwrite)||
         REXCVAR_GET(gow3_native_forward_control_flow)||REXCVAR_GET(gow3_native_forward_vertex_control_flow)||
         REXCVAR_GET(gow3_native_forward_pixel_catalog)||REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)||
         REXCVAR_GET(gow3_native_fsi_state_forward_cfg)||REXCVAR_GET(gow3_native_retained_opaque_writer)||
         REXCVAR_GET(gow3_native_retained_opaque_catalog)||retained_fp24_depth_writer_enabled||retained_shared_depth_queries_enabled||
         REXCVAR_GET(gow3_native_queue_retained_exports_for_draws)||REXCVAR_GET(gow3_native_reuse_retained_opaque_resources)||
         REXCVAR_GET(gow3_native_attachmentless_fsi))
        throw std::invalid_argument("FSI hoist requires the exclusive Native FSI provider");
      fsi_hoist_compiler=std::make_unique<NativeFsiHoistCompiler>(compiler,fsi_hoist_mode,
          cache.empty()?std::filesystem::path{}:cache/"native-fsi-hoist");
    }
    if(exact_fsi_stores_enabled) {
      if(!description.ordered_pixel_interlock||!description.features.fragmentStoresAndAtomics||
         REXCVAR_GET(gow3_native_implicit_fsi_barriers)||REXCVAR_GET(gow3_native_builtin_atomic_depth)||
         REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)||REXCVAR_GET(gow3_native_opaque_color_overwrite)||
         REXCVAR_GET(gow3_native_forward_control_flow)||REXCVAR_GET(gow3_native_forward_vertex_control_flow)||
         REXCVAR_GET(gow3_native_forward_pixel_catalog)||REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)||
         REXCVAR_GET(gow3_native_fsi_state_forward_cfg)||REXCVAR_GET(gow3_native_retained_opaque_writer)||
         REXCVAR_GET(gow3_native_retained_opaque_catalog)||retained_fp24_depth_writer_enabled||retained_shared_depth_queries_enabled||
         REXCVAR_GET(gow3_native_queue_retained_exports_for_draws)||REXCVAR_GET(gow3_native_reuse_retained_opaque_resources)||
         REXCVAR_GET(gow3_native_attachmentless_fsi))
        throw std::invalid_argument("Exact FSI stores require the exclusive Native FSI provider");
      exact_fsi_compiler=std::make_unique<NativeFsiExactStoresCompiler>(compiler,
          cache.empty()?std::filesystem::path{}:cache/"native-fsi-exact-stores");
    }
    if(REXCVAR_GET(gow3_native_pipeline_prewarm)&&native_raster&&!cache.empty())try {
      pipeline_recipes=std::make_shared<NativePipelineRecipeCache>(
          cache/"native-pipeline-recipes"/"v1"/NativeShaderCompiler::CacheVersion(),NativeShaderCompiler::CacheVersion());
    }catch(const std::exception& error){REXLOG_WARN("Observed native pipeline cache unavailable: {}",error.what());}
    if(REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)&&REXCVAR_GET(gow3_native_fsi_state_forward_cfg))
      throw std::invalid_argument("Pure FSI state and composed forward FSI state are separate experiments");
    if(REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)||REXCVAR_GET(gow3_native_fsi_state_forward_cfg)||fsi_state_material_composition_enabled) {
      if(REXCVAR_GET(gow3_native_implicit_fsi_barriers)||REXCVAR_GET(gow3_native_builtin_atomic_depth)||
         REXCVAR_GET(gow3_native_opaque_color_overwrite)||REXCVAR_GET(gow3_native_forward_control_flow)||
         REXCVAR_GET(gow3_native_forward_vertex_control_flow)||REXCVAR_GET(gow3_native_forward_pixel_catalog)||
         REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)||(readonly_depth_attachment_enabled&&!fsi_state_material_composition_enabled))
        throw std::invalid_argument("FSI state specialization requires exclusive Native shader experiment");
      if(!cache.empty()&&pipeline_recipes)try {
        fsi_state_shader_cache=cache/(REXCVAR_GET(gow3_native_fsi_state_forward_cfg)?
            "native-fsi-state-forward-shaders":"native-fsi-state-shaders");
        // Existing exact-state observations are canonical-source-authenticated
        // ranking input only. Derived modules never use that variant namespace.
        fsi_state_recipes=std::make_shared<NativeFsiStateRecipeCache>(cache/"native-fsi-state-recipes"/"v1"/
            FsiStateSpecializationCompilerVersion(),FsiStateSpecializationCompilerVersion());
      }catch(const std::exception& error){REXLOG_WARN("FSI state recipe cache unavailable: {}",error.what());}
    }
    memory_callback=memory->RegisterPhysicalMemoryInvalidationCallback(
        [](void* context,uint32_t address,uint32_t bytes,bool) {
          auto& state=*static_cast<RuntimeState*>(context);
          state.RecordCpuWrite(address,bytes);
          state.memory_coherence.Invalidate(address,bytes);
          // Even an unrelated fault must not unprotect neighboring pages we
          // still own: SDK callbacks may otherwise expand the unwatch to 4 MiB.
          return std::pair{address,bytes};
        },this);
  }

  void RecordCpuWrite(uint32_t address,uint32_t bytes) {
    if(!bytes||address>=NativeMemoryCoherence::kCapacity)return;
    const auto epoch=++next_write_epoch;
    const auto last=(std::min<uint64_t>(uint64_t(address)+bytes,NativeMemoryCoherence::kCapacity)-1)/NativeMemoryCoherence::kPage;
    for(size_t page=address/NativeMemoryCoherence::kPage;page<=last;++page) {
      auto previous=write_epochs[page].load();
      while(previous<epoch&&!write_epochs[page].compare_exchange_weak(previous,epoch)) {}
    }
  }

  uint64_t LatestCpuWrite(uint32_t address,uint32_t bytes) const {
    if(!bytes||uint64_t(address)+bytes>NativeMemoryCoherence::kCapacity)
      throw std::runtime_error("Invalid native physical memory range");
    uint64_t epoch=0;
    for(size_t page=address/NativeMemoryCoherence::kPage;page<=(uint64_t(address)+bytes-1)/NativeMemoryCoherence::kPage;++page)
      epoch=std::max(epoch,write_epochs[page].load());
    return epoch;
  }

  void StopMemoryCallbacks() {
    if(memory_callback)memory->UnregisterPhysicalMemoryInvalidationCallback(std::exchange(memory_callback,nullptr));
  }

  void SaveFailure(const GraphicsBoundaryEvent& event, const std::string& reason) {
    if (failure_directory.empty()) return;
    try {
      const auto directory = failure_directory / ("frame-" + std::to_string(event.frame) +
          "-event-" + std::to_string(event.sequence));
      if (std::filesystem::exists(directory)) return;
      std::filesystem::create_directories(directory);
      nlohmann::json metadata{{"frame", event.frame}, {"event", event.sequence}, {"function", event.function},
          {"phase", event.phase}, {"result", event.result}, {"lr", event.lr}, {"args", event.args},
          {"error", reason}, {"blocks", nlohmann::json::array()}};
      if(event.command_span) {
        const auto& span=*event.command_span;
        metadata["command_span"]={{"generation",span.generation},{"api_sequence",span.api_sequence},
            {"address",span.address},{"bytes",span.bytes.size()},{"path","command-span.bin"}};
        std::ofstream commands(directory/"command-span.bin",std::ios::binary);
        commands.write(reinterpret_cast<const char*>(span.bytes.data()),span.bytes.size());
        if(!commands)throw std::runtime_error("Cannot write failed command span");
      }
      size_t saved = 0;
      for (const auto& block : event.blocks) {
        const auto bytes=block.data();
        const bool include = bytes.size() <= 64 * 1024 * 1024 - saved;
        const auto name = "block-" + std::to_string(block.kind) + ".bin";
        metadata["blocks"].push_back({{"kind", block.kind}, {"address", block.address},
            {"bytes", bytes.size()}, {"deferred_physical_bytes",block.deferred_physical_bytes},
            {"saved", include&&!block.deferred_physical_bytes}, {"path", name}});
        if (include&&!block.deferred_physical_bytes) {
          std::ofstream file(directory / name, std::ios::binary);
          file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
          if (!file) throw std::runtime_error("Cannot write failed event block");
          saved += bytes.size();
        }
      }
      std::ofstream manifest(directory / "event.json");
      manifest << metadata.dump(2) << '\n';
      if (!manifest) throw std::runtime_error("Cannot write failed event metadata");
      REXLOG_INFO("Unsupported native API event saved to {}", directory.string());
    } catch (const std::exception& error) {
      REXLOG_ERROR("Cannot save native renderer failure: {}", error.what());
    }
  }

  void SaveQueryMismatch(const nlohmann::json& comparison) {
    if (failure_directory.empty() || query_diagnostics >= 2 ||
        !comparison.at("visibility_mismatches").get<uint64_t>()) return;
    try {
      const auto directory = failure_directory / ("query-frame-" + std::to_string(active_frame->frame));
      if (std::filesystem::exists(directory)) return;
      ++query_diagnostics;
      std::filesystem::create_directories(directory);
      const auto write_json = [&](const char* name, const nlohmann::json& value) {
        std::ofstream file(directory / name);
        file << value.dump(2) << '\n';
        if (!file) throw std::runtime_error("Cannot write native query diagnostics");
      };
      write_json("operations.json", active_frame->operations.ToJson());
      write_json("comparison.json", comparison);
      write_json("renderer-report.json", renderer->report());
      constexpr size_t limit = 64 * 1024 * 1024;
      size_t saved_bytes = 0;
      nlohmann::json manifest = {{"frame", active_frame->frame}, {"files", nlohmann::json::array()}};
      std::unordered_set<std::string> saved;
      const auto save = [&](const std::string& hash, bool shader) {
        const auto name = hash + (shader ? ".shader.bin" : ".payload.bin");
        if (!saved.insert(name).second) return;
        const auto& data = *active_frame->ResourceAt(hash,shader);
        const bool include = data.size() <= limit - saved_bytes;
        manifest["files"].push_back({{"path", name}, {"bytes", data.size()}, {"included", include}});
        if (!include) return;
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        if (!file) throw std::runtime_error("Cannot write native query payload");
        saved_bytes += data.size();
      };
      // Preserve draw state and geometry before large optional texture data.
      for (const auto& input : active_frame->operations) {
        const auto operation=input.ToJson();
        if (operation.contains("device_state")) save(operation.at("device_state"), false);
        if (operation.contains("geometry"))
          for (const auto& geometry : operation.at("geometry"))
            if (geometry.contains("sha256")) save(geometry.at("sha256"), false);
      }
      // Preserve the previous map traversal for capped diagnostic exports.
      for (const auto& [hash, data] : active_frame->MaterializeResources(true)) save(hash, true);
      for (const auto& [hash, data] : active_frame->MaterializeResources(false)) save(hash, false);
      manifest["saved_bytes"] = saved_bytes;
      write_json("manifest.json", manifest);
      REXLOG_INFO("Native query mismatch diagnostics saved to {}", directory.string());
    } catch (const std::exception& error) {
      REXLOG_ERROR("Cannot save native query diagnostics: {}", error.what());
    }
  }

  void SaveExecutionFailure(const LiveOperation& input,const std::string& reason) {
    if(failure_directory.empty()||!active_frame)return;
    try {
      const auto operation=input.ToJson();
      const auto directory=failure_directory/("execution-frame-"+std::to_string(active_frame->frame)+
          "-event-"+std::to_string(operation.at("event").get<uint64_t>()));
      if(std::filesystem::exists(directory))return;
      std::filesystem::create_directories(directory);
      nlohmann::json manifest{{"frame",active_frame->frame},{"operation",operation},{"error",reason},
          {"files",nlohmann::json::array()}};
      std::unordered_set<std::string> saved;
      size_t saved_bytes=0;
      const auto retain=[&](auto&& self,const nlohmann::json& value)->void {
        if(value.is_string()) {
          const auto& hash=value.get_ref<const std::string&>();
          for(bool shader:{false,true}) {
            const auto* found=active_frame->FindResource(hash,shader);
            if(!found)continue;
            const auto name=hash+(shader?".shader.bin":".payload.bin");
            if(!saved.insert(name).second)continue;
            const auto& bytes=**found;
            const bool include=bytes.size()<=64*1024*1024-saved_bytes;
            manifest["files"].push_back({{"path",name},{"bytes",bytes.size()},{"included",include}});
            if(!include)continue;
            std::ofstream output(directory/name,std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            if(!output)throw std::runtime_error("Cannot save failed native operation payload");
            saved_bytes+=bytes.size();
          }
        } else if(value.is_structured())for(const auto& item:value)self(self,item);
      };
      // Preserve the register ABI before optional large texture snapshots.
      if(operation.contains("device_state"))retain(retain,operation.at("device_state"));
      retain(retain,operation);
      std::ofstream output(directory/"operation.json");output<<manifest.dump(2)<<'\n';
      if(!output)throw std::runtime_error("Cannot save failed native operation metadata");
      REXLOG_INFO("Unsupported native execution saved to {}",directory.string());
    }catch(const std::exception& error){REXLOG_ERROR("Cannot save native execution failure: {}",error.what());}
  }

  nlohmann::json PacketDiagnostic(const Processor::NativeDrawBoundary& boundary) const {
    return {{"address",boundary.command_address},{"primitive",boundary.primitive},
        {"count",boundary.index_count},{"edram_mode",boundary.edram_mode},{"color_mask",boundary.color_mask},
        {"depth_control",boundary.depth_control},{"surface_info",boundary.surface_info},
        {"clip_control",boundary.clip_control},{"vte_control",boundary.vte_control},
        {"index_offset",boundary.index_offset},{"copy_destination",boundary.copy_destination},
        {"copy_destination_info",boundary.copy_destination_info},{"proven_no_effect",boundary.proven_no_effect},
        {"bin_mask",boundary.bin_mask},{"bin_select",boundary.bin_select},
        {"packet_predicated",boundary.packet_predicated},{"predicate_passed",boundary.predicate_passed},
        {"native_query_active",bool(processor->ActiveNativeQuery())},
        {"vertex_shader_words",std::vector<uint32_t>(boundary.vertex_shader_words.begin(),boundary.vertex_shader_words.end())},
        {"packet_bytes",std::vector<uint8_t>(boundary.packet_bytes.begin(),boundary.packet_bytes.end())}};
  }

  void SaveUnmatchedPacket(const Processor::NativeDrawBoundary& boundary) {
    if(failure_directory.empty()||unmatched_draw_packets>32)return;
    try {
      const auto directory=failure_directory/"unmatched-packets";
      std::filesystem::create_directories(directory);
      auto data=PacketDiagnostic(boundary);
      {
        std::lock_guard lock(pending_mutex);
        data["pending_span_count"]=pending_packets.size();
        if(!pending_packets.empty()) {
          const auto& packet=pending_packets.front().prefix;
          data["pending_front"]={{"generation",packet.span.generation},{"api_sequence",packet.span.api_sequence},
              {"address",packet.span.address},{"bytes",packet.span.bytes},{"frame",packet.batch->frame},
              {"first_operation",packet.first_operation},{"operation_end",packet.operation_end},
              {"operations",packet.batch->operations.ToJson()}};
        }
      }
      std::ofstream file(directory/(std::to_string(unmatched_draw_packets)+".json"));
      file<<data.dump(2)<<'\n';
      if(!file)throw std::runtime_error("Cannot save unmatched packet");
    }catch(const std::exception& error){REXLOG_ERROR("Native packet diagnostic failed: {}",error.what());}
  }

  void SaveFailedPacket(const Processor::NativeDrawBoundary& boundary,const QueuedPacket& packet,
                        const PacketOwnership::Match& match,const std::string& reason) {
    if(failure_directory.empty()||!packet.prefix.batch)return;
    try {
      const auto directory=failure_directory/"failed-packet";
      std::filesystem::create_directories(directory);
      auto data=PacketDiagnostic(boundary);
      data["reason"]=reason;data["frame"]=packet.prefix.batch->frame;
      data["generation"]=match.generation;data["api_sequence"]=match.api_sequence;
      data["draw_index"]=match.draw_index;data["last_draw"]=match.last_draw;
      data["replacement_safe"]=match.replacement_safe;
      data["first_operation"]=packet.prefix.first_operation;data["operation_end"]=packet.prefix.operation_end;
      data["operations"]=packet.prefix.batch->operations.ToJson();
      data["span_address"]=packet.prefix.span.address;
      data["span_bytes"]=packet.prefix.span.bytes;
      std::ofstream file(directory/"packet.json");file<<data.dump(2)<<'\n';
      if(!file)throw std::runtime_error("Cannot save failed packet");
    }catch(const std::exception& error){REXLOG_ERROR("Native packet diagnostic failed: {}",error.what());}
  }

  // Command thread only. True when a Get for this shader will not compile:
  // in memory, on disk, finished in the background, or past the wait budget.
  bool AsyncShaderReady(const CanonicalResourceRef& resource,bool fsi) {
    if(!shader_prewarm||!resource.owner)return true;  // Nothing can compile in the background.
    if(fsi&&(fsi_hoist_compiler||exact_fsi_compiler))return true;  // Derived compilers: synchronous.
    const auto digest=resource.hash.View();
    std::string key(digest);if(fsi)key+=".fsi";
    if(async_ready.contains(key))return true;
    bool ready=false;
    {std::error_code error;ready=std::filesystem::exists(native_shader_cache/(key+".g3spv"),error);}
    const std::string job_key=std::string(digest)+(fsi?".fsi":".raster");
    if(!ready){std::lock_guard lock(async_finished_mutex);ready=async_finished.contains(job_key);}
    if(ready) {async_ready.insert(std::move(key));async_waiting.erase(job_key);return true;}
    const auto now=std::chrono::steady_clock::now();
    const auto [waiting,first]=async_waiting.try_emplace(job_key,now);
    if(first) {
      // Starts the compile on a real-request worker, or promotes it out of
      // the speculative lane. A running or already-queued job is unchanged.
      async_enqueued+=shader_prewarm->TryEnqueue({job_key,resource.owner,fsi,false});
    } else if(now-waiting->second>=async_shader_budget) {
      ++async_budget_blocks;async_waiting.erase(waiting);
      async_ready.insert(std::move(key));return true;  // Block in Get like the synchronous path.
    }
    ++async_not_ready;return false;
  }

  std::shared_ptr<const CompiledNativeShader> CompileGameplayShader(
      const std::vector<uint8_t>& source,bool fsi,std::string_view hash) {
    using Clock=std::chrono::steady_clock;
    const auto nanoseconds=[](Clock::time_point time) {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          time.time_since_epoch()).count());
    };
    ++shader_provider_requests;
    std::string key;
    NativeShaderPrewarmQueue::JobInspection before{};
    before.coverage_complete=false;
    try {
      key=std::string(hash)+(fsi?".fsi":".raster");
      if(shader_prewarm)before=shader_prewarm->Inspect(key);
    }catch(...) {before.coverage_complete=false;} // Optional evidence cannot reject a draw.
    const auto began=Clock::now();
    const auto shader=fsi&&fsi_hoist_compiler?fsi_hoist_compiler->Get(source):
        fsi&&exact_fsi_compiler?exact_fsi_compiler->Get(source):
        compiler.Get(source,fsi?NativeShaderMode::FragmentInterlock:NativeShaderMode::Raster);
    const auto ended=Clock::now();
    const double milliseconds=std::chrono::duration<double,std::milli>(ended-began).count();
    if(milliseconds>=5.0)try {
      if(slow_shader_preparation_count==slow_shader_preparations.size()) {
        ++slow_shader_preparation_dropped;
      } else {
        SlowShaderPreparation sample;
        sample.key=std::move(key);sample.before=before;
        sample.after.coverage_complete=false;
        if(shader_prewarm)sample.after=shader_prewarm->Inspect(sample.key);
        sample.requested_ns=nanoseconds(began);sample.returned_ns=nanoseconds(ended);
        sample.milliseconds=milliseconds;
        // This describes the shared compiler result's first production, not a
        // disk access by this particular Get (which may have waited on it).
        sample.original_compilation_disk_cache_hit=shader->disk_cache_hit;
        if(active_frame) {
          sample.frame=active_frame->frame;
          if(!active_frame->operations.empty()) {
            sample.prefix_first_event=active_frame->operations.at(0).Event();
            sample.prefix_last_event=active_frame->operations.back().Event();
          }
        }
        slow_shader_preparations[slow_shader_preparation_count++]=std::move(sample);
      }
    }catch(...) {++slow_shader_preparation_dropped;}
    return shader;
  }

  void LogShaderPreparation() noexcept {
    try {
      using Json=nlohmann::json;
      const auto inspection=[](const NativeShaderPrewarmQueue::JobInspection& s) {
        const char* status="unknown";
        using Status=NativeShaderPrewarmQueue::JobStatus;
        switch(s.status) {
          case Status::Queued:status="queued";break;
          case Status::Running:status="running";break;
          case Status::Completed:status="completed";break;
          case Status::Failed:status="failed";break;
          default:break;
        }
        return Json{{"status",status},{"coverage_complete",s.coverage_complete},
                    {"queued_ns",s.queued_ns},{"started_ns",s.started_ns},{"finished_ns",s.finished_ns}};
      };
      Json samples=Json::array();
      for(size_t i=0;i<slow_shader_preparation_count;++i) {
        const auto& s=slow_shader_preparations[i];
        samples.push_back({{"key",s.key},{"frame",s.frame},
            {"prefix_first_event",s.prefix_first_event},{"prefix_last_event",s.prefix_last_event},
            {"requested_ns",s.requested_ns},{"returned_ns",s.returned_ns},{"milliseconds",s.milliseconds},
            {"original_compilation_disk_cache_hit",s.original_compilation_disk_cache_hit},
            {"before",inspection(s.before)},{"after",inspection(s.after)}});
      }
      const Json report={{"scope","cold gameplay shader-provider requests; prefix events are bounds, not an exact draw"},
          {"prewarm_enabled",bool(shader_prewarm)},{"generic_compiler",!fsi_hoist_compiler&&!exact_fsi_compiler},
          {"clock","steady_clock_nanoseconds"},{"requests",shader_provider_requests},
          {"threshold_ms",5.0},{"maximum_samples",slow_shader_preparations.size()},
          {"dropped",slow_shader_preparation_dropped},{"samples",std::move(samples)}};
      REXLOG_INFO("Native shader preparation: {}",report.dump());
      if(shader_prewarm) {
        const auto d=shader_prewarm->diagnostics();
        Json failures=Json::array();
        for(const auto& f:d.failures)failures.push_back({{"key",f.key},{"reason",f.reason},
            {"queued_ns",f.queued_ns},{"started_ns",f.started_ns},{"finished_ns",f.finished_ns},
            {"key_truncated",f.key_truncated},{"reason_truncated",f.reason_truncated}});
        const Json details={{"entries",d.entries},{"maximum_entries",d.maximum_entries},
            {"coverage_complete",d.coverage_complete},{"history_evictions",d.history_evictions},
            {"metadata_failures",d.metadata_failures},{"key_refusals",d.key_refusals},
            {"failure_records_dropped",d.failure_records_dropped},{"failures",std::move(failures)}};
        REXLOG_INFO("Native shader prewarm diagnostics: {}",details.dump());
      }
    }catch(...) {} // Shutdown diagnostics cannot change successful rendering.
  }

  void StartShaderPrewarm() {
    if(!REXCVAR_GET(gow3_native_shader_prewarm))return;
    try {
      shader_prewarm=std::make_shared<NativeShaderPrewarmQueue>([this](const NativeShaderPrewarmQueue::Job& job) {
        // Async shaders treat a failed compile as finished too: the draw then
        // takes the ordinary path and reports the same failure.
        struct Finished {
          RuntimeState& owner;const std::string& key;
          ~Finished(){try{std::lock_guard lock(owner.async_finished_mutex);owner.async_finished.insert(key);}catch(...){}}
        } finished{*this,job.key};
        if(job.fragment_interlock&&fsi_hoist_compiler)fsi_hoist_compiler->Get(*job.source);
        else if(job.fragment_interlock&&exact_fsi_compiler)exact_fsi_compiler->Get(*job.source);
        else compiler.Get(*job.source,job.fragment_interlock?NativeShaderMode::FragmentInterlock:NativeShaderMode::Raster);
      },unsigned(REXCVAR_GET(gow3_native_shader_prewarm_threads)),NativeShaderPrewarmQueue::kMaxJobs,
        NativeShaderPrewarmQueue::kMaxBytes,REXCVAR_GET(gow3_native_block_shader_prewarm));
      std::weak_ptr<NativeShaderPrewarmQueue> weak=shader_prewarm;
      const bool use_fsi=description.ordered_pixel_interlock&&description.features.fragmentStoresAndAtomics;
      Gow3SetShaderDiscoverySink([weak,fsi=use_fsi](std::shared_ptr<const std::vector<uint8_t>> source,bool pixel,bool speculative) {
        if(auto queue=weak.lock()) {
          const bool fragment_interlock=fsi&&pixel;
          auto key=ContentHash(*source)+(fragment_interlock?".fsi":".raster");
          queue->TryEnqueue({std::move(key),std::move(source),fragment_interlock,speculative});
        }
      });
      REXLOG_INFO("Native shader prewarm enabled: workers={}, max_jobs={}, max_source_bytes={}",
          shader_prewarm->statistics().workers,NativeShaderPrewarmQueue::kMaxJobs,NativeShaderPrewarmQueue::kMaxBytes);
    }catch(const std::exception& error) {
      Gow3SetShaderDiscoverySink({});
      if(shader_prewarm)shader_prewarm->StopAndJoin();
      shader_prewarm.reset();
      REXLOG_WARN("Native shader prewarm unavailable: {}; ordinary draw compilation retained",error.what());
    }
  }
  void StopShaderDiscovery() {
    Gow3SetPublishedQueryResults({});
    Gow3SetQueryReadinessProbe({});
    Gow3EnableNativeQueryPollYield(false);
    if(pipeline_recipes)pipeline_recipes->StopAccepting();
    if(fsi_state_recipes)fsi_state_recipes->StopAccepting();
    Gow3SetShaderDiscoverySink({});
    if(shader_prewarm)shader_prewarm->StopAccepting();
  }

  // Same parseable Scene counters for verbose frame reports and cold quiet-mode health.
  void LogSceneCounters() {
    for(const auto* key:{"device","page_versions","cpu_stage_timings","texture_filtering","fsi_immutable_state_hoist","exact_fsi_stores","draw_batches","draw_command_groups","async_shaders","async_pipelines","fsi_draw_pass_groups","retained_opaque_phase_render_pass_groups","packed_color_render_pass_groups","packed_color_blend_pass_groups","packed_color_action_pass_groups","packed_depth_render_pass_groups","readonly_query_render_pass_groups","native_readonly_depth_no_write_metadata","dummy_fsi_render_pass","attachmentless_fsi_render_pass","cpu_memory_writes","cpu_write_upload_reuse","cpu_upload_receipts","vertex_buffer_pool","fsi_raster_attachment","texture_layout_cache","resolve_image_pool","resolve_descriptor_pool","pass_transfer_descriptor_pool","pass_transfer_render_pass_cache","resolve_identity_views","texture_eviction_reload_census","cpu_texture_drains","cpu_image_slot_lookup","draw_descriptors","unused_descriptor_sets","draw_descriptor_content_cache","constant_delta_uploads","pinned_null_constant_uploads","draw_recording","packed_zero_depth_clear","packed_primary_stencil_clear","packed_secondary_stencil_clear","atlas_depth_region_restores","fused_atlas_depth_clear","direct_atlas_depth_resolves","fused_packed_atlas_resolves","direct_packed_color_resolves","full_color_clear_restore","draw_barriers","builtin_atomic_depth","builtin_readonly_queries","native_readonly_depth_attachment","native_early_depth","native_early_depth_catalog","native_early_depth_guard_reuse","native_early_depth_material_retention","retained_fp24_depth_writer","retained_opaque_writer","retained_opaque_catalog","retained_opaque_phase","retained_shared_depth_queries","retained_disjoint_queries","native_stencil_material_cutoff","opaque_color_overwrite","forward_control_flow","forward_vertex_control_flow","forward_pixel_catalog","fsi_state_specialization","fsi_state_material_composition","cpu_texture_footprint_cache","gpu_alias_state_cache","compact_resolve_channels","canonical_resolve_stores","texture_updates","transfer_batches","gpu_work_queue","query_prefix_retirement","query_resets","alias_generations","gpu_timings",
                        "queued_volume_alias_loads","pipeline_cache","shader_cache","shader_lookup_lookaside","pipeline_lookup_lookaside","cpu_image_lookup_lookaside","msaa_fallback","index_cache","texture_cache","vertex_cache","full_cpu_texture_overlays","cpu_texture_sampling_storage","raw_gpu_float_cpu_textures","gpu_batch_attribution","gpu_physical_timings","gpu_draw_timings", "presentation_gpu_timings","depth_stencil_restores","surface_conversions","hardware_render_passes"})
      if(const auto values=renderer->report().find(key);values!=renderer->report().end())
        REXLOG_INFO("Native {}: {}",key,values->dump());
  }

  ~RuntimeState() {
    stopping=true;pending_changed.notify_all();StopPlanner();
    // SDK callbacks and retained presentation images hold shared Runtime owners.
    // Their final release makes this cold export safe; UI Stop only queues CP shutdown.
    const auto journal_result=frame_journal.Finish({stopping.load(),failed.load(),
        completed_frames.load(),last_completed_api_frame.load()});
    try {
      if(frame_journal.Requested())REXLOG_INFO("Native frame journal: status={}, stored={}, observed={}, dropped={}, recording_failed={}",
          NativeFrameJournal::Name(journal_result),frame_journal.Rows().size(),frame_journal.Observed(),
          frame_journal.Dropped(),frame_journal.RecordingFailed());
    }catch(...) {} // Optional status formatting must not affect normal destruction.
    LogQueryReadiness("final");
    if(fsi_state_recipes){fsi_state_recipes->StopAccepting();fsi_state_recipes->Drain();}

    if(pipeline_recipes) {
      pipeline_recipes->StopAccepting();pipeline_recipes->Drain();
      const auto stats=pipeline_recipes->statistics();
      REXLOG_INFO("Observed native pipeline cache: admitted={}, written={}, duplicates={}, dropped={}, failures={}, pending={}, peak_pending={}, peak_bytes={}",
          stats.admitted,stats.written,stats.duplicate,stats.dropped,stats.failures,stats.pending,stats.peak_pending,stats.peak_pending_bytes);
      pipeline_recipes.reset();
    }
    if(shader_prewarm) {
      shader_prewarm->StopAndJoin();
      const auto stats=shader_prewarm->statistics();
      REXLOG_INFO("Native shader prewarm final: workers={}, attempted={}, admitted={}, duplicate={}, full={}, stopped={}, started={}, completed={}, failures={}, cancelled={}, outstanding={}, retained_bytes={}, peak_jobs={}, peak_bytes={}",
          stats.workers,stats.attempted,stats.admitted,stats.duplicate,stats.full,stats.stopped,
          stats.started,stats.completed,stats.failures,stats.cancelled,stats.outstanding,
          stats.retained_bytes,stats.peak_jobs,stats.peak_bytes);
      if(stats.speculative_worker)REXLOG_INFO("Native speculative shader prewarm: admitted={}, started={}, promoted={}",
          stats.speculative_admitted,stats.speculative_started,stats.promoted);
      if(async_shaders)REXLOG_INFO("Native async shaders: not_ready_checks={}, enqueued={}, budget_blocks={}, budget_ms={}",
          async_not_ready,async_enqueued,async_budget_blocks,async_shader_budget.count());
    }
    LogShaderPreparation();
    StopMemoryCallbacks();
    completed_frame_disposer.StopAndDrain();
    const auto disposal=completed_frame_disposer.statistics();
    REXLOG_INFO("Native CPU snapshot disposal: worker={}, accepted={}, completed={}, inline={}, peak_outstanding_frames={}, peak_retained_payload_bytes={}",
                disposal.worker_started,disposal.accepted_frames,disposal.completed_frames,disposal.inline_frames,
                disposal.peak_outstanding_frames,disposal.peak_retained_payload_bytes);
    REXLOG_INFO("Native renderer final counters: frames={}, query_prefixes={}, raster_packets={}, copy_packets={}, matched_packets={}, unmatched_packets={}, no_effect_packets={}, reference_retired={}",
                completed_frames.load(),completed_query_prefixes,native_raster_packets,replaced_copy_packets,
                matched_draw_packets,unmatched_draw_packets,proven_no_effect_packets,reference_retired);
    REXLOG_INFO("Native deferred query counters: enabled={}, deferred={}, completed={}, pending={}, ready={}, scene_pending={}, peak_pending={}",
                deferred_queries,deferred_query_prefixes,completed_query_prefixes,pending_query_publications.size(),
                ready_query_publications.size()-next_ready_query,renderer?renderer->DeferredQueryCount():0,peak_deferred_queries);
    LogQueryPressure(last_completed_api_frame.load());
    REXLOG_INFO("Native final state identity mode: {}",planner.LazyStateMode());
    {
      const auto device_states=planner.DeviceStateStats();
      REXLOG_INFO("Native lazy device states: calls={} current_hits={} previous_hits={} created={} collisions={}",
          device_states.calls,device_states.current_hits,device_states.previous_hits,device_states.created,
          device_states.collisions);
      const auto payloads=planner.PayloadInternStats();
      static constexpr const char* kClasses[]={"vertex_index","up","texture","other"};
      for(size_t c=0;c<4;++c)
        REXLOG_INFO("Native payload intern {}: snapshot={}/{}MB current={}/{}MB previous={}/{}MB created={}/{}MB",kClasses[c],
            payloads.calls[c][0],payloads.bytes[c][0]>>20,payloads.calls[c][1],payloads.bytes[c][1]>>20,
            payloads.calls[c][2],payloads.bytes[c][2]>>20,payloads.calls[c][3],payloads.bytes[c][3]>>20);
    }
    if(async_planner) {
      const double ends=std::max<double>(1,double(planner_query_ends.load()));
      REXLOG_INFO("Native async planner: items={} producer_waits={} peak_queue={} query_ends={} "
          "queue_us={:.0f} plan_us={:.0f} cp_queue_us={:.0f} pending_waits={} pending_wait_ms_total={:.1f}",
          planner_items.load(),planner_producer_waits.load(),planner_peak_queue,planner_query_ends.load(),
          planner_queue_ns.load()/1e3/ends,planner_plan_ns.load()/1e3/ends,planner_cp_ns.load()/1e3/ends,
          planner_pending_waits.load(),planner_pending_wait_ns.load()/1e6);
    }
    // A presented image retains this state through its host submission fence.
    // Hence no allocator/device can disappear while a swap still samples it.
    if (device) {
      try { device->WaitIdle(); }
      catch (const std::exception& error) { REXLOG_ERROR("Native renderer shutdown: {}", error.what()); }
    }
    if(renderer&&!verbose_frame_reports)try {
      // Final shared-owner release excludes CP callbacks. Ordinary shutdown
      // already fenced accepted work; a diagnostic must never seal pending work.
      if(renderer->HasPendingGpuWork()) {
        REXLOG_WARN("Native quiet final scene counters unavailable: GPU work remains pending");
      } else {
        LogSceneCounters();
        REXLOG_INFO("Native quiet final scene counters: complete");
      }
    }catch(const std::exception& error) {
      try {REXLOG_WARN("Native quiet final scene counters unavailable: {}",error.what());}catch(...) {}
    }catch(...) {
      try {REXLOG_WARN("Native quiet final scene counters unavailable");}catch(...) {}
    }
    if(renderer&&!gpu_batch_report_path.empty())try {
      // Query only already-retired diagnostic records. Reporting introduces
      // no queue submission, fence wait or resource lifetime change.
      auto report=renderer->GpuBatchAttributionReport(true);
      report["last_completed_api_frame"]=last_completed_api_frame.load();
      report["completed_native_frames"]=completed_frames.load();
      report["reference_retired"]=reference_retired;
      report["pending_gpu_work"]=renderer->HasPendingGpuWork();
      if(!gpu_batch_report_path.parent_path().empty())
        std::filesystem::create_directories(gpu_batch_report_path.parent_path());
      std::ofstream file(gpu_batch_report_path);file<<report.dump(2)<<'\n';file.close();
      if(!file)throw std::runtime_error("Cannot write native GPU batch report");
      REXLOG_INFO("Native GPU batch report saved to {}",gpu_batch_report_path.string());
    }catch(const std::exception& error) {
      REXLOG_WARN("Native GPU batch diagnostic export failed: {}",error.what());
    }
    if(renderer&&!gpu_physical_report_path.empty())try {
      // Cold, nonsealing observation after ordinary shutdown synchronization.
      auto report=renderer->GpuPhysicalTimingReport(true);
      report["last_completed_api_frame"]=last_completed_api_frame.load();
      report["pending_gpu_work"]=renderer->HasPendingGpuWork();
      if(!gpu_physical_report_path.parent_path().empty())
        std::filesystem::create_directories(gpu_physical_report_path.parent_path());
      std::ofstream file(gpu_physical_report_path);file<<report.dump(2)<<'\n';file.close();
      if(!file)throw std::runtime_error("Cannot write native physical GPU report");
    }catch(const std::exception& error) {
      REXLOG_WARN("Native physical GPU diagnostic export failed: {}",error.what());
    }
    if(renderer&&!gpu_draw_report_path.empty())try {
      // Query only already-retired diagnostic records. Reporting introduces
      // no queue submission, fence wait or resource lifetime change.
      auto report=renderer->GpuDrawTimingReport(true);
      report["last_completed_api_frame"]=last_completed_api_frame.load();
      report["completed_native_frames"]=completed_frames.load();
      report["reference_retired"]=reference_retired;
      report["pending_gpu_work"]=renderer->HasPendingGpuWork();
      if(!gpu_draw_report_path.parent_path().empty())
        std::filesystem::create_directories(gpu_draw_report_path.parent_path());
      std::ofstream file(gpu_draw_report_path);file<<report.dump(2)<<'\n';file.close();
      if(!file)throw std::runtime_error("Cannot write native GPU draw report");
      REXLOG_INFO("Native GPU draw report saved to {}",gpu_draw_report_path.string());
    }catch(const std::exception& error) {
      REXLOG_WARN("Native GPU draw diagnostic export failed: {}",error.what());
    }
    output.reset();
    renderer.reset();
    queue.reset();
    device.reset();
  }

  void Fail(uint32_t frame, const std::string& reason) {
    if (!failed.exchange(true))
      REXLOG_ERROR("Native renderer disabled at API frame {} after {} completed native frames (last API frame {}): {}; {}",
                   frame, completed_frames.load(), last_completed_api_frame.load(), reason,
                   "automatic fallback disabled; select Xenia and restart to use it");
    // The guest thread may fail while CP executes a native packet. Keep its
    // immutable ownership entries until CP finishes consuming committed draws;
    // clearing here would turn a completed native draw into a reference replay.
    pending_changed.notify_all();
  }

  [[noreturn]] void RejectUnsafeCoherence(const char* stage,uint32_t address,uint32_t bytes) {
    coherence_unsafe.store(true);
    throw std::runtime_error(std::string("Unsafe native CPU coherence (stage=")+stage+
        ", address="+std::to_string(address)+", bytes="+std::to_string(bytes)+
        ", page="+std::to_string(address/NativeMemoryCoherence::kPage)+
        ", observed_epoch="+std::to_string(LatestCpuWrite(address,bytes))+
        "); Native rendering cannot safely continue");
  }

  void Push(GraphicsBoundaryEvent&& event) {
    if(async_planner){PushAsync(std::move(event));return;}
    if (stopping || failed) return;
    const auto frame_number = event.frame;
    const auto captured_write_epoch = next_write_epoch.load();
    try {
      for(const auto& [address,bytes]:memory_coherence.TakeInvalidated()) {
        // Freed pages invalidate ownership without attempting to dereference
        // them. Readable CPU updates are copied only at an ordered API boundary,
        // never from the write-fault callback itself. Rearm before seeding a
        // readable page so a subsequent CPU write revokes that retained copy.
        for(uint32_t offset=0;offset<bytes;offset+=NativeMemoryCoherence::kPage) {
          const uint32_t first=address+offset,count=NativeMemoryCoherence::kPage;
          // Match the SDK fault/allocation lock across all alias arms and the
          // immutable copy. Never wait for rendering while holding this lock.
          // A notified but not yet retried store must refault after fresh arm;
          // holding only the epoch would not prove that the store completed.
          auto memory_lock=rex::thread::global_critical_region::AcquireDirect();
          if(memory->IsExternalHostWritePending(first,count))
            RejectUnsafeCoherence("overlapping_external_writer",first,count);
          // Label only writes observed before copying. A concurrent store must
          // remain newer than this snapshot when the GPU later imports it.
          auto coherence_epoch=write_epochs[first/NativeMemoryCoherence::kPage].load();
          std::vector<uint8_t> data;
          const auto access=memory->GetPhysicalHeap()->QueryRangeAccess(first,first+count-1);
          if(int(access)&int(rex::memory::PageAccess::kReadOnly)) {
            auto snapshot=memory_coherence.CaptureCpuSnapshot(first,count,
                [&](uint32_t address,uint32_t bytes) {
                  if(!memory->TryEnablePhysicalMemoryAccessCallbacks(address,bytes,true,false))
                    RejectUnsafeCoherence("snapshot_rearm_failed",address,bytes);
                },
                [&] {return write_epochs[first/NativeMemoryCoherence::kPage].load();},
                [&] {
                  const auto* source=memory->TranslatePhysical(first);
                  return std::vector<uint8_t>(source,source+count);
                });
            coherence_epoch=snapshot.first;
            data=std::move(snapshot.second);
          }
          const auto published_epoch=write_epochs[first/NativeMemoryCoherence::kPage].load();
          if(published_epoch!=coherence_epoch)
            throw std::runtime_error(NativeMemoryCoherence::Conflict(
                "publishing",first,count,coherence_epoch,published_epoch));
          planner.InvalidateMemory(first,count,std::move(data),coherence_epoch);
        }
      }
      const bool census_begin=(query_readiness||published_results)&&event.function==profile::d3d::kOcclusionQueryReport&&event.args[2]==1;
      const auto census_handle=event.args[1];
      uint32_t census_address=0;
      if(census_begin)for(const auto& block:event.blocks)if(block.kind==104&&block.data().size()==0x98) {
        const auto* p=block.data().data()+0x1c;
        const uint32_t report=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
        census_address=(report&0x1fffffffu)+(((report>>20)+0x200)&0x1000);
      }
      const uint32_t event_function=event.function;
      const bool event_query_begin=event.args[2]==1;
      const uint32_t event_query_handle=event.args[1];
      auto frame = planner.Push(std::move(event));
      // Only after the existing planner accepted the exact single-report ABI.
      if(census_begin) {
        if(query_readiness)query_readiness->Begin(census_handle,census_address);
        if(published_results)published_results->Invalidate(census_handle,census_address);
      }
      if(auto prefix=planner.TakePacketPrefix()) {
        std::lock_guard lock(pending_mutex);
        if(stopping||failed)return;
        packet_ownership.Enqueue(prefix->span);
        pending_packets.push_back({std::move(*prefix),captured_write_epoch});
      }
      if(auto copy=planner.TakeMemoryCopy();copy&&direct&&copy->cpu_source) {
        // The GPU-less guest treats this GPU memcpy as complete at once and may
        // reuse either range immediately. Perform it now, at its API point.
        auto* destination=memory->TranslatePhysical(copy->destination);
        const auto* source=memory->TranslatePhysical(copy->source);
        if(!destination||!source)throw std::runtime_error("Direct memcpy has unmapped physical memory");
        std::memmove(destination,source,copy->bytes);
        ++direct_api_memory_copies;
      }
      if(direct&&event_function==profile::d3d::kOcclusionQueryReport&&event_query_begin)Gow3DirectQueryBegun(event_query_handle);
      if (auto prefix = planner.TakeQueryPrefix(); prefix&&direct) {
        const auto handle=prefix->batch->operations.back().QueryHandle();
        Gow3DirectQueryEnded(handle,prefix->generation);
        std::weak_ptr<RuntimeState> weak=weak_from_this();
        processor->PostDirectWork([weak,prefix=std::move(*prefix),captured_write_epoch]() mutable {
          if(auto self=weak.lock())self->DirectQuery(std::move(prefix),captured_write_epoch);
        });
      } else if(direct) {
        if(auto batch=planner.TakeDirectPrefix(direct_batch_operations)) {
          std::weak_ptr<RuntimeState> weak=weak_from_this();
          processor->PostDirectWork([weak,batch=std::move(*batch),captured_write_epoch]() mutable {
            if(auto self=weak.lock())self->DirectQuery(std::move(batch),captured_write_epoch);
          });
        }
      } else if (prefix) {
        if(query_readiness||published_results) {
          const auto handle=prefix->batch->operations.back().QueryHandle();
          if(query_readiness)query_readiness->End(handle,{prefix->generation,prefix->batch->frame,prefix->end_address,false});
          if(published_results)published_results->End(handle,prefix->generation,prefix->batch->frame,prefix->end_address);
        }
        std::lock_guard lock(pending_mutex);
        if (stopping || failed) return;
        // Never wait here: the guest may still need to submit this query's
        // command buffer. Unique payload bytes are bounded by the planner's
        // per-frame budget; immutable vectors are shared by all its prefixes.
        if (pending_prefixes.size() >= 4096)
          throw std::runtime_error("Native query prefix queue exceeds its operation limit");
        pending_prefixes.push_back(std::move(*prefix));
      }
      for(const auto& [address,bytes]:memory_coherence.TakeWatches())
        if(!memory->TryEnablePhysicalMemoryAccessCallbacks(address,bytes,true,false))
          RejectUnsafeCoherence("gpu_watch_arm_failed",address,bytes);
      if (!frame) return;
      if(direct) {
        std::weak_ptr<RuntimeState> weak=weak_from_this();
        processor->PostDirectWork([weak,captured_write_epoch] {
          if(auto self=weak.lock())self->active_capture_epoch=captured_write_epoch;
        });
      }
      std::unique_lock lock(pending_mutex);
      // Preserve ordered history instead of dropping frames when rendering is
      // slower than the guest. One pending frame bounds snapshot retention.
      pending_changed.wait(lock, [&] { return stopping || failed || pending.empty(); });
      if (stopping || failed) return;
      pending.push_back(std::move(frame));
    } catch (const std::exception& error) {
      SaveFailure(event, error.what());
      Fail(frame_number, error.what());
      throw;  // The bridge disables its sink without recursively taking its lock.
    }
  }

  void QueuePlannerItem(PlannerItem&& item) {
    std::unique_lock lock(planner_mutex);
    if(planner_queue.size()>=kPlannerQueueLimit) {
      ++planner_producer_waits;
      planner_space.wait(lock,[&]{return stopping||failed||planner_queue.size()<kPlannerQueueLimit;});
    }
    if(stopping||failed)return;
    item.enqueued=std::chrono::steady_clock::now();
    planner_queue.push_back(std::move(item));++planner_items;
    planner_peak_queue=std::max(planner_peak_queue,planner_queue.size());
    if(planner_idle)planner_ready.notify_one();
  }
  void StopPlanner() {
    if(async_planner&&processor)processor->SetDirectSwapRouter({});
    {std::lock_guard lock(planner_mutex);}
    planner_ready.notify_all();planner_space.notify_all();
    if(planner_thread.joinable()&&planner_thread.get_id()!=std::this_thread::get_id())planner_thread.join();
  }
  void PlannerLoop() {
    rex::thread::set_current_thread_name("Native Planner");
    std::deque<PlannerItem> batch;
    while(true) {
      {
        std::unique_lock lock(planner_mutex);
        planner_idle=true;
        planner_ready.wait(lock,[&]{return stopping||failed||!planner_queue.empty();});
        planner_idle=false;
        if(stopping||failed)return;
        batch.swap(planner_queue);
      }
      planner_space.notify_all();
      for(auto& item:batch) {
        if(stopping||failed)return;
        PlanItem(item);
      }
      batch.clear();
    }
  }
  void PlanItem(PlannerItem& item) {
    const auto frame_number=item.event.frame;
    if(item.swap){processor->PostDirectWork(std::move(item.swap));return;}
    try {
      if(item.invalidation) {
        planner.InvalidateMemory(item.address,item.bytes,std::move(item.data),item.coherence_epoch);
        return;
      }
      const auto captured_write_epoch=item.captured_write_epoch;
      const auto plan_began=std::chrono::steady_clock::now();
      const auto ns=[](auto d){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());};
      if(item.query_end&&item.enqueued!=std::chrono::steady_clock::time_point{}) {
        ++planner_query_ends;planner_queue_ns+=ns(plan_began-item.enqueued);
      }
      auto frame=planner.Push(std::move(item.event));
      if(planner.TakePacketPrefix())throw std::runtime_error("Async planner received a packet prefix");
      const auto copy=planner.TakeMemoryCopy();
      if(bool(copy)!=bool(item.memory_copy)||(copy&&(copy->source!=item.memory_copy->source||
         copy->destination!=item.memory_copy->destination||copy->bytes!=item.memory_copy->bytes||
         copy->cpu_source!=item.memory_copy->cpu_source)))
        throw std::runtime_error("Async planner memory-export copy diverged from its API-point replica");
      if(auto prefix=planner.TakeQueryPrefix()) {
        if(!item.query_end||prefix->generation!=item.query_generation)
          throw std::runtime_error("Async planner query generation diverged from its API-point replica");
        std::weak_ptr<RuntimeState> weak=weak_from_this();
        const auto posted=std::chrono::steady_clock::now();planner_plan_ns+=ns(posted-plan_began);
        processor->PostDirectWork([weak,prefix=std::move(*prefix),captured_write_epoch,posted]() mutable {
          if(auto self=weak.lock()) {
            self->planner_cp_ns+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-posted).count());
            self->DirectQuery(std::move(prefix),captured_write_epoch);
          }
        });
      } else {
        if(item.query_end)throw std::runtime_error("Async planner query end produced no prefix");
        if(auto batch=planner.TakeDirectPrefix(direct_batch_operations)) {
          std::weak_ptr<RuntimeState> weak=weak_from_this();
          processor->PostDirectWork([weak,batch=std::move(*batch),captured_write_epoch]() mutable {
            if(auto self=weak.lock())self->DirectQuery(std::move(batch),captured_write_epoch);
          });
        }
      }
      if(!frame)return;
      {
        std::weak_ptr<RuntimeState> weak=weak_from_this();
        processor->PostDirectWork([weak,captured_write_epoch] {
          if(auto self=weak.lock())self->active_capture_epoch=captured_write_epoch;
        });
      }
      std::unique_lock lock(pending_mutex);
      if(!pending.empty()) {
        const auto waited=std::chrono::steady_clock::now();++planner_pending_waits;
        pending_changed.wait(lock,[&]{return stopping||failed||pending.empty();});
        planner_pending_wait_ns+=ns(std::chrono::steady_clock::now()-waited);
      }
      if(stopping||failed)return;
      pending.push_back(std::move(frame));
    } catch(const std::exception& error) {
      SaveFailure(item.event,error.what());
      Fail(frame_number,error.what());
      // No later result will be published: let GetData fall back rather than wait.
      Gow3EnableDirectQueries(false);
      planner_space.notify_all();
    }
  }
  void PushAsync(GraphicsBoundaryEvent&& event) {
    if(stopping)return;
    if(failed) {
      // Report a failure once, so the bridge disables its sink.
      if(std::exchange(planner_failure_reported,true))return;
      throw std::runtime_error("Native renderer failed; async planner stopped");
    }
    if(!ownership_replica)
      ownership_replica=std::make_unique<GearsLiveOwnership>([this](uint32_t address,uint32_t bytes){memory_coherence.Watch(address,bytes);});
    if(!planner_thread.joinable()) {
      planner_thread=std::thread([this]{PlannerLoop();});
      processor->SetDirectSwapRouter([this](std::function<void()>& swap) {
        std::unique_lock lock(planner_mutex);
        if(stopping||failed)return false;
        PlannerItem item;item.swap=std::move(swap);
        planner_queue.push_back(std::move(item));++planner_items;
        if(planner_idle)planner_ready.notify_one();
        return true;
      });
    }
    const auto frame_number=event.frame;
    const auto captured_write_epoch=next_write_epoch.load();
    try {
      for(const auto& [address,bytes]:memory_coherence.TakeInvalidated()) {
        for(uint32_t offset=0;offset<bytes;offset+=NativeMemoryCoherence::kPage) {
          const uint32_t first=address+offset,count=NativeMemoryCoherence::kPage;
          // Identical to the synchronous path: copy under the SDK lock at the API point.
          auto memory_lock=rex::thread::global_critical_region::AcquireDirect();
          if(memory->IsExternalHostWritePending(first,count))
            RejectUnsafeCoherence("overlapping_external_writer",first,count);
          auto coherence_epoch=write_epochs[first/NativeMemoryCoherence::kPage].load();
          std::vector<uint8_t> data;
          const auto access=memory->GetPhysicalHeap()->QueryRangeAccess(first,first+count-1);
          if(int(access)&int(rex::memory::PageAccess::kReadOnly)) {
            auto snapshot=memory_coherence.CaptureCpuSnapshot(first,count,
                [&](uint32_t address,uint32_t bytes) {
                  if(!memory->TryEnablePhysicalMemoryAccessCallbacks(address,bytes,true,false))
                    RejectUnsafeCoherence("snapshot_rearm_failed",address,bytes);
                },
                [&] {return write_epochs[first/NativeMemoryCoherence::kPage].load();},
                [&] {
                  const auto* source=memory->TranslatePhysical(first);
                  return std::vector<uint8_t>(source,source+count);
                });
            coherence_epoch=snapshot.first;
            data=std::move(snapshot.second);
          }
          const auto published_epoch=write_epochs[first/NativeMemoryCoherence::kPage].load();
          if(published_epoch!=coherence_epoch)
            throw std::runtime_error(NativeMemoryCoherence::Conflict(
                "publishing",first,count,coherence_epoch,published_epoch));
          ownership_replica->InvalidateMemory(first,count);
          PlannerItem item;item.invalidation=true;item.address=first;item.bytes=count;
          item.data=std::move(data);item.coherence_epoch=coherence_epoch;
          QueuePlannerItem(std::move(item));
        }
      }
      const auto decision=ownership_replica->Observe(event);
      if(decision.texture_count) {
        const auto blocks=std::span<GraphicsBoundaryBlock* const>(decision.textures).first(decision.texture_count);
        if(event.materialize_textures)event.materialize_textures(blocks);
        else {
          if(!event.materialize_texture)throw std::runtime_error("Missing deferred texture snapshot capability");
          for(auto* block:blocks)event.materialize_texture(*block);
        }
      }
      if(const auto& copy=decision.memory_copy;copy&&copy->cpu_source) {
        // The GPU-less guest treats this GPU memcpy as complete at once and may
        // reuse either range immediately. Perform it now, at its API point.
        auto* destination=memory->TranslatePhysical(copy->destination);
        const auto* source=memory->TranslatePhysical(copy->source);
        if(!destination||!source)throw std::runtime_error("Direct memcpy has unmapped physical memory");
        std::memmove(destination,source,copy->bytes);
        ++direct_api_memory_copies;
      }
      if(decision.query_begin&&(query_readiness||published_results)) {
        // Same report address the synchronous path derives after Push.
        uint32_t census_address=0;
        for(const auto& block:event.blocks)if(block.kind==104&&block.data().size()==0x98) {
          const auto* p=block.data().data()+0x1c;
          const uint32_t report=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
          census_address=(report&0x1fffffffu)+(((report>>20)+0x200)&0x1000);
        }
        if(query_readiness)query_readiness->Begin(decision.query_handle,census_address);
        if(published_results)published_results->Invalidate(decision.query_handle,census_address);
      }
      if(decision.query_begin)Gow3DirectQueryBegun(decision.query_handle);
      if(decision.query_end)Gow3DirectQueryEnded(decision.query_handle,decision.query_generation);
      for(const auto& [address,bytes]:memory_coherence.TakeWatches())
        if(!memory->TryEnablePhysicalMemoryAccessCallbacks(address,bytes,true,false))
          RejectUnsafeCoherence("gpu_watch_arm_failed",address,bytes);
      PlannerItem item;item.event=std::move(event);item.captured_write_epoch=captured_write_epoch;
      item.query_end=decision.query_end;item.query_generation=decision.query_generation;
      item.memory_copy=decision.memory_copy;
      QueuePlannerItem(std::move(item));
    } catch(const std::exception& error) {
      SaveFailure(event,error.what());
      Fail(frame_number,error.what());
      planner_failure_reported=true;
      throw;
    }
  }

  void Initialize() {
    if (renderer) return;
    // Commit together only after construction succeeds. A skipped startup
    // warmup must not leave an old queue pointing at a replaced device wrapper.
    auto initializing_device = std::make_unique<LiveVulkanDevice>(description);
    auto initializing_queue = initializing_device->device().createCommandQueue(plume::RenderCommandListType::DIRECT);
    SceneExecutorConfig config;
    if(pipeline_recipes) {
      std::weak_ptr<NativePipelineRecipeCache> weak=pipeline_recipes;
      config.observe_pipeline=[weak](ObservedNativePipeline observation) {
        if(auto cache=weak.lock())cache->Observe(std::move(observation));
      };
    }
    if(fsi_state_recipes) {
      std::weak_ptr<NativeFsiStateRecipeCache> weak=fsi_state_recipes;
      config.observe_fsi_state=[weak](const NativeFsiStateRecipe& recipe)->std::atomic<uint64_t>* {
        if(auto cache=weak.lock())return cache->Bind(recipe);return nullptr;
      };
    }
    config.payload = [this](const std::string& hash) {
      if (!active_frame) throw std::runtime_error("Native payload requested outside its frame");
      const auto* found=active_frame->FindResource(hash);
      if(!found)throw std::runtime_error("Missing live payload " + hash);
      return **found;
    };
    config.payload_snapshot = [this](const std::string& hash) {
      if (!active_frame) throw std::runtime_error("Native payload requested outside its frame");
      const auto* found=active_frame->FindResource(hash);
      if(!found)throw std::runtime_error("Missing live payload " + hash);
      return *found;
    };
    const auto compile_shader = [this](const std::string& hash, NativeShaderMode mode) {
      if (!active_frame) throw std::runtime_error("Native shader requested outside its frame");
      const auto* found=active_frame->FindResource(hash,true);
      if(!found)throw std::runtime_error("Missing live shader " + hash);
      const auto shader=CompileGameplayShader(**found,mode==NativeShaderMode::FragmentInterlock,hash);
      if (shader->source_sha256 != hash) throw std::runtime_error("Live shader identity mismatch");
      std::vector<uint8_t> bytes(shader->spirv.size() * sizeof(uint32_t));
      std::memcpy(bytes.data(), shader->spirv.data(), bytes.size());
      return bytes;
    };
    config.shader = [compile_shader](const std::string& hash) {
      return compile_shader(hash, NativeShaderMode::Raster);
    };
    config.fsi_shader = [compile_shader](const std::string& hash) {
      return compile_shader(hash, NativeShaderMode::FragmentInterlock);
    };
    if(async_shaders)config.shader_ready=[this](const CanonicalResourceRef& resource,bool fsi) {
      return AsyncShaderReady(resource,fsi);
    };
    config.shader_snapshot=[this](const CanonicalResourceRef& resource,bool fsi) {
      if(!active_frame)throw std::runtime_error("Native shader requested outside its frame");
      if(!resource.owner)throw std::runtime_error("Missing immutable live shader");
      const auto shader=CompileGameplayShader(*resource.owner,fsi,resource.hash.View());
      if(shader->source_sha256!=resource.hash.View())throw std::runtime_error("Live shader identity mismatch");
      std::vector<uint8_t> bytes(shader->spirv.size()*sizeof(uint32_t));
      std::memcpy(bytes.data(),shader->spirv.data(),bytes.size());return bytes;
    };
    config.fsi_depth_shader = [this] {
      const auto shader = compiler.GetDepthOnlyFragmentInterlock();
      std::vector<uint8_t> bytes(shader->spirv.size() * sizeof(uint32_t));
      std::memcpy(bytes.data(), shader->spirv.data(), bytes.size());
      return bytes;
    };
    config.max_draw_batch_size = uint32_t(std::clamp(REXCVAR_GET(gow3_native_draw_batch_size),1,64));
    config.defer_native_queries=deferred_queries;
    config.retire_completed_query_prefixes=retire_query_prefixes;
    config.max_deferred_queries=query_capacity;
    config.batch_transfers=native_raster&&REXCVAR_GET(gow3_native_batch_transfers);
    config.unified_gpu_queue=unified_gpu_queue;
    config.queue_cpu_memory_writes=queue_cpu_memory_writes;
    config.diagnose_marker_query_cohorts=REXCVAR_GET(gpu_command_summary);
    config.reuse_cpu_write_upload=native_raster&&REXCVAR_GET(gow3_native_reuse_cpu_write_upload);
    config.forward_cpu_upload_receipts=native_raster&&REXCVAR_GET(gow3_native_forward_cpu_upload_receipts);
    config.async_gpu_submissions=unified_gpu_queue&&REXCVAR_GET(gow3_native_async_gpu_submissions);
    config.async_submission_capacity=uint32_t(REXCVAR_GET(gow3_native_submission_capacity));
    config.max_inflight_batches=REXCVAR_GET(gow3_native_four_inflight_batches)?4u:2u;
    // Default on, so only where its asynchronous unified queue exists.
    config.submission_worker=config.async_gpu_submissions&&REXCVAR_GET(gow3_native_submission_worker);
    config.gpu_batch_attribution=unified_gpu_queue&&!gpu_batch_report_path.empty();
    config.gpu_draw_timestamps=unified_gpu_queue&&!gpu_draw_report_path.empty();
    config.gpu_physical_timestamps=unified_gpu_queue&&!gpu_physical_report_path.empty();
    config.gpu_physical_first=REXCVAR_GET(gow3_native_gpu_physical_first);
    config.gpu_physical_end=REXCVAR_GET(gow3_native_gpu_physical_end);
    config.gpu_physical_interval=REXCVAR_GET(gow3_native_gpu_physical_interval);
    config.restore_depth_stencil_export=REXCVAR_GET(gow3_native_stencil_export_restores);
    config.shader_stencil_export_enabled=description.shader_stencil_export;
    config.gpu_timestamps=unified_gpu_queue&&(REXCVAR_GET(gow3_native_gpu_timestamps)||config.gpu_batch_attribution);
    config.detailed_reports=REXCVAR_GET(gow3_native_detailed_reports);
    config.diagnose_full_cpu_texture_overlays=REXCVAR_GET(gow3_native_diagnose_full_cpu_texture_overlays);
    config.share_cpu_texture_sampling_variants=native_raster&&REXCVAR_GET(gow3_native_share_cpu_texture_sampling_variants);
    config.raw_gpu_float_cpu_textures=REXCVAR_GET(gow3_native_raw_gpu_float_cpu_textures);
    config.defer_cpu_texture_drains=REXCVAR_GET(gow3_native_defer_cpu_texture_drains);
    config.cpu_image_slot_lookup=REXCVAR_GET(gow3_native_cpu_image_slot_lookup);
    config.max_dormant_texture_entries=REXCVAR_GET(gow3_native_dormant_texture_entries);
    config.max_dormant_texture_bytes=uint64_t(REXCVAR_GET(gow3_native_dormant_texture_mib))*1024*1024;
    config.max_dormant_texture_epochs=REXCVAR_GET(gow3_native_dormant_texture_frames);
    if(!REXCVAR_GET(gow3_native_retain_unused_textures))
      config.max_dormant_texture_bytes=config.max_dormant_texture_entries=0;
    if(!REXCVAR_GET(gow3_native_retain_unused_vertices))
      config.max_dormant_vertex_bytes=config.max_dormant_vertex_entries=0;
    config.texture_probe_directory=REXCVAR_GET(gow3_native_texture_probe_directory);
    config.texture_probe_hashes=REXCVAR_GET(gow3_native_texture_probe_hashes);
    config.native_fsi = description.ordered_pixel_interlock && description.features.fragmentStoresAndAtomics &&
        !REXCVAR_GET(gow3_native_hardware_render_targets);
    config.retain_fsi_raster_extent=REXCVAR_GET(gow3_native_retain_fsi_raster_extent);
    config.discard_dummy_fsi_attachment=REXCVAR_GET(gow3_native_discard_dummy_fsi_attachment);
    config.attachmentless_fsi=REXCVAR_GET(gow3_native_attachmentless_fsi);
    config.cache_texture_layouts=REXCVAR_GET(gow3_native_cache_texture_layouts);
    config.associative_texture_layouts=REXCVAR_GET(gow3_native_associative_texture_layouts);
    config.block_page_versions=REXCVAR_GET(gow3_native_block_page_versions);
    config.range_texture_versions=REXCVAR_GET(gow3_native_range_texture_versions);
    config.compact_cpu_texture_version_pages=REXCVAR_GET(gow3_native_compact_cpu_texture_version_pages);
    config.coalesced_texture_ranges=REXCVAR_GET(gow3_native_coalesced_texture_ranges);
    config.reuse_defined_ranges=REXCVAR_GET(gow3_native_reuse_defined_ranges);
    config.cache_gpu_alias_states=REXCVAR_GET(gow3_native_cache_gpu_alias_states);
    config.queued_volume_alias_loads=REXCVAR_GET(gow3_native_queued_volume_alias_loads);
    config.batch_draw_descriptors=REXCVAR_GET(gow3_native_batch_draw_descriptors);
    config.skip_unused_descriptor_sets=REXCVAR_GET(gow3_native_skip_unused_descriptor_sets);
    config.shader_lookup_lookaside=REXCVAR_GET(gow3_native_shader_lookup_lookaside);
    config.pipeline_lookup_lookaside=REXCVAR_GET(gow3_native_pipeline_lookup_lookaside);
    config.cpu_image_lookup_lookaside=REXCVAR_GET(gow3_native_cpu_image_lookup_lookaside);
    config.msaa_single_sample_fallback=REXCVAR_GET(gow3_native_msaa_single_sample_fallback);
    config.cache_draw_descriptors=REXCVAR_GET(gow3_native_cache_draw_descriptors);
    config.delta_constant_uploads=REXCVAR_GET(gow3_native_delta_constant_uploads);
    config.pinned_null_constant_uploads=REXCVAR_GET(gow3_native_pinned_null_constant_uploads);
    config.reset_draw_command_pool=REXCVAR_GET(gow3_native_reset_draw_command_pool);
    config.time_draw_recording=REXCVAR_GET(gow3_native_time_draw_recording);
    config.time_cpu_stages=time_cpu_stages;
    config.multi_draw_command_buffers=REXCVAR_GET(gow3_native_multi_draw_command_buffers);
    config.packed_color_render_pass_groups=REXCVAR_GET(gow3_native_packed_color_render_pass_groups);
    config.hardware_render_pass_continuation=REXCVAR_GET(gow3_native_hardware_render_pass_continuation);
    config.packed_color_blend_pass_groups=REXCVAR_GET(gow3_native_packed_color_blend_pass_groups);
    config.packed_color_action_pass_groups=REXCVAR_GET(gow3_native_packed_color_action_pass_groups);
    config.packed_depth_render_pass_groups=REXCVAR_GET(gow3_native_packed_depth_render_pass_groups);
    config.fsi_draw_pass_groups=REXCVAR_GET(gow3_native_fsi_draw_pass_groups);
    config.fsi_draw_pass_queries=REXCVAR_GET(gow3_native_fsi_draw_pass_queries);
    config.async_pipelines=REXCVAR_GET(gow3_native_async_pipelines);
    config.async_pipeline_budget=std::chrono::milliseconds(std::max(0,REXCVAR_GET(gow3_native_async_shader_budget_ms)));
    config.packed_zero_depth_clear=REXCVAR_GET(gow3_native_packed_zero_depth_clear);
    config.packed_primary_stencil_clear=native_raster&&REXCVAR_GET(gow3_native_packed_primary_stencil_clear);
    config.packed_secondary_stencil_clear=native_raster&&REXCVAR_GET(gow3_native_packed_secondary_stencil_clear);
    config.atlas_depth_region_restores=native_raster&&REXCVAR_GET(gow3_native_atlas_depth_region_restores);
    config.fused_atlas_depth_clear=native_raster&&REXCVAR_GET(gow3_native_fused_atlas_depth_clear);
    config.direct_atlas_depth_resolves=native_raster&&REXCVAR_GET(gow3_native_direct_atlas_depth_resolves);
    config.fused_packed_atlas_resolves=native_raster&&REXCVAR_GET(gow3_native_fused_packed_atlas_resolves);
    config.canonical_resolve_stores=native_raster&&REXCVAR_GET(gow3_native_canonical_resolve_stores);
    config.direct_packed_color_resolves=native_raster&&REXCVAR_GET(gow3_native_direct_packed_color_resolves);
    config.skip_full_color_clear_restore=REXCVAR_GET(gow3_native_skip_full_color_clear_restore);
    config.recycle_resolve_images=REXCVAR_GET(gow3_native_recycle_resolve_images);
    config.recycle_resolve_descriptors=REXCVAR_GET(gow3_native_recycle_resolve_descriptors);
    config.recycle_pass_transfer_descriptors=REXCVAR_GET(gow3_native_recycle_pass_transfer_descriptors);
    config.recycle_pass_transfer_render_passes=REXCVAR_GET(gow3_native_recycle_pass_transfer_render_passes);
    config.reuse_resolve_identity_views=REXCVAR_GET(gow3_native_reuse_resolve_identity_views);
    config.diagnose_texture_eviction_reloads=REXCVAR_GET(gow3_native_diagnose_texture_eviction_reloads);
    config.recycle_vertex_buffers=REXCVAR_GET(gow3_native_recycle_vertex_buffers);
    config.compact_resolve_channels=REXCVAR_GET(gow3_native_compact_resolve_channels);
    config.storage_image_extended_formats_enabled=description.features.shaderStorageImageExtendedFormats;
    config.implicit_fsi_barriers=REXCVAR_GET(gow3_native_implicit_fsi_barriers);
    config.fsi_hoist_mode=fsi_hoist_mode;
    if(fsi_hoist_compiler)config.fsi_hoist_version=NativeFsiHoistCompiler::CacheVersion(fsi_hoist_mode);
    config.exact_fsi_stores=exact_fsi_stores_enabled;
    if(exact_fsi_compiler)config.exact_fsi_stores_version=NativeFsiExactStoresCompiler::CacheVersion();
    config.builtin_readonly_depth_queries=REXCVAR_GET(gow3_native_builtin_readonly_depth_queries);
    config.native_readonly_depth_attachment=readonly_depth_attachment_enabled;
    config.native_readonly_depth_read_only_pass=readonly_depth_attachment_enabled&&
        REXCVAR_GET(gow3_native_readonly_depth_read_only_pass);
    config.native_readonly_depth_lower_bound_query=readonly_depth_attachment_enabled&&
        REXCVAR_GET(gow3_native_readonly_depth_lower_bound_query);
    config.native_readonly_depth_no_write_metadata=readonly_depth_attachment_enabled&&
        REXCVAR_GET(gow3_native_readonly_depth_no_write_metadata);
    config.native_readonly_depth_conservative=readonly_depth_attachment_enabled&&
        REXCVAR_GET(gow3_native_readonly_depth_conservative);
    config.native_early_depth=early_depth_enabled;
    config.host_query_reset=REXCVAR_GET(gow3_native_host_query_reset);
    config.readonly_query_render_pass_groups=REXCVAR_GET(gow3_native_readonly_query_render_pass_groups);
    config.native_early_depth_catalog=early_depth_catalog_enabled;
    config.reuse_early_depth_readonly_boundaries=reuse_early_depth_readonly_boundaries_enabled;
    config.native_early_depth_material_retention=REXCVAR_GET(gow3_native_early_depth_material_retention);
    config.retained_fp24_depth_writer=retained_fp24_depth_writer_enabled;
    config.retained_opaque_writer=REXCVAR_GET(gow3_native_retained_opaque_writer);
    config.retained_opaque_program=retained_opaque_program;
    config.retained_opaque_catalog=REXCVAR_GET(gow3_native_retained_opaque_catalog);
    config.retained_opaque_phase=REXCVAR_GET(gow3_native_retained_opaque_phase);
    config.retained_opaque_phase_render_pass_groups=REXCVAR_GET(gow3_native_retained_opaque_phase_render_pass_groups);
    config.share_retained_opaque_phase_scratch=REXCVAR_GET(gow3_native_share_retained_opaque_phase_scratch);
    config.poll_retained_opaque_phase_pressure=REXCVAR_GET(gow3_native_poll_retained_opaque_phase_pressure);
    config.share_retained_opaque_phase_attachments=REXCVAR_GET(gow3_native_share_retained_opaque_phase_attachments);
    config.retained_shared_depth_queries=retained_shared_depth_queries_enabled;
    config.retained_d32_composition=retained_d32_composition_enabled;
    config.retain_primary_across_disjoint_queries=retain_primary_across_disjoint_queries_enabled;
    config.queue_retained_exports_for_draws=REXCVAR_GET(gow3_native_queue_retained_exports_for_draws);
    config.reuse_retained_opaque_resources=REXCVAR_GET(gow3_native_reuse_retained_opaque_resources);
    config.retained_opaque_catalog_programs=retained_opaque_catalog_programs;
    config.retained_opaque_phase_programs=retained_opaque_phase_programs;
    // Preserve the frozen effective D32 capability/conflict policy.
    config.native_readonly_depth_skip_edram_barrier=readonly_depth_attachment_enabled&&
        REXCVAR_GET(gow3_native_readonly_depth_skip_edram_barrier);
    config.builtin_atomic_depth=REXCVAR_GET(gow3_native_builtin_atomic_depth);
    config.opaque_color_overwrite=REXCVAR_GET(gow3_native_opaque_color_overwrite);
    config.opaque_color_overwrite_program=opaque_overwrite_program;
    config.native_stencil_material_cutoff=REXCVAR_GET(gow3_native_stencil_material_cutoff);
    config.native_stencil_material_program=stencil_material_program;
    config.forward_control_flow=REXCVAR_GET(gow3_native_forward_control_flow);
    config.forward_control_flow_program=forward_control_flow_program;
    config.forward_vertex_control_flow=REXCVAR_GET(gow3_native_forward_vertex_control_flow);
    config.forward_vertex_program=forward_vertex_program;
    config.forward_pixel_catalog=REXCVAR_GET(gow3_native_forward_pixel_catalog);
    config.forward_pixel_catalog_programs=forward_pixel_catalog;
    config.fsi_state_forward_cfg=REXCVAR_GET(gow3_native_fsi_state_forward_cfg);
    config.fsi_state_material_composition=fsi_state_material_composition_enabled;
    config.fsi_state_inactive_early_catalog=fsi_state_inactive_early_catalog_enabled;
    config.fsi_state_cache_headroom=REXCVAR_GET(gow3_native_fsi_state_cache_headroom);
    if(config.fsi_state_cache_headroom) {
      const auto limits=NativeFsiMaterialShaderCacheMaximum(true);
      config.max_shader_cache_entries=limits.entries;
      config.max_shader_cache_bytes=limits.bytes;
      REXLOG_INFO("Native FSI shader cache headroom: policy={}, modules={}, spirv_bytes={}",
          kNativeFsiMaterialShaderCacheHeadroomPolicy,limits.entries,limits.bytes);
    }
    config.specialize_fsi_pipeline_state=REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)||config.fsi_state_forward_cfg||config.fsi_state_material_composition;
    config.fsi_state_programs=fsi_state_programs;
    config.coalesce_draw_barriers=REXCVAR_GET(gow3_native_coalesce_draw_barriers);
    config.skip_readonly_sample_barriers=REXCVAR_GET(gow3_native_skip_readonly_sample_barriers);
    config.precise_fsi_barriers=REXCVAR_GET(gow3_native_precise_fsi_barriers);

    config.ordered_pixel_interlock_enabled = description.ordered_pixel_interlock;
    // A registry lookup by name per draw is measurable; settings changes still
    // apply within 100 ms. Only the CP thread calls this.
    config.anisotropy_override=[value=int32_t(0),checked=std::chrono::steady_clock::time_point{}]() mutable {
      const auto now=std::chrono::steady_clock::now();
      if(now-checked>=std::chrono::milliseconds(100)) {
        value=REXCVAR_QUERY(int32_t,anisotropic_override);checked=now;
      }
      return value;
    };
    config.max_sampler_anisotropy=description.features.samplerAnisotropy?
        uint32_t(processor->GetVulkanDevice()->properties().maxSamplerAnisotropy):0;
    if(config.implicit_fsi_barriers)
      REXLOG_INFO("Native FSI shader variant: implicit coherent interlock v1");
    REXLOG_INFO("Native raster output: {}", config.native_fsi ? "ordered pixel interlock" : "fixed-function diagnostic path");
    config.present = [this](uint32_t frame, std::shared_ptr<plume::RenderTexture> texture,
                            std::array<uint32_t, 6> fetch) {
      output_frame = frame;
      output = std::move(texture);
      output_fetch = fetch;
    };
    auto initializing_renderer=std::make_unique<SceneExecutor>(std::move(config),&initializing_device->device(),initializing_queue.get());
    device=std::move(initializing_device);queue=std::move(initializing_queue);renderer=std::move(initializing_renderer);
  }

  void PrepareObservedPipelines(rex::graphics::ShaderStorageProgress &progress) {
    using Phase = rex::graphics::ShaderStorageProgress::Phase;
    using Outcome = rex::graphics::ShaderStorageProgress::Outcome;
    if (stopping || !pipeline_recipes)
      return;
    progress.Begin(Phase::kReading);
    auto recipes = pipeline_recipes->Load();
    if (recipes.empty() || stopping) {
      progress.Finish();
      return;
    }
    auto state_records=fsi_state_recipes?fsi_state_recipes->Load(recipes):std::vector<RankedNativeFsiStateRecipe>{};
    if(REXCVAR_GET(gow3_native_fsi_state_forward_cfg))
      std::erase_if(state_records,[](const auto& record){return !FsiStateForwardCfgAllowed(record.recipe);});
    if(fsi_state_material_composition_enabled) {
      const size_t observed=state_records.size();
      // Filter before the ranked PSO bound and module-source collection. Null
      // and active early-depth sources cannot consume this optional cache budget.
      std::erase_if(state_records,[this](const auto& record) {
        const auto& generic=record.recipe.generic;
        return !FsiStateMaterialCompositionSourceAllowed(generic.vertex,
            generic.pixel?std::string_view(*generic.pixel):std::string_view{},
            fsi_state_inactive_early_catalog_enabled,early_depth_catalog_enabled);
      });
      REXLOG_INFO("FSI material state source filtering: observed={}, eligible={}, excluded={}",
          observed,state_records.size(),observed-state_records.size());
    }
    if(state_records.size()>kFsiStateMaxPipelines)state_records.resize(kFsiStateMaxPipelines);
    // Preserve first-seen ranked tuple order when admitting unique modules.
    // The builtin competes for the same byte budget at its actual hot rank.
    struct StateSource {std::optional<std::string> pixel;PipelineShaderSource bytes;};
    std::vector<StateSource> state_sources;
    std::set<std::string> state_source_digests;
    bool state_depth_only=false;
    for(const auto& record:state_records) {
      if(!record.recipe.generic.pixel) {
        if(!state_depth_only)state_sources.push_back({std::nullopt,{}});
        state_depth_only=true;continue;
      }
      for(const auto& source:recipes)if(source.recipe==record.recipe.generic) {
        if(state_source_digests.insert(*source.recipe.pixel).second)
          state_sources.push_back({source.recipe.pixel,source.pixel});
        break;
      }
    }
    // Source preparation reuses the exact compiler and bounded speculative
    // worker implementation. No future is reserved until a worker runs Get.
    std::map<std::string, NativeShaderPrewarmQueue::Job> sources;
    for (const auto &item : recipes) {
      sources.try_emplace(item.recipe.vertex + ".raster",
                          NativeShaderPrewarmQueue::Job{item.recipe.vertex + ".raster", item.vertex, false});
      if (item.pixel)
        sources.try_emplace(*item.recipe.pixel + ".fsi",
                            NativeShaderPrewarmQueue::Job{*item.recipe.pixel + ".fsi", item.pixel, true});
    }
    const bool prepare_retained=REXCVAR_GET(gow3_native_retained_opaque_writer)&&!renderer;
    const bool prepare_retained_catalog=REXCVAR_GET(gow3_native_retained_opaque_catalog)&&!renderer;
    const bool prepare_retained_phase=REXCVAR_GET(gow3_native_retained_opaque_phase)&&!renderer;
    std::map<std::string,PipelineShaderSource> retained_phase_sources;
    if(prepare_retained_phase)for(const auto& item:recipes)if(item.pixel&&item.recipe.pixel&&
        NativeRetainedOpaquePhaseRecipe(item.recipe.vertex,*item.recipe.pixel,item.recipe.raster)&&
        item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1)
        retained_phase_sources.try_emplace(*item.recipe.pixel,item.pixel);
    std::map<std::string,PipelineShaderSource> retained_catalog_sources;
    if(prepare_retained_catalog)for(const auto& item:recipes)if(item.pixel&&item.recipe.pixel&&
        RetainedOpaqueCatalogPair(item.recipe.vertex,*item.recipe.pixel)&&RetainedOpaqueCatalogRaster(item.recipe.raster)&&
        item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1)
        retained_catalog_sources.try_emplace(*item.recipe.pixel,item.pixel);
    const bool prepare_stencil_material=REXCVAR_GET(gow3_native_stencil_material_cutoff)&&!renderer;
    const bool prepare_opaque=REXCVAR_GET(gow3_native_opaque_color_overwrite)&&
        !REXCVAR_GET(gow3_native_implicit_fsi_barriers)&&!renderer;
    const bool prepare_forward=REXCVAR_GET(gow3_native_forward_control_flow)&&
        !REXCVAR_GET(gow3_native_implicit_fsi_barriers)&&
        !REXCVAR_GET(gow3_native_opaque_color_overwrite)&&!renderer;
    const bool prepare_forward_vertex=REXCVAR_GET(gow3_native_forward_vertex_control_flow)&&
        !REXCVAR_GET(gow3_native_implicit_fsi_barriers)&&!REXCVAR_GET(gow3_native_opaque_color_overwrite)&&
        !REXCVAR_GET(gow3_native_forward_control_flow)&&!REXCVAR_GET(gow3_native_builtin_atomic_depth)&&!renderer;
    const bool prepare_catalog=REXCVAR_GET(gow3_native_forward_pixel_catalog)&&
        !REXCVAR_GET(gow3_native_implicit_fsi_barriers)&&!REXCVAR_GET(gow3_native_opaque_color_overwrite)&&
        !REXCVAR_GET(gow3_native_forward_control_flow)&&!REXCVAR_GET(gow3_native_forward_vertex_control_flow)&&
        !REXCVAR_GET(gow3_native_builtin_atomic_depth)&&!renderer;
    std::map<std::string,PipelineShaderSource> catalog_sources;
    if(prepare_catalog)for(const auto& item:recipes)if(item.pixel&&item.recipe.pixel&&ForwardPixelCatalogAllowed(*item.recipe.pixel,item.recipe.raster)&&
        item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1&&catalog_sources.size()<kForwardPixelCatalogMaxPrograms)
        catalog_sources.try_emplace(*item.recipe.pixel,item.pixel);
    progress.Begin(Phase::kShaders, sources.size()+size_t(prepare_retained)+retained_catalog_sources.size()+size_t(prepare_stencil_material)+size_t(prepare_opaque)+size_t(prepare_forward)+size_t(prepare_forward_vertex)+catalog_sources.size()+state_sources.size()+retained_phase_sources.size());
    // This temporary startup queue is joined before CP preparation and guest
    // launch. The live discovery queue has no guest producers yet.
    NativeShaderPrewarmQueue prepare(
        [this, &progress](const NativeShaderPrewarmQueue::Job &job) {
          if (stopping) {
            progress.Advance(false);
            return;
          }
          try {
            if(job.fragment_interlock&&fsi_hoist_compiler)fsi_hoist_compiler->Get(*job.source);
        else if(job.fragment_interlock&&exact_fsi_compiler)exact_fsi_compiler->Get(*job.source);
            else compiler.Get(*job.source, job.fragment_interlock ? NativeShaderMode::FragmentInterlock
                                                                : NativeShaderMode::Raster);
            progress.Advance();
          } catch (...) {
            progress.Advance(false);
            throw;
          }
        },
        unsigned(REXCVAR_GET(gow3_native_shader_prewarm_threads)));
    for (auto &[key, job] : sources)
      if (!stopping)
        prepare.TryEnqueue(job);
    prepare.Drain();
    prepare.StopAndJoin();
    // One optional CPU translation happens before CP pipeline preparation and
    // guest launch. Never translate this experimental variant during gameplay.
    if(prepare_retained&&!stopping) {
      try {
        for(const auto& item:recipes)if(item.pixel&&item.recipe.pixel&&
            *item.recipe.pixel==kRetainedOpaquePixel&&item.recipe.vertex==kRetainedOpaqueVertex&&
            item.recipe.raster[0]==uint32_t(plume::RenderCullMode::BACK)&&
            (item.recipe.raster[1]==uint32_t(plume::RenderFrontFace::CLOCKWISE)||item.recipe.raster[1]==uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE))&&
            item.recipe.raster[2]==1&&item.recipe.raster[3]==uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST)&&
            !item.recipe.raster[4]&&!item.recipe.raster[5]&&
            item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1) {
          retained_opaque_program=TryPrepareNativeRetainedOpaque(compiler,*item.pixel);break;
        }
        progress.Advance(bool(retained_opaque_program));
        if(retained_opaque_program)REXLOG_INFO("Retained opaque startup shader prepared: variant={}, spirv={}",retained_opaque_program->variant_version,retained_opaque_program->spirv_sha256);
        else REXLOG_INFO("Retained opaque startup shader unavailable; ordinary Native retained");
      }catch(const std::exception& error){progress.Advance(false);retained_opaque_program.reset();REXLOG_WARN("Retained opaque shader preparation refused: {}",error.what());}
    }
    if(prepare_retained_catalog&&!stopping) {
      auto catalog=std::make_shared<NativeRetainedOpaqueCatalog>();
      for(const auto& [digest,source]:retained_catalog_sources) {
        if(stopping)break;
        try {
          auto program=TryPrepareNativeRetainedOpaqueCatalog(compiler,*source);
          if(program&&program->source_sha256!=digest)throw std::runtime_error("Retained catalog producer source identity mismatch");
          const bool ready=bool(program)&&catalog->programs.size()<kRetainedOpaqueCatalogMaxPrograms;
          if(ready)catalog->programs.push_back(std::move(program));
          progress.Advance(ready);
        }catch(const std::exception& error){progress.Advance(false);REXLOG_WARN("Retained opaque catalog shader refused: {}",error.what());}
      }
      retained_opaque_catalog_programs=std::move(catalog);
      REXLOG_INFO("Retained opaque catalog startup shaders: prepared={}, maximum=3",retained_opaque_catalog_programs->programs.size());
    }
    if(prepare_stencil_material&&!stopping) {
      try {
        for(const auto& item:recipes)if(item.pixel&&item.recipe.pixel&&
            *item.recipe.pixel==kStencilMaterialPixel&&item.recipe.vertex==kStencilMaterialVertex&&
            item.recipe.raster==std::array<uint32_t,6>{3,2,1,4,0,0}&&
            item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1) {
          // The compiler authenticates owned source bytes and the canonical
          // module. The Scene separately authenticates the resident VS/PS.
          stencil_material_program=CompileNativeStencilMaterial(compiler,*item.pixel,NativeStencilMaterialMode::Uninstrumented);
          break;
        }
        progress.Advance(bool(stencil_material_program));
        if(stencil_material_program)
          REXLOG_INFO("Native stencil material startup shader prepared: mode=uninstrumented, variant={}, source={}",
              stencil_material_program->derived_version,stencil_material_program->source);
        else REXLOG_INFO("Native stencil material startup shader unavailable; ordinary Native shaders retained");
      }catch(const std::exception& error) {
        progress.Advance(false);stencil_material_program.reset();
        REXLOG_WARN("Native stencil material startup shader preparation refused: {}",error.what());
      }
    }
    if(prepare_opaque&&!stopping) {
      try {
        for(const auto& item:recipes)if(item.pixel&&item.recipe.pixel&&
            *item.recipe.pixel==kOpaqueColorOverwritePixel&&item.recipe.vertex==kOpaqueColorOverwriteVertex&&
            OpaqueColorOverwriteRasterKey(item.recipe.raster)) {
          opaque_overwrite_program=TryPrepareOpaqueColorOverwrite(compiler,*item.pixel);
          break;
        }
        progress.Advance(bool(opaque_overwrite_program));
        if(opaque_overwrite_program)
          REXLOG_INFO("Opaque Native overwrite prepared: variant={}, spirv={}",
              opaque_overwrite_program->variant_version,opaque_overwrite_program->compiled_spirv_sha256);
        else REXLOG_INFO("Opaque Native overwrite unavailable; ordinary Native shaders retained");
      } catch(const std::exception& error) {
        progress.Advance(false);opaque_overwrite_program.reset();
        REXLOG_WARN("Opaque Native overwrite preparation refused: {}",error.what());
      }
    }
    if(prepare_forward&&!stopping) {
      try {
        for(const auto& item:recipes)if(item.pixel&&item.recipe.pixel&&
            *item.recipe.pixel==kForwardControlFlowPixel&&item.recipe.vertex==kForwardControlFlowVertex&&
            ForwardControlFlowRasterKey(item.recipe.raster)&&
            item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1) {
          forward_control_flow_program=TryPrepareForwardControlFlow(compiler,*item.pixel);
          break;
        }
        progress.Advance(bool(forward_control_flow_program));
        if(forward_control_flow_program)
          REXLOG_INFO("Forward-CFG Native control flow prepared: variant={}, spirv={}",
              forward_control_flow_program->variant_version,forward_control_flow_program->compiled_spirv_sha256);
        else REXLOG_INFO("Forward-CFG Native control flow unavailable; ordinary Native shaders retained");
      } catch(const std::exception& error) {
        progress.Advance(false);forward_control_flow_program.reset();
        REXLOG_WARN("Forward-CFG Native control flow preparation refused: {}",error.what());
      }
    }
    if(prepare_forward_vertex&&!stopping) {
      try {
        for(const auto& item:recipes)if(ForwardVertexAllowed(item.recipe.vertex,item.recipe.raster)&&
            item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1) {
          forward_vertex_program=TryPrepareForwardVertex(compiler,*item.vertex);break;
        }
        progress.Advance(bool(forward_vertex_program));
        if(forward_vertex_program)REXLOG_INFO("Forward-VS prepared: variant={}, spirv={}",forward_vertex_program->variant_version,forward_vertex_program->compiled_spirv_sha256);
        else REXLOG_INFO("Forward-VS unavailable; ordinary Native shaders retained");
      } catch(const std::exception& error) {progress.Advance(false);forward_vertex_program.reset();REXLOG_WARN("Forward-VS preparation refused: {}",error.what());}
    }
    if(prepare_catalog&&!stopping) {
      auto prepared_catalog=std::make_shared<ForwardPixelCatalog>();
      for(const auto& [source,container]:catalog_sources) {
        if(stopping)break;
        try {
          auto program=TryPrepareForwardPixelCatalog(compiler,*container);progress.Advance(bool(program));
          if(program)prepared_catalog->programs.emplace(source,std::move(program));
          else REXLOG_INFO("Forward pixel catalog source refused: {}",source);
        }catch(const std::exception& error){progress.Advance(false);REXLOG_WARN("Forward pixel catalog source preparation refused: {} {}",source,error.what());}
      }
      if(!prepared_catalog->programs.empty())forward_pixel_catalog=std::move(prepared_catalog);
      REXLOG_INFO("Forward pixel catalog source preparation: loaded={}, requested={}, limit={}",forward_pixel_catalog?forward_pixel_catalog->programs.size():0,catalog_sources.size(),kForwardPixelCatalogMaxPrograms);
    }
    if((REXCVAR_GET(gow3_native_specialize_fsi_pipeline_state)||REXCVAR_GET(gow3_native_fsi_state_forward_cfg)||fsi_state_material_composition_enabled)&&!stopping) {
      auto catalog=std::make_shared<FsiStateSpecializationCatalog>();size_t bytes=0;
      for(const auto& source:state_sources) {
        if(stopping)break;
        try {
          auto program=REXCVAR_GET(gow3_native_fsi_state_forward_cfg)?
              (source.pixel?TryPrepareFsiStateForwardCfg(compiler,*source.bytes,fsi_state_shader_cache):nullptr):
              (source.pixel?TryPrepareFsiStateSpecialization(compiler,*source.bytes,fsi_state_shader_cache):
               TryPrepareDepthOnlyFsiStateSpecialization(compiler,fsi_state_shader_cache));
          const size_t size=program?program->spirv.size()*4:0;
          if(program&&size<=16u*1024*1024-bytes) {
            bytes+=size;
            if(source.pixel)catalog->programs.emplace(*source.pixel,std::move(program));
            else catalog->depth_only=std::move(program);
            progress.Advance();
          } else {
            progress.Advance(Outcome::kSkipped);
            REXLOG_INFO("Optional FSI state source skipped: source={}, reason={}, spirv_bytes={}, remaining_bytes={}",
                source.pixel?*source.pixel:"depth-only",program?"source_byte_budget":"unsupported_source",
                size,16u*1024*1024-bytes);
          }
        }catch(const std::exception& error){progress.Advance(false);REXLOG_WARN("FSI state source refused: {} {}",source.pixel?*source.pixel:"depth-only",error.what());}
      }
      fsi_state_programs=std::move(catalog);
    }
    // Append this separate optional source preparation after all existing
    // translations. Only authenticated observed catalogue recipes contribute;
    // compilation never becomes reachable from a gameplay draw.
    if(prepare_retained_phase&&!stopping) {
      auto catalog=std::make_shared<NativeRetainedOpaquePhaseCatalog>();uint64_t bytes=0;
      for(const auto& [digest,source]:retained_phase_sources) {
        if(stopping)break;
        try {
          auto program=TryPrepareNativeRetainedOpaquePhaseConservative(compiler,*source);
          if(program&&program->source_sha256!=digest)
            throw std::runtime_error("Retained opaque phase producer source identity mismatch");
          const uint64_t size=program?program->spirv.size()*sizeof(uint32_t):0;
          const bool ready=program&&catalog->programs.size()<kRetainedOpaquePhaseMaxPrograms&&
              size<=kRetainedOpaquePhaseMaxModuleBytes-bytes;
          if(ready){bytes+=size;catalog->programs.push_back(std::move(program));}
          progress.Advance(ready?Outcome::kPrepared:Outcome::kSkipped);
        }catch(const std::exception& error){progress.Advance(false);REXLOG_WARN("Retained opaque phase shader refused: {}",error.what());}
      }
      retained_opaque_phase_programs=std::move(catalog);
      REXLOG_INFO("Retained opaque phase startup shaders: requested={}, prepared={}, bytes={}, maximum={}, byte_limit={}",
          retained_phase_sources.size(),retained_opaque_phase_programs->programs.size(),bytes,
          kRetainedOpaquePhaseMaxPrograms,kRetainedOpaquePhaseMaxModuleBytes);
    }
    if (stopping) {
      progress.Finish();
      return;
    }
    struct Prepared {
      NativeFsiPipelineRecipe recipe;
      std::shared_ptr<const CompiledNativeShader> vertex, pixel;
    };
    std::vector<Prepared> prepared,overflow_prepared;
    const bool overflow_enabled=REXCVAR_GET(gow3_native_prewarm_overflow_pipelines);
    struct Admission {
      uint64_t requested=0,source_validation_failed=0,source_compile_failed=0;
      uint64_t pso_cap=0,module_count_cap=0,byte_cap=0,cancelled=0,overflow_storage_cap=0;
    } admission;
    admission.requested=recipes.size();
    size_t visited=0;
    std::set<std::string> modules;
    size_t module_bytes = 0;
    // Fit the existing 75-percent persistent retention targets so loading
    // splash frames don't immediately discard all the prepared action variants.
    for (const auto &item : recipes) {
      if (stopping || prepared.size() >= 1536) {
        (stopping?admission.cancelled:admission.pso_cap)+=recipes.size()-visited;
        break;
      }
      ++visited;
      bool source_invalid=false;
      try {
        auto vs = compiler.Get(*item.vertex, NativeShaderMode::Raster);
        auto ps = item.pixel ? (fsi_hoist_compiler?fsi_hoist_compiler->Get(*item.pixel):
            exact_fsi_compiler?exact_fsi_compiler->Get(*item.pixel):
                                 compiler.Get(*item.pixel, NativeShaderMode::FragmentInterlock))
                             : compiler.GetDepthOnlyFragmentInterlock();
        if (vs->stage != NativeShaderStage::Vertex || ps->stage != NativeShaderStage::Pixel ||
            vs->source_sha256 != item.recipe.vertex ||
            (item.recipe.pixel && ps->source_sha256 != *item.recipe.pixel)) {
          source_invalid=true;++admission.source_validation_failed;
          throw std::runtime_error("Observed native recipe source stage/identity mismatch");
        }
        const std::string vk = item.recipe.vertex + ".raster",
                          pk = item.recipe.pixel ? *item.recipe.pixel + ".fsi" : "depth-only.fsi";
        const size_t extra_count = size_t(!modules.contains(vk)) + size_t(!modules.contains(pk));
        const size_t extra_bytes = (modules.contains(vk) ? 0 : vs->spirv.size() * 4) +
                                   (modules.contains(pk) ? 0 : ps->spirv.size() * 4);
        if (modules.size() + extra_count > 768 || extra_bytes > 48u * 1024 * 1024 - module_bytes) {
          if(modules.size()+extra_count>768)++admission.module_count_cap;else ++admission.byte_cap;
          if(overflow_enabled) {
            if(overflow_prepared.size()<kNativeOverflowPrewarmMaximumPipelines)
              overflow_prepared.push_back({item.recipe,std::move(vs),std::move(ps)});
            else ++admission.overflow_storage_cap;
          }
          continue;
        }
        modules.insert(vk);
        modules.insert(pk);
        module_bytes += extra_bytes;
        prepared.push_back({item.recipe, std::move(vs), std::move(ps)});
      } catch (const std::exception &error) {
        if(!source_invalid)++admission.source_compile_failed;
        REXLOG_WARN("Observed native pipeline skipped: {}", error.what());
      }
    }
    REXLOG_INFO("Observed native pipeline admission: requested={}, selected={}, source_validation_failed={}, source_compile_failed={}, pso_cap={}, module_count_cap={}, byte_cap={}, cancelled={}, selected_modules={}, selected_module_bytes={}, overflow_enabled={}, overflow_candidates={}, overflow_storage_cap={}",
        admission.requested,prepared.size(),admission.source_validation_failed,admission.source_compile_failed,
        admission.pso_cap,admission.module_count_cap,admission.byte_cap,admission.cancelled,modules.size(),module_bytes,
        overflow_enabled,overflow_prepared.size(),admission.overflow_storage_cap);
    if (stopping || processor->is_paused()) {
      progress.Finish();
      return;
    }
    const auto stencil_material_recipe=[](const Prepared& item) {
      return item.recipe.pixel&&*item.recipe.pixel==kStencilMaterialPixel&&item.recipe.vertex==kStencilMaterialVertex&&
          item.recipe.raster==std::array<uint32_t,6>{3,2,1,4,0,0}&&
          item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1;
    };
    const size_t stencil_material_count=stencil_material_program&&
        std::any_of(prepared.begin(),prepared.end(),stencil_material_recipe)?1:0;
    const auto opaque_recipe=[](const Prepared& item) {
      return item.recipe.pixel&&*item.recipe.pixel==kOpaqueColorOverwritePixel&&
          item.recipe.vertex==kOpaqueColorOverwriteVertex&&OpaqueColorOverwriteRasterKey(item.recipe.raster);
    };
    const size_t opaque_count=opaque_overwrite_program?size_t(std::count_if(prepared.begin(),prepared.end(),opaque_recipe)):0;
    const auto forward_recipe=[](const Prepared& item) {
      return item.recipe.pixel&&*item.recipe.pixel==kForwardControlFlowPixel&&
          item.recipe.vertex==kForwardControlFlowVertex&&ForwardControlFlowRasterKey(item.recipe.raster)&&
          item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1;
    };
    const size_t forward_count=forward_control_flow_program?size_t(std::count_if(prepared.begin(),prepared.end(),forward_recipe)):0;
    const auto forward_vertex_recipe=[](const Prepared& item) {
      return ForwardVertexAllowed(item.recipe.vertex,item.recipe.raster)&&
          item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1;
    };
    const size_t forward_vertex_count=forward_vertex_program?std::min(size_t(kForwardVertexMaxPipelines),size_t(std::count_if(prepared.begin(),prepared.end(),forward_vertex_recipe))):0;
    const auto catalog_recipe=[this](const Prepared& item) {
      return item.recipe.pixel&&forward_pixel_catalog&&forward_pixel_catalog->programs.contains(*item.recipe.pixel)&&
          ForwardPixelCatalogAllowed(*item.recipe.pixel,item.recipe.raster)&&item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1;
    };
    const size_t catalog_count=forward_pixel_catalog?std::min(size_t(kForwardPixelCatalogMaxPipelines),size_t(std::count_if(prepared.begin(),prepared.end(),catalog_recipe))):0;
    std::shared_ptr<const NativeEarlyDepthProgram> early_program;
    if(early_depth_enabled)for(const auto& item:prepared) {
      if(!item.recipe.pixel||*item.recipe.pixel!=kNativeEarlyDepthPixelSource||
         item.recipe.vertex!=kNativeEarlyDepthVertexSource)continue;
      try {
        if(NativeEarlyDepthVertexCertified(item.vertex->spirv,item.recipe.vertex,compiler.CacheVersion()))
          early_program=TryPrepareNativeEarlyDepthShader(item.pixel->spirv,*item.recipe.pixel,compiler.CacheVersion());
      }catch(const std::exception& error){REXLOG_WARN("Native early-depth shader preparation refused: {}",error.what());}
      break;
    }
    const auto early_recipe=[](const Prepared& item) {
      return item.recipe.pixel&&*item.recipe.pixel==kNativeEarlyDepthPixelSource&&
             item.recipe.vertex==kNativeEarlyDepthVertexSource;
    };
    const size_t early_count=early_program?std::min(size_t(8),size_t(std::count_if(prepared.begin(),prepared.end(),early_recipe))):0;
    std::map<std::string,std::shared_ptr<const NativeEarlyDepthProgram>,std::less<>> early_catalog_programs;
    if(early_depth_catalog_enabled)for(const auto& item:prepared) {
      if(!item.recipe.pixel||!NativeEarlyDepthCatalogPair(item.recipe.vertex,*item.recipe.pixel)||
         early_catalog_programs.contains(*item.recipe.pixel))continue;
      try {
        if(!NativeEarlyDepthCatalogVertexCertified(item.vertex->spirv,item.recipe.vertex,compiler.CacheVersion()))continue;
        auto program=TryPrepareNativeEarlyDepthCatalogShader(item.pixel->spirv,*item.recipe.pixel,compiler.CacheVersion());
        if(program)early_catalog_programs.emplace(*item.recipe.pixel,std::move(program));
      }catch(const std::exception& error){REXLOG_WARN("Native early-depth catalogue shader refused: {}",error.what());}
    }
    const auto early_catalog_recipe=[&](const Prepared& item) {
      return item.recipe.pixel&&NativeEarlyDepthCatalogPair(item.recipe.vertex,*item.recipe.pixel)&&
             early_catalog_programs.contains(*item.recipe.pixel);
    };
    const size_t early_catalog_count=std::min(size_t(8)-early_count,
        size_t(std::count_if(prepared.begin(),prepared.end(),early_catalog_recipe)));
    const auto readonly_recipe=[](const Prepared& item) {return !item.recipe.pixel&&item.recipe.vertex==kReadonlyQueryVertexSource;};
    const size_t readonly_count=(REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)||readonly_depth_attachment_enabled)?
        std::min(size_t(128),size_t(std::count_if(prepared.begin(),prepared.end(),readonly_recipe))):0;
    const auto retained_recipe=[](const Prepared& item) {
      const auto& r=item.recipe;
      return r.pixel&&*r.pixel==kRetainedOpaquePixel&&r.vertex==kRetainedOpaqueVertex&&
          r.raster[0]==uint32_t(plume::RenderCullMode::BACK)&&
          (r.raster[1]==uint32_t(plume::RenderFrontFace::CLOCKWISE)||r.raster[1]==uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE))&&
          r.raster[2]==1&&r.raster[3]==uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST)&&!r.raster[4]&&!r.raster[5]&&
          r.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&r.depth_format==-1;
    };
    const size_t retained_count=retained_opaque_program?std::min(size_t(2),size_t(std::count_if(prepared.begin(),prepared.end(),retained_recipe))):0;
    const auto retained_catalog_recipe=[this](const Prepared& item) {
      if(!retained_opaque_catalog_programs||!item.recipe.pixel||!RetainedOpaqueCatalogPair(item.recipe.vertex,*item.recipe.pixel)||
         !RetainedOpaqueCatalogRaster(item.recipe.raster)||item.recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||item.recipe.depth_format!=-1)return false;
      return std::any_of(retained_opaque_catalog_programs->programs.begin(),retained_opaque_catalog_programs->programs.end(),
          [&](const auto& program){return program&&program->source_sha256==*item.recipe.pixel&&program->vertex_sha256==item.recipe.vertex;});
    };
    const size_t retained_catalog_count=retained_opaque_catalog_programs?std::min(size_t(kRetainedOpaqueCatalogMaxPipelines),
        size_t(std::count_if(prepared.begin(),prepared.end(),retained_catalog_recipe))):0;
    const auto retained_phase_recipe=[this](const Prepared& item) {
      const auto& r=item.recipe;
      if(!retained_opaque_phase_programs||!r.pixel||!NativeRetainedOpaquePhaseRecipe(r.vertex,*r.pixel,r.raster)||
         r.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||r.depth_format!=-1)return false;
      return std::any_of(retained_opaque_phase_programs->programs.begin(),retained_opaque_phase_programs->programs.end(),
          [&](const auto& program){return program&&program->source_sha256==*r.pixel;});
    };
    const size_t retained_phase_count=retained_opaque_phase_programs?std::min(size_t(kRetainedOpaquePhaseMaxPipelines),
        size_t(std::count_if(prepared.begin(),prepared.end(),retained_phase_recipe))):0;
    const auto shared_depth_recipe=[](const Prepared& item) {
      const auto& r=item.recipe;
      return !r.pixel&&r.vertex==kReadonlyQueryVertexSource&&r.raster==std::array<uint32_t,6>{3,2,1,4,0,0}&&
          r.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&r.depth_format==-1;
    };
    const size_t shared_depth_count=retained_shared_depth_queries_enabled?
        std::min(size_t(1),size_t(std::count_if(prepared.begin(),prepared.end(),shared_depth_recipe))):0;
    const auto fp24_depth_recipe=[](const Prepared& item) {
      const auto& r=item.recipe;
      return !r.pixel&&NativeRetainedFp24DepthSourceIndex(r.vertex)&&NativeRetainedFp24DepthRaster(r.raster)&&
          r.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&r.depth_format==-1;
    };
    const size_t fp24_depth_count=retained_fp24_depth_writer_enabled?
        std::min(size_t(4),size_t(std::count_if(prepared.begin(),prepared.end(),fp24_depth_recipe))):0;
    progress.Begin(Phase::kPipelines, prepared.size()+overflow_prepared.size()+early_count+early_catalog_count+fp24_depth_count+shared_depth_count+retained_catalog_count+retained_count+stencil_material_count+opaque_count+forward_count+forward_vertex_count+catalog_count+readonly_count+state_records.size()+retained_phase_count);
    std::weak_ptr<RuntimeState> weak = shared_from_this();
    auto task = std::make_shared<NativePipelineStartupTask>(
        [weak, &progress, prepared = std::move(prepared), overflow_prepared=std::move(overflow_prepared), overflow_enabled, state_records=std::move(state_records), early_program=std::move(early_program), early_catalog_programs=std::move(early_catalog_programs)]() mutable {
          auto owner = weak.lock();
          if (!owner || owner->stopping)
            return;
          owner->Initialize();
          uint64_t loaded = 0, skipped = 0;
          for (const auto &item : prepared) {
            if (owner->stopping)
              break;
            try {
              owner->renderer->PrewarmFsiPipeline(item.recipe, item.vertex->spirv, item.pixel->spirv);
              ++loaded;
              progress.Advance();
            } catch (const std::exception &error) {
              ++skipped;
              progress.Advance(false);
              REXLOG_WARN("Native pipeline preparation skipped: {}", error.what());
            }
          }
          uint64_t fp24_loaded=0,fp24_skipped=0;
          if(owner->retained_fp24_depth_writer_enabled)for(const auto& item:prepared) {
            if(owner->stopping||fp24_loaded+fp24_skipped>=4)break;
            const auto& r=item.recipe;
            if(r.pixel||!NativeRetainedFp24DepthSourceIndex(r.vertex)||!NativeRetainedFp24DepthRaster(r.raster)||
               r.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||r.depth_format!=-1)continue;
            try {const bool ready=owner->renderer->PrewarmRetainedFp24DepthPipeline(r,item.vertex->spirv,item.pixel->spirv);
              ready?++fp24_loaded:++fp24_skipped;progress.Advance(ready);
            }catch(const std::exception& error){++fp24_skipped;progress.Advance(false);REXLOG_WARN("Retained FP24 depth PSO refused: {}",error.what());}
          }
          if(owner->retained_fp24_depth_writer_enabled)
            REXLOG_INFO("Retained FP24 depth startup PSOs: loaded={}, skipped={}, maximum=4",fp24_loaded,fp24_skipped);
          uint64_t retained_loaded=0,retained_skipped=0;
          if(owner->retained_opaque_program)for(const auto& item:prepared) {
            if(owner->stopping||retained_loaded+retained_skipped>=2)break;
            const auto& r=item.recipe;
            if(!r.pixel||*r.pixel!=kRetainedOpaquePixel||r.vertex!=kRetainedOpaqueVertex||
               r.raster[0]!=uint32_t(plume::RenderCullMode::BACK)||
               (r.raster[1]!=uint32_t(plume::RenderFrontFace::CLOCKWISE)&&r.raster[1]!=uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE))||
               r.raster[2]!=1||r.raster[3]!=uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST)||r.raster[4]||r.raster[5]||
               r.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||r.depth_format!=-1)continue;
            try {const bool ready=owner->renderer->PrewarmRetainedOpaquePipeline(r,item.vertex->spirv,item.pixel->spirv);
              ready?++retained_loaded:++retained_skipped;progress.Advance(ready);
            }catch(const std::exception& error){++retained_skipped;progress.Advance(false);REXLOG_WARN("Retained opaque PSO preparation refused: {}",error.what());}
          }
          if(owner->retained_opaque_program)REXLOG_INFO("Retained opaque startup PSOs: loaded={}, skipped={}, maximum=2",retained_loaded,retained_skipped);
          uint64_t retained_catalog_loaded=0,retained_catalog_skipped=0;
          if(owner->retained_opaque_catalog_programs)for(const auto& item:prepared) {
            if(owner->stopping||retained_catalog_loaded+retained_catalog_skipped>=kRetainedOpaqueCatalogMaxPipelines)break;
            const auto& r=item.recipe;
            if(!r.pixel||!RetainedOpaqueCatalogPair(r.vertex,*r.pixel)||!RetainedOpaqueCatalogRaster(r.raster)||
               r.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||r.depth_format!=-1)continue;
            const bool compiled=std::any_of(owner->retained_opaque_catalog_programs->programs.begin(),owner->retained_opaque_catalog_programs->programs.end(),
                [&](const auto& program){return program&&program->source_sha256==*r.pixel&&program->vertex_sha256==r.vertex;});
            if(!compiled)continue;
            try {const bool ready=owner->renderer->PrewarmRetainedOpaquePipeline(r,item.vertex->spirv,item.pixel->spirv);
              ready?++retained_catalog_loaded:++retained_catalog_skipped;progress.Advance(ready);
            }catch(const std::exception& error){++retained_catalog_skipped;progress.Advance(false);REXLOG_WARN("Retained catalog PSO refused: {}",error.what());}
          }
          if(owner->retained_opaque_catalog_programs)REXLOG_INFO("Retained opaque catalog startup PSOs: loaded={}, skipped={}, maximum=6",retained_catalog_loaded,retained_catalog_skipped);
          // This optional readonly PSO shares the exact retained depth image.
          // Prepare only after all generic and V2 material entries are resident.
          uint64_t shared_depth_loaded=0,shared_depth_skipped=0;
          if(owner->retained_shared_depth_queries_enabled)for(const auto& item:prepared) {
            if(owner->stopping||shared_depth_loaded+shared_depth_skipped>=1)break;
            const auto& r=item.recipe;
            if(r.pixel||r.vertex!=kReadonlyQueryVertexSource||r.raster!=std::array<uint32_t,6>{3,2,1,4,0,0}||
               r.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||r.depth_format!=-1)continue;
            try {const bool ready=owner->renderer->PrewarmRetainedSharedDepthQueryPipeline(r,item.vertex->spirv,item.pixel->spirv);
              ready?++shared_depth_loaded:++shared_depth_skipped;progress.Advance(ready);
            }catch(const std::exception& error){++shared_depth_skipped;progress.Advance(false);REXLOG_WARN("Retained shared-depth query PSO refused: {}",error.what());}
          }
          if(owner->retained_shared_depth_queries_enabled)
            REXLOG_INFO("Retained shared-depth query startup PSO: loaded={}, skipped={}, maximum=1",shared_depth_loaded,shared_depth_skipped);
          NativeFsiActionStateSourceLookup<Prepared> fsi_state_sources(
              prepared,overflow_prepared,owner->fsi_action_state_overflow_lookup_enabled);
          const auto prepare_fsi_states=[&] {
            uint64_t state_loaded=0,state_skipped=0,overflow_found=0,overflow_loaded=0;
            // Load ranked exact tuples after every generic PSO. Neither compiler
            // nor variant PSO creation is reachable from the gameplay path.
            for(const auto& record:state_records) {
              if(owner->stopping)break;
              const auto original=fsi_state_sources.Find(record.recipe);
              if(original.overflow)++overflow_found;
              try {
                const bool ready=original.source&&owner->renderer->PrewarmFsiStatePipeline(record.recipe,original.source->vertex->spirv);
                ready?++state_loaded:++state_skipped;
                if(ready&&original.overflow)++overflow_loaded;
                progress.Advance(ready?Outcome::kPrepared:Outcome::kSkipped);
                if(!ready)REXLOG_INFO("Optional FSI state PSO skipped: vertex={}, pixel={}, reason={}",
                    record.recipe.generic.vertex,record.recipe.generic.pixel?*record.recipe.generic.pixel:"depth-only",
                    !original.source?"generic_not_admitted":"optional_state_not_admitted");
              }catch(const std::exception& error){++state_skipped;progress.Advance(false);REXLOG_WARN("FSI state PSO refused: {}",error.what());}
            }
            if(owner->fsi_state_programs)REXLOG_INFO("FSI state PSO preparation: loaded={}, skipped={}, maximum={}",state_loaded,state_skipped,kFsiStateMaxPipelines);
            if(owner->fsi_action_state_overflow_lookup_enabled)REXLOG_INFO(
                "FSI action state overflow source lookup: ready_generics={}, found={}, loaded={}, refused={}",
                fsi_state_sources.ReadyCount(),overflow_found,overflow_loaded,overflow_found-overflow_loaded);
          };
          // Preserve the historical broad-mode ordering. The new
          // composition must not consume shipping startup headroom.
          if(!owner->fsi_state_material_composition_enabled)prepare_fsi_states();
          // Add alternatives only after all generic recipes, avoiding eviction
          // by later startup warmup. Actual draws still check live residency.
          uint64_t stencil_material_loaded=0,stencil_material_skipped=0;
          if(owner->stencil_material_program)for(const auto& item:prepared) {
            if(owner->stopping)break;
            if(!item.recipe.pixel||*item.recipe.pixel!=kStencilMaterialPixel||item.recipe.vertex!=kStencilMaterialVertex||
               item.recipe.raster!=std::array<uint32_t,6>{3,2,1,4,0,0}||
               item.recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||item.recipe.depth_format!=-1)continue;
            try {
              const bool ready=owner->renderer->PrewarmStencilMaterialPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
              ready?++stencil_material_loaded:++stencil_material_skipped;progress.Advance(ready);
            }catch(const std::exception& error) {
              ++stencil_material_skipped;progress.Advance(false);
              REXLOG_WARN("Native stencil material startup pipeline refused: {}",error.what());
            }
            break; // One exact alternative; unavailable/capacity refusal keeps ordinary Native.
          }
          if(REXCVAR_GET(gow3_native_stencil_material_cutoff))
            REXLOG_INFO("Native stencil material startup pipeline: program_ready={}, loaded={}, skipped={}, maximum=1",
                bool(owner->stencil_material_program),stencil_material_loaded,stencil_material_skipped);
          uint64_t opaque_loaded=0,opaque_skipped=0;
          if(owner->opaque_overwrite_program)for(const auto& item:prepared) {
            if(owner->stopping)break;
            if(!item.recipe.pixel||*item.recipe.pixel!=kOpaqueColorOverwritePixel||
               item.recipe.vertex!=kOpaqueColorOverwriteVertex||!OpaqueColorOverwriteRasterKey(item.recipe.raster))continue;
            try {
              const bool ready=owner->renderer->PrewarmOpaqueColorOverwritePipeline(item.recipe,item.vertex->spirv);
              ready?++opaque_loaded:++opaque_skipped;progress.Advance(ready);
            } catch(const std::exception& error) {
              ++opaque_skipped;progress.Advance(false);
              REXLOG_WARN("Opaque Native pipeline preparation refused: {}",error.what());
            }
          }
          if(owner->opaque_overwrite_program)
            REXLOG_INFO("Opaque Native pipeline preparation: loaded={}, skipped={}",opaque_loaded,opaque_skipped);
          uint64_t forward_loaded=0,forward_skipped=0;
          if(owner->forward_control_flow_program)for(const auto& item:prepared) {
            if(owner->stopping)break;
            if(!item.recipe.pixel||*item.recipe.pixel!=kForwardControlFlowPixel||
               item.recipe.vertex!=kForwardControlFlowVertex||!ForwardControlFlowRasterKey(item.recipe.raster)||
               item.recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||item.recipe.depth_format!=-1)continue;
            try {
              const bool ready=owner->renderer->PrewarmForwardControlFlowPipeline(item.recipe,item.vertex->spirv);
              ready?++forward_loaded:++forward_skipped;progress.Advance(ready);
            } catch(const std::exception& error) {
              ++forward_skipped;progress.Advance(false);
              REXLOG_WARN("Forward-CFG Native pipeline preparation refused: {}",error.what());
            }
          }
          if(owner->forward_control_flow_program)
            REXLOG_INFO("Forward-CFG Native pipeline preparation: loaded={}, skipped={}",forward_loaded,forward_skipped);
          uint64_t forward_vertex_loaded=0,forward_vertex_skipped=0;
          if(owner->forward_vertex_program)for(const auto& item:prepared) {
            if(owner->stopping||forward_vertex_loaded+forward_vertex_skipped>=kForwardVertexMaxPipelines)break;
            if(!ForwardVertexAllowed(item.recipe.vertex,item.recipe.raster)||
               item.recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||item.recipe.depth_format!=-1)continue;
            try {
              const bool ready=owner->renderer->PrewarmForwardVertexPipeline(item.recipe,item.vertex->spirv);
              ready?++forward_vertex_loaded:++forward_vertex_skipped;progress.Advance(ready);
            } catch(const std::exception& error) {++forward_vertex_skipped;progress.Advance(false);REXLOG_WARN("Forward-VS pipeline preparation refused: {}",error.what());}
          }
          if(owner->forward_vertex_program)REXLOG_INFO("Forward-VS pipeline preparation: loaded={}, skipped={}, limit={}",forward_vertex_loaded,forward_vertex_skipped,kForwardVertexMaxPipelines);
          uint64_t catalog_loaded=0,catalog_skipped=0;
          if(owner->forward_pixel_catalog)for(const auto& item:prepared) {
            if(owner->stopping||catalog_loaded+catalog_skipped>=kForwardPixelCatalogMaxPipelines)break;
            if(!item.recipe.pixel||!owner->forward_pixel_catalog->programs.contains(*item.recipe.pixel)||
               !ForwardPixelCatalogAllowed(*item.recipe.pixel,item.recipe.raster)||item.recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||item.recipe.depth_format!=-1)continue;
            try {
              const bool ready=owner->renderer->PrewarmForwardPixelCatalogPipeline(item.recipe,item.vertex->spirv);
              ready?++catalog_loaded:++catalog_skipped;progress.Advance(ready);
            }catch(const std::exception& error){++catalog_skipped;progress.Advance(false);REXLOG_WARN("Forward pixel catalog pipeline preparation refused: {}",error.what());}
          }
          if(owner->forward_pixel_catalog)REXLOG_INFO("Forward pixel catalog pipeline preparation: loaded={}, skipped={}, limit={}",catalog_loaded,catalog_skipped,kForwardPixelCatalogMaxPipelines);
          uint64_t early_loaded=0,early_skipped=0;
          if(early_program)for(const auto& item:prepared) {
            if(owner->stopping||early_loaded+early_skipped>=8)break;
            if(!item.recipe.pixel||*item.recipe.pixel!=kNativeEarlyDepthPixelSource||
               item.recipe.vertex!=kNativeEarlyDepthVertexSource)continue;
            try {
              const bool ready=owner->renderer->PrewarmNativeEarlyDepthPipeline(
                  item.recipe,item.vertex->spirv,item.pixel->spirv,early_program);
              ready?++early_loaded:++early_skipped;progress.Advance(ready);
            }catch(const std::exception& error){++early_skipped;progress.Advance(false);REXLOG_WARN("Native early-depth pipeline preparation refused: {}",error.what());}
          }
          if(owner->early_depth_enabled)REXLOG_INFO("Native early-depth preparation: loaded={}, skipped={}, limit=8",early_loaded,early_skipped);
          uint64_t early_catalog_loaded=0,early_catalog_skipped=0;
          if(owner->early_depth_catalog_enabled)for(const auto& item:prepared) {
            if(owner->stopping||early_loaded+early_skipped+early_catalog_loaded+early_catalog_skipped>=8)break;
            if(!item.recipe.pixel||!NativeEarlyDepthCatalogPair(item.recipe.vertex,*item.recipe.pixel))continue;
            const auto program=early_catalog_programs.find(*item.recipe.pixel);
            if(program==early_catalog_programs.end())continue;
            try {
              const bool ready=owner->renderer->PrewarmNativeEarlyDepthCatalogPipeline(
                  item.recipe,item.vertex->spirv,item.pixel->spirv,program->second);
              ready?++early_catalog_loaded:++early_catalog_skipped;progress.Advance(ready);
            }catch(const std::exception& error){++early_catalog_skipped;progress.Advance(false);REXLOG_WARN("Native early-depth catalogue pipeline refused: {}",error.what());}
          }
          if(owner->early_depth_catalog_enabled)REXLOG_INFO("Native early-depth catalogue preparation: loaded={}, skipped={}, combined_limit=8",early_catalog_loaded,early_catalog_skipped);
          uint64_t readonly_loaded=0,readonly_skipped=0,readonly_attempts=0;
          if(REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)||owner->readonly_depth_attachment_enabled)for(const auto& item:prepared) {
            if(owner->stopping||readonly_attempts>=128)break;
            if(item.recipe.pixel||item.recipe.vertex!=kReadonlyQueryVertexSource)continue;
            ++readonly_attempts;
            try {
              const bool ready=owner->readonly_depth_attachment_enabled?
                  owner->renderer->PrewarmNativeReadonlyDepthPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv):
                  owner->renderer->PrewarmReadonlyDepthQueryPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
              ready?++readonly_loaded:++readonly_skipped;progress.Advance(ready);
            }catch(const std::exception& error) {
              ++readonly_skipped;progress.Advance(false);REXLOG_WARN("Read-only Native query prewarm refused: {}",error.what());
            }
          }
          if(REXCVAR_GET(gow3_native_builtin_readonly_depth_queries)||owner->readonly_depth_attachment_enabled)
            REXLOG_INFO("Read-only Native query preparation: loaded={}, skipped={}, limit=128",readonly_loaded,readonly_skipped);
          uint64_t overflow_ready=0,overflow_created=0,overflow_resident=0,overflow_refused=0,overflow_cancelled=0;
          uint64_t overflow_invalid=0,overflow_unsupported=0,overflow_pso_cap=0,overflow_module_cap=0,overflow_byte_cap=0;
          uint64_t overflow_shader_failure=0,overflow_pipeline_failure=0,temporary_modules=0,temporary_bytes=0;
          for(size_t i=0;i<overflow_prepared.size();++i) {
            if(owner->stopping) {overflow_cancelled=overflow_prepared.size()-i;break;}
            const auto& item=overflow_prepared[i];
            try {
              const auto result=owner->renderer->PrewarmFsiOverflowPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
              fsi_state_sources.RecordOverflowResult(i,result);
              if(result.ready()) {
                ++overflow_ready;temporary_modules+=result.temporary_modules;temporary_bytes+=result.temporary_bytes;
                if(result.status==NativeOverflowPrewarmStatus::Prepared)++overflow_created;else ++overflow_resident;
              } else {
                ++overflow_refused;
                switch(result.status) {
                case NativeOverflowPrewarmStatus::InvalidRecipe:++overflow_invalid;break;
                case NativeOverflowPrewarmStatus::UnsupportedVariant:++overflow_unsupported;break;
                case NativeOverflowPrewarmStatus::PipelineCap:++overflow_pso_cap;break;
                case NativeOverflowPrewarmStatus::ModuleCountCap:++overflow_module_cap;break;
                case NativeOverflowPrewarmStatus::ByteCap:++overflow_byte_cap;break;
                case NativeOverflowPrewarmStatus::ShaderCreationFailed:++overflow_shader_failure;break;
                case NativeOverflowPrewarmStatus::PipelineCreationFailed:++overflow_pipeline_failure;break;
                default:throw std::logic_error("Unexpected overflow prewarm result");
                }
              }
              progress.Advance(result.ready());
            } catch(const std::exception& error) {
              ++overflow_refused;++overflow_pipeline_failure;progress.Advance(false);
              REXLOG_WARN("Native overflow pipeline preparation refused: {}",error.what());
            }
          }
          REXLOG_INFO("Native overflow pipeline preparation: enabled={}, requested={}, ready={}, created={}, already_resident={}, refused={}, cancelled={}, invalid={}, unsupported={}, pso_cap={}, module_count_cap={}, byte_cap={}, shader_failure={}, pipeline_failure={}, temporary_modules={}, temporary_module_bytes={}, temporary_counts_are_cumulative=true",
              overflow_enabled,overflow_prepared.size(),overflow_ready,overflow_created,overflow_resident,overflow_refused,
              overflow_cancelled,overflow_invalid,overflow_unsupported,overflow_pso_cap,overflow_module_cap,overflow_byte_cap,
              overflow_shader_failure,overflow_pipeline_failure,temporary_modules,temporary_bytes);
          // D32, early-depth and generic overflow preparation retain their
          // original admission priority. Optional material tuples use only
          // remaining hard capacity and never evict these resident owners.
          if(owner->fsi_state_material_composition_enabled)prepare_fsi_states();
          // Last optional admission: preserve all generic, query, early-depth,
          // overflow and specialized-material owners. Scene accepts only an
          // already-resident canonical recipe and never evicts it to make room.
          uint64_t phase_loaded=0,phase_skipped=0;
          if(owner->retained_opaque_phase_programs)for(const auto& item:prepared) {
            if(owner->stopping||phase_loaded+phase_skipped>=kRetainedOpaquePhaseMaxPipelines)break;
            const auto& r=item.recipe;
            if(!r.pixel||!NativeRetainedOpaquePhaseRecipe(r.vertex,*r.pixel,r.raster)||
               r.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||r.depth_format!=-1)continue;
            const bool compiled=std::any_of(owner->retained_opaque_phase_programs->programs.begin(),owner->retained_opaque_phase_programs->programs.end(),
                [&](const auto& program){return program&&program->source_sha256==*r.pixel;});
            if(!compiled)continue;
            try {
              const bool ready=owner->renderer->PrewarmRetainedOpaquePhasePipeline(r,item.vertex->spirv,item.pixel->spirv);
              ready?++phase_loaded:++phase_skipped;progress.Advance(ready?Outcome::kPrepared:Outcome::kSkipped);
            }catch(const std::exception& error){++phase_skipped;progress.Advance(false);REXLOG_WARN("Retained opaque phase PSO refused: {}",error.what());}
          }
          if(owner->retained_opaque_phase_programs)
            REXLOG_INFO("Retained opaque phase startup PSOs: loaded={}, skipped={}, maximum={}",phase_loaded,phase_skipped,kRetainedOpaquePhaseMaxPipelines);
          REXLOG_INFO("Observed native pipeline preparation: loaded={}, skipped={}, cancelled={}, "
                      "initial_edram_import_pending={}",
                      loaded, skipped, owner->stopping.load(), !owner->edram_initialized);
        });
    auto finished = task->Future();
    processor->CallInThread([task] { task->Run(); });
    // A CP paused/stopped after the initial check may never admit the task.
    // Bound only pending admission, not an already-running driver call. A
    // cancelled callback contains no progress reference or RuntimeState owner
    // and is harmless if the CP later resumes or discards its pending queue.
    const auto admission_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (finished.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
      if (stopping || std::chrono::steady_clock::now() >= admission_deadline) {
        if (task->CancelPending())
          REXLOG_INFO("Native observed pipeline preparation cancelled before CP admission");
      }
    }
    finished.get();
    progress.Finish();
  }

  void BeginExecution(std::shared_ptr<LiveRenderFrame> batch) {
    if (executing_frame == UINT32_MAX) {
      executing_frame = batch->frame;
      executed_operations = 0;
      execution_milliseconds = 0;
      operation_timings={};
      transaction_flush_milliseconds=0;
      batch_completion_milliseconds=0;
      batch_completion_count=0;
    } else if (executing_frame != batch->frame) {
      throw std::runtime_error("Native query prefixes crossed an unfinished API frame");
    }
    active_frame = std::move(batch);
    Initialize();
  }

  void LogQueryPublication(const PendingQuery& query,uint64_t samples) {
    // A scene can publish hundreds of queries each frame. Keep startup proof
    // and sparse progress without formatting/writing success logs every frame.
    if(++completed_query_prefixes<=3||completed_query_prefixes%10000==0)
      REXLOG_INFO("Native query publication {} at API frame {}, generation {}, report 0x{:08X}: {} actual samples",
                  completed_query_prefixes,query.frame,query.boundary.generation,query.boundary.end_address,samples);
  }

  void LogQueryReadiness(const char* phase) {
    if(published_results) {
      const auto s=Gow3ReadPublishedQueryStatistics();
      REXLOG_INFO("Native published query {}: checks={} ready={} bound={} success={} pending={} errors={}; global_marker_changes=0 sample_count_overrides=0",phase,s[0],s[1],s[2],s[3],s[4],s[5]);
      if(published_results->observing_polls()) {
        const auto census=published_results->ObservePolls();const auto& c=census.counts;
        REXLOG_INFO("Native query wait census {}: polls={} untracked={} unbound={} wrong_issuer={} ineligible={} mismatch={} pending={} ready={}; cp_unknown={} cp_staged={} cp_submitted={} cp_completed={} generation_changes={} before_execution={} generation_distance_sum={} generation_distance_max={}; result_changes=0 submission_changes=0",
            phase,census.latest.sequence,c[0],c[1],c[2],c[3],c[4],c[5],c[6],
            query_wait_stages[0],query_wait_stages[1],query_wait_stages[2],query_wait_stages[3],query_wait_generation_changes,query_wait_before_execution,query_wait_generation_distance,query_wait_max_generation_distance);
      }
    }
    if(!query_readiness)return;
    const auto s=Gow3ReadQueryReadinessStatistics();
    REXLOG_INFO("Native query readiness {}: samples={} tracked={} published_before={} pending_after_published={} completed_after_published={} changed_during_call={} errors={}; interval=1024 result_changes=0",
        phase,s.samples,s.tracked,s.published_before,s.pending_after_published,
        s.completed_after_published,s.changed_during_call,s.errors);
  }

  void PublishReadyQueries(bool poll=false) {
    if(!deferred_queries||!renderer)return;
    if(published_results&&published_results->observing_polls()) {
      const auto observed=published_results->ObservePolls().latest;
      if(observed.sequence!=observed_query_poll_sequence) {
        observed_query_poll_sequence=observed.sequence;
        if(observed.status==PublishedQueryResults::PollStatus::Pending) {
          const auto work=renderer->ObserveQueryWork(observed.frame,observed.generation,observed.address);
          ++query_wait_stages[size_t(work.stage)];
          if(work.stage==SceneExecutor::QueryWorkStage::Unknown&&observed.generation>latest_executed_query_generation) {
            ++query_wait_before_execution;
            const auto distance=observed.generation-latest_executed_query_generation;
            query_wait_generation_distance+=distance;
            query_wait_max_generation_distance=std::max(query_wait_max_generation_distance,distance);
          }
          if(observed.generation!=observed_query_poll_generation) {
            observed_query_poll_generation=observed.generation;++query_wait_generation_changes;
          }
        }
      }
    }
    if(retire_query_prefixes) {
      if(poll)try {renderer->PollCompletedQueries();}
      catch(const std::exception& error) {
        throw NativeTransactionFatal("Accepted native query fence poll failed: "+std::string(error.what()));
      }
    } else if(renderer->HasPendingGpuWork())return;
    // In prefix mode Scene proves each end's accepted-work frontier, including
    // unrelated draws/transfers before zero-draw ends. Future work does not
    // delay an already-fenced result; the SDK completion latch remains set.
    for(;;) {
      if(ready_query_publications.empty()) {
        ready_query_publications=renderer->TakeCompletedQueries();
        next_ready_query=0;
        if(ready_query_publications.empty())return;
      }
      for(;next_ready_query<ready_query_publications.size();++next_ready_query) {
        const auto& result=ready_query_publications[next_ready_query];
        const bool deferred=!pending_query_publications.empty();
        // A failure after sealing the current end but before Defer still owns
        // its immediate SDK capability. Handback may publish that exact end;
        // it must never manufacture a zero subtotal or restart an ended query.
        const PendingQuery* expected=deferred?&pending_query_publications.front():
            current_query_end?&*current_query_end:nullptr;
        if(!expected||result.frame!=expected->frame||result.generation!=expected->boundary.generation||
           result.end_event!=expected->end_event||result.result.end_address!=expected->boundary.end_address)
          throw NativeTransactionFatal("Deferred native query completion lost its exact API/packet identity");
        if(direct)Gow3DirectQueryResult(expected->boundary.generation,result.result.samples);
        else try {processor->PublishNativeQueryResult(expected->boundary,result.result.samples);}
        catch(const std::exception& error) {
          throw NativeTransactionFatal("Accepted native query publication failed: "+std::string(error.what()));
        }
        if(query_readiness)query_readiness->Publish(expected->frame,expected->boundary.generation,expected->boundary.end_address);
        if(published_results)published_results->Publish(expected->frame,expected->boundary.generation,expected->boundary.end_address);
        LogQueryPublication(*expected,result.result.samples);
        if(deferred)pending_query_publications.pop_front();
      }
      ready_query_publications.clear();next_ready_query=0;
    }
  }

  static CompletionCause CompletionCauseForOperation(std::string_view type) noexcept {
    if(type=="cpu_memory_write")return CompletionCause::CpuWrite;
    if(type=="present")return CompletionCause::PresentOperation;
    if(type=="memory_copy")return CompletionCause::MemoryCopy;
    return CompletionCause::OtherOperation;
  }
  static CompletionCause CompletionCauseForSdk(rex::graphics::NativeCompletionReason cause) noexcept {
    using Reason=rex::graphics::NativeCompletionReason;
    switch(cause) {
    case Reason::CommandPacket:return CompletionCause::SdkCommandPacket;
    case Reason::ScratchWrite:return CompletionCause::SdkScratchWrite;
    case Reason::Wait:return CompletionCause::SdkWait;
    case Reason::PrimaryBufferEnd:return CompletionCause::SdkPrimaryBufferEnd;
    case Reason::Presentation:return CompletionCause::SdkPresentation;
    case Reason::ReferenceDraw:return CompletionCause::SdkReferenceDraw;
    case Reason::ProviderRemoval:return CompletionCause::SdkProviderRemoval;
    case Reason::QueryPressure:return CompletionCause::SdkQueryPressure;
    case Reason::Shutdown:return CompletionCause::SdkShutdown;
    }
    return CompletionCause::Unclassified;
  }
  void RecordCompletionCause(CompletionCauseCounters& row,double elapsed,
      const SceneExecutor::QueueCompletionSnapshot& before,bool completed) noexcept {
    row.completed_calls+=completed;row.failed_calls+=!completed;
    row.completion_wall_ms+=elapsed;
    row.maximum_completion_wall_ms=std::max(row.maximum_completion_wall_ms,elapsed);
    const auto after=renderer->ObserveQueueCompletion();
    if(before.available&&after.available) {
      ++row.queue_observed_completions;
      row.queue_wait_call_completions+=after.waits!=before.waits;
      row.queue_waits+=after.waits-before.waits;
      if(before.timings_enabled&&after.timings_enabled) {
        ++row.queue_timed_completions;
        row.queue_fence_wait_ms+=after.fence_wait_ms-before.fence_wait_ms;
      }
    }
  }
  nlohmann::json CompletionCauseReport() const {
    nlohmann::json reasons=nlohmann::json::object();
    for(size_t i=0;i<completion_causes.size();++i) {
      const auto& c=completion_causes[i];
      reasons[completion_cause_names[i]]={{"calls",c.calls},{"no_renderer_calls",c.no_renderer_calls},
        {"no_op_calls",c.no_op_calls},{"pending_work_calls",c.pending_work_calls},
        {"query_only_calls",c.query_only_calls},{"completed_calls",c.completed_calls},
        {"failed_calls",c.failed_calls},{"completion_wall_ms",c.completion_wall_ms},
        {"maximum_completion_wall_ms",c.maximum_completion_wall_ms},
        {"queue_observed_completions",c.queue_observed_completions},
        {"queue_timed_completions",c.queue_timed_completions},
        {"queue_wait_call_completions",c.queue_wait_call_completions},
        {"queue_waits",c.queue_waits},{"queue_fence_wait_ms",c.queue_fence_wait_ms}};
    }
    return {{"schema",1},{"enabled",true},{"reasons",std::move(reasons)},
      {"excluded_retained_query_completion_calls",excluded_retained_query_completion_calls},
      {"scope","Outer Runtime CompletePending only; caller wall time includes commit, queue retirement and query publication. Existing queue fence-wait counter delta is a subset, never additive; wait-call counts do not prove the CPU blocked."},
      {"limits","Queue wait milliseconds require existing GPU timings; compare queue_timed_completions with completed_calls+failed_calls. No new timestamps are enabled. CPU upload standalone fences and internal Scene drains outside CompletePending are excluded. Existing fence_wait_ms may include fence reset/retirement wrapper overhead; SDK command_packet does not distinguish opcodes. Original draw timing still includes completion wall time."}};
  }

  // Accepted packets cannot be replayed if submission or completion fails. This
  // deliberately ignores stopping/failed: those latches stop new admission,
  // while accepted GPU resources must survive through their fence.
  void CompletePending(CompletionCause cause) {
    auto* diagnostic=diagnose_completion_causes?&completion_causes[size_t(cause)]:nullptr;
    if(diagnostic)++diagnostic->calls;
    if(!renderer){if(diagnostic)++diagnostic->no_renderer_calls;return;}
    const bool had_work=renderer->HasPendingGpuWork();
    if(!had_work&&(!deferred_queries||(!renderer->DeferredQueryCount()&&
       pending_query_publications.empty()&&ready_query_publications.empty()))) {
      if(diagnostic)++diagnostic->no_op_calls;return;
    }
    SceneExecutor::QueueCompletionSnapshot before;
    if(diagnostic) {
      diagnostic->pending_work_calls+=had_work;diagnostic->query_only_calls+=!had_work;
      before=renderer->ObserveQueueCompletion();
    }
    const auto began=std::chrono::steady_clock::now();
    try {
      renderer->FlushGpuWork();
      if(renderer->HasPendingGpuWork())
        throw NativeTransactionFatal("Native completion returned with accepted GPU work still pending");
      // Do this even without draws: a deferred zero-draw end must make
      // progress at CP idle or a guest wait without requiring another frame.
      PublishReadyQueries();
      if(!pending_query_publications.empty()||next_ready_query!=ready_query_publications.size())
        throw NativeTransactionFatal("Native completion left deferred query publications pending");
    }
    catch(const NativeTransactionFatal&) {
      if(diagnostic)RecordCompletionCause(*diagnostic,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(),before,false);
      throw;
    }
    catch(const std::exception& error) {
      if(diagnostic)RecordCompletionCause(*diagnostic,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(),before,false);
      throw NativeTransactionFatal("Accepted native work completion failed: "+std::string(error.what()));
    }
    catch(...) {
      if(diagnostic)RecordCompletionCause(*diagnostic,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(),before,false);
      throw;
    }
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    if(diagnostic)RecordCompletionCause(*diagnostic,elapsed,before,true);
    execution_milliseconds+=elapsed;
    operation_timings[0].milliseconds+=elapsed;
    batch_completion_milliseconds+=elapsed;
    if(had_work)++batch_completion_count;
  }

  // Only SDK/full runtime query pressure may retain exact dirty primary images.
  // The original strong completion method and all of its other callers remain unchanged.
  void CompletePendingForQueryPressure(CompletionCause cause) {
    if(!retain_primary_across_disjoint_queries_enabled){CompletePending(cause);return;}
    if(diagnose_completion_causes)++excluded_retained_query_completion_calls;
    if(!renderer)return;
    const bool had_work=renderer->HasPendingGpuWork();
    if(!had_work&&(!deferred_queries||(!renderer->DeferredQueryCount()&&
       pending_query_publications.empty()&&ready_query_publications.empty())))return;
    const auto began=std::chrono::steady_clock::now();
    try {
      renderer->CompleteGpuWorkForQueryCompletion();
      if(renderer->HasPendingGpuWork())
        throw NativeTransactionFatal("Native completion returned with accepted GPU work still pending");
      // Do this even without draws: a deferred zero-draw end must make
      // progress at CP idle or a guest wait without requiring another frame.
      PublishReadyQueries();
      if(!pending_query_publications.empty()||next_ready_query!=ready_query_publications.size())
        throw NativeTransactionFatal("Native completion left deferred query publications pending");
    }
    catch(const NativeTransactionFatal&) {throw;}
    catch(const std::exception& error) {
      throw NativeTransactionFatal("Accepted native work completion failed: "+std::string(error.what()));
    }
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    execution_milliseconds+=elapsed;
    operation_timings[0].milliseconds+=elapsed;
    batch_completion_milliseconds+=elapsed;
    if(had_work)++batch_completion_count;
  }

  bool SubmitCacheCoherence() {
    // Scene consumes immutable inputs before each Execute returns and retains
    // every GPU resource through its fence. Native memory_copy publishes guest
    // bytes synchronously via ReadPhysical before returning. Cache visibility
    // between GPU operations is explicit in Scene/ResolveMemory barriers.
    // Stopping/failed ownership declines to the unchanged full completion path.
    if(!cache_coherence_waits||stopping.load()||failed.load()||!renderer)return false;
    const auto began=std::chrono::steady_clock::now();
    try {
      renderer->SubmitGpuWork();PublishReadyQueries();
      // Preserve the existing accepted-work attribution. Only this opt-in
      // register callback samples clocks; there is no new per-draw timing.
      const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
      execution_milliseconds+=elapsed;operation_timings[0].milliseconds+=elapsed;
      return true;
    } catch(const NativeTransactionFatal&) {throw;}
    catch(const std::exception& error) {
      throw NativeTransactionFatal("Accepted native coherence progress failed: "+std::string(error.what()));
    }
  }

  bool PrepareImmediateMarkerInputs() {
    // This authority is narrower than a generic GPU-complete event. All
    // preceding live inputs have been copied into Native-owned resources;
    // guest output copies are synchronous and query output is refused here.
    if(!cache_coherence_waits||!ownership||!native_raster||!edram_initialized||
       stopping.load()||failed.load()||coherence_unsafe.load()||!renderer||
       current_query_end||!pending_query_publications.empty()||
       next_ready_query!=ready_query_publications.size()||
       renderer->DeferredQueryCount()||renderer->ActiveQueryMetadata()||
       retained_d32_composition_enabled||retained_fp24_depth_writer_enabled||
       retained_shared_depth_queries_enabled||retain_primary_across_disjoint_queries_enabled||
       REXCVAR_GET(gow3_native_retained_opaque_writer)||REXCVAR_GET(gow3_native_retained_opaque_catalog))return false;
    {
      std::lock_guard lock(pending_mutex);
      if(!deferred_packets.empty())return false;
    }
    if(!SubmitCacheCoherence())return false;
    const auto work=renderer->ObserveAcceptedWork();
    const auto queries=renderer->ObserveMarkerQueryReadiness();
    if(!work.available||!queries.available||queries.active||queries.closed||queries.unknown||
       current_query_end||!pending_query_publications.empty()||
       next_ready_query!=ready_query_publications.size())
      throw NativeTransactionFatal("Native marker submit lost its zero-query owned prefix");
    return true;
  }

  void CompleteQueryPressure(bool sdk) {
    const bool had_gpu_work=renderer&&renderer->HasPendingGpuWork();
    const auto began=std::chrono::steady_clock::now();
    if(retire_query_prefixes&&!sdk) {
      // Runtime logical pressure needs one free ticket, not a drain of newer
      // GPU work. SDK callbacks still require the full completion contract.
      try {
        PublishReadyQueries(true);
        if(pending_query_publications.size()>=query_capacity||renderer->DeferredQueryCount()>=query_capacity) {
          renderer->FlushOldestQuery();PublishReadyQueries();
        }
        if(pending_query_publications.size()>=query_capacity||renderer->DeferredQueryCount()>=query_capacity)
          throw NativeTransactionFatal("Native query pressure did not release an exact oldest result");
      } catch(const NativeTransactionFatal&) {throw;}
      catch(const std::exception& error) {
        throw NativeTransactionFatal("Accepted native query prefix completion failed: "+std::string(error.what()));
      }
    } else CompletePendingForQueryPressure(sdk?CompletionCause::SdkQueryPressure:CompletionCause::RuntimeQueryPressure);
    auto& timing=sdk?sdk_query_pressure:runtime_query_pressure;
    ++timing.drains;timing.gpu_drains+=had_gpu_work;
    timing.milliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
  }

  void LogQueryPressure(uint32_t frame) const {
    REXLOG_INFO("Native query pressure at API frame {}: capacity={}, runtime_drains={}, runtime_gpu_drains={}, runtime_ms={:.3f}, sdk_drains={}, sdk_gpu_drains={}, sdk_ms={:.3f}",
                frame,query_capacity,runtime_query_pressure.drains,runtime_query_pressure.gpu_drains,runtime_query_pressure.milliseconds,
                sdk_query_pressure.drains,sdk_query_pressure.gpu_drains,sdk_query_pressure.milliseconds);
  }

  Processor::NativeDrawDisposition AcceptedDisposition() const {
    return renderer&&renderer->HasPendingGpuWork()?Processor::NativeDrawDisposition::NativePending:
        Processor::NativeDrawDisposition::NativeCompleted;
  }

  // Observation only: immutable operation envelopes are inspected in the
  // exact prefix slice the unchanged loop is about to execute. In particular,
  // query guards describe prefix-start state, not a later queuing decision.
  void RecordCpuWriteCohorts(const LiveOperations& operations,size_t first,size_t end,
                            CpuWriteCohortPrefix kind) noexcept {
    if(!diagnose_cpu_write_cohorts)return;
    auto& stats=cpu_write_cohorts;
    const auto category=size_t(kind);
    stats.Add(stats.prefixes[category]);
    uint64_t run=0,run_bytes=0,run_nulls=0,run_non_nulls=0,run_malformed=0;bool has_writes=false;
    const auto finish_run=[&] {
      if(!run)return;
      stats.Add(stats.cohorts[category]);
      const auto bin=std::min<uint64_t>(run,stats.kMaximumRun);
      stats.Add(stats.run_lengths[bin]);stats.Add(stats.run_members[bin],run);
      stats.Add(stats.run_lengths_by_prefix[category][bin]);
      stats.Add(stats.run_requested_bytes[bin],run_bytes);
      stats.Add(stats.run_null_payloads[bin],run_nulls);stats.Add(stats.run_non_null_payloads[bin],run_non_nulls);
      stats.Add(stats.run_malformed_members[bin],run_malformed);
      stats.maximum_run_length=std::max(stats.maximum_run_length,run);
      if(run>stats.kMaximumRun)stats.Add(stats.oversized_cohorts);
      run=run_bytes=run_nulls=run_non_nulls=run_malformed=0;
    };
    for(size_t index=first;index<end;++index) {
      bool cpu_write=false;
      try {cpu_write=operations.at(index).Kind()=="cpu_memory_write";}
      catch(...) {stats.Add(stats.unknown_operation_kinds);}
      if(!cpu_write){finish_run();continue;}
      ++run;has_writes=true;stats.Add(stats.members[category]);
      try {
        const auto& control=operations.at(index).Control();
        const auto read_u32=[](const nlohmann::json& value) -> uint32_t {
          if(!value.is_number_integer()||
             (value.is_number_unsigned()?value.get<uint64_t>()>UINT32_MAX:
               value.get<int64_t>()<0||uint64_t(value.get<int64_t>())>UINT32_MAX))
            throw std::invalid_argument("Malformed CPU-write census integer");
          return value.get<uint32_t>();
        };
        const uint32_t address=read_u32(control.at("address"));
        const uint32_t bytes=read_u32(control.at("bytes"));
        const bool null_payload=control.at("payload").is_null();
        stats.Add(stats.requested_bytes,bytes);stats.Add(run_bytes,bytes);
        stats.Add(null_payload?run_nulls:run_non_nulls);
        stats.Add(null_payload?stats.null_payloads:stats.non_null_payloads);
        if(!NativeCpuWriteQueueEligible(true,unified_gpu_queue,address,bytes)) {
          stats.Add(stats.bounds_refused);continue;
        }
        stats.Add(stats.bounds_candidates);
        if(!null_payload&&bytes==NativeMemoryCoherence::kPage&&address%NativeMemoryCoherence::kPage==0)
          stats.Add(stats.exact_page_uploads);
        bool overlap=false;
        for(const auto& pending:pending_query_publications)
          overlap|=NativeCpuWriteOverlapsQueryReport(address,bytes,pending.boundary.end_address);
        for(size_t i=next_ready_query;i<ready_query_publications.size();++i)
          overlap|=NativeCpuWriteOverlapsQueryReport(address,bytes,ready_query_publications[i].result.end_address);
        if(current_query_end)
          overlap|=NativeCpuWriteOverlapsQueryReport(address,bytes,current_query_end->boundary.end_address);
        if(overlap)stats.Add(stats.query_report_overlap_at_prefix_start);
      } catch(...) {stats.Add(stats.malformed_members);stats.Add(run_malformed);}
    }
    finish_run();
    if(has_writes)stats.Add(stats.prefixes_with_writes);
  }
  nlohmann::json CpuWriteCohortReport() const {
    const auto& stats=cpu_write_cohorts;
    return {{"schema",1},{"enabled",diagnose_cpu_write_cohorts},{"queue_enabled",queue_cpu_memory_writes},
        {"unified_queue",unified_gpu_queue},{"maximum_histogram_length",stats.kMaximumRun},
        {"histogram_bin_64_includes_larger_runs",true},{"run_length_histogram",stats.run_lengths},
        {"run_members",stats.run_members},{"run_lengths_by_prefix",stats.run_lengths_by_prefix},
        {"run_requested_bytes",stats.run_requested_bytes},{"run_null_payloads",stats.run_null_payloads},
        {"run_non_null_payloads",stats.run_non_null_payloads},
        {"run_malformed_members",stats.run_malformed_members},
        {"prefix_order",{"packet","query","presentation"}},{"prefixes",stats.prefixes},
        {"cohorts",stats.cohorts},{"members",stats.members},{"prefixes_with_writes",stats.prefixes_with_writes},
        {"requested_bytes",stats.requested_bytes},{"null_payloads",stats.null_payloads},
        {"non_null_payloads",stats.non_null_payloads},{"malformed_members",stats.malformed_members},
        {"unknown_operation_kinds",stats.unknown_operation_kinds},{"bounds_candidates",stats.bounds_candidates},
        {"bounds_refused",stats.bounds_refused},{"exact_page_uploads",stats.exact_page_uploads},
        {"query_report_overlap_at_prefix_start",stats.query_report_overlap_at_prefix_start},
        {"oversized_cohorts",stats.oversized_cohorts},{"maximum_run_length",stats.maximum_run_length},
        {"saturated",stats.saturated},
        {"observation_point","before_existing_executor_loop"},
        {"cohort_definition","Consecutive cpu_memory_write operation envelopes inside one actual immutable packet/query/presentation slice; every non-write or slice end terminates a cohort"},
        {"scope","Cumulative prefix-start observations of actual immutable operation slices; not successful execution, upload submissions, queued groups, payload-readability or batching admission certificates"}};
  }

  bool QueueCpuWrite(const LiveOperation& operation) {
    const auto& control=operation.Control();
    ++cpu_write_queue_decisions.considered;
    cpu_write_queue_decisions.null_payloads+=control.at("payload").is_null();
    if(!queue_cpu_memory_writes||!unified_gpu_queue) {++cpu_write_queue_decisions.disabled;return false;}
    const uint32_t address=control.at("address"),count=control.at("bytes");
    if(!NativeCpuWriteQueueEligible(true,true,address,count)) {++cpu_write_queue_decisions.bounds_refused;return false;}
    const auto refuse_report=[&] {++cpu_write_queue_decisions.query_report_refused;return false;};
    // Generic CPU updates do not supersede SDK publication tickets. Keep the
    // exact pre-update completion/publication order for any report overlap,
    // including already-ready results not yet consumed by the guest publisher.
    for(const auto& pending:pending_query_publications)
      if(NativeCpuWriteOverlapsQueryReport(address,count,pending.boundary.end_address))return refuse_report();
    for(size_t i=next_ready_query;i<ready_query_publications.size();++i)
      if(NativeCpuWriteOverlapsQueryReport(address,count,ready_query_publications[i].result.end_address))return refuse_report();
    if(current_query_end&&NativeCpuWriteOverlapsQueryReport(address,count,current_query_end->boundary.end_address))return refuse_report();
    ++cpu_write_queue_decisions.queued;return true;
  }
  void ExecuteOperation(const LiveOperation& operation) {
    // Provider entry points stop new admission. A close request during an
    // already-entered immutable prefix must let that prefix finish: interrupting
    // it here would turn normal shutdown into fallback or a partial-write fatal.
    // SDK shutdown suppresses subsequent dispatch and fences accepted work.
    if (failed) throw std::runtime_error("Native execution failed");
    const auto operation_type=operation.Kind();
    if(REXCVAR_GET(gpu_command_summary))processor->ObserveNativeMarkerOperation(operation_type);
    const bool deferred_query=deferred_queries&&(operation_type=="query_begin"||operation_type=="query_end");
    const bool queued_cpu_write=operation_type=="cpu_memory_write"&&QueueCpuWrite(operation);
    const bool queued_transfer=unified_gpu_queue&&(operation_type=="clearrect"||operation_type=="resolve"||queued_cpu_write);
    if(operation_type!="draw"&&!deferred_query&&!queued_transfer)
      CompletePending(diagnose_completion_causes?CompletionCauseForOperation(operation_type):CompletionCause::Unclassified);
    const auto began = time_cpu_stages?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    const bool copy=native_raster&&operation_type=="memory_copy";
    uint32_t copy_source=0,copy_destination=0,copy_bytes=0;
    uint64_t copy_source_epoch=0,copy_destination_epoch=0;
    if(copy) {
      copy_source=operation.Control().at("source");copy_destination=operation.Control().at("destination");copy_bytes=operation.Control().at("bytes");
      copy_source_epoch=LatestCpuWrite(copy_source,copy_bytes);copy_destination_epoch=LatestCpuWrite(copy_destination,copy_bytes);
      if(!direct&&(copy_source_epoch>active_capture_epoch||copy_destination_epoch>active_capture_epoch))
        throw std::runtime_error("Native memcpy snapshot predates a CPU memory update");
      const auto access=memory->GetPhysicalHeap()->QueryRangeAccess(copy_destination,copy_destination+copy_bytes-1);
      if((int(access)&int(rex::memory::PageAccess::kReadWrite))!=int(rex::memory::PageAccess::kReadWrite))
        throw std::runtime_error("Native memcpy destination is not CPU-readable/writable physical memory");
    }
    if(native_raster) {
      const auto flush_began=time_cpu_stages?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
      // This flush belongs to previously accepted operations. Any failure must
      // stay fatal even though the current operation has not changed its serial.
      try {
        if(const auto* draw=operation.Draw())renderer->PrepareCommittedStateForDraw(*draw);
        else if(deferred_query&&(retained_shared_depth_queries_enabled||retain_primary_across_disjoint_queries_enabled))
          renderer->PrepareCommittedStateForDeferredQuery(operation_type=="query_begin"?NativeDeferredQueryControl::Begin:NativeDeferredQueryControl::End);
        else renderer->FlushCommittedStateAtOperation();
      }
      catch(const std::exception& error) {
        throw NativeTransactionFatal("Cannot flush committed native state: "+std::string(error.what()));
      }
      if(time_cpu_stages)transaction_flush_milliseconds+=std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-flush_began).count();
    }
    const auto serial=renderer->mutation_serial();
    try {
    if(const auto* draw=operation.Draw())renderer->Execute(*draw);
    else if(const auto* query=operation.Query())renderer->Execute(*query);
    else renderer->Execute(operation.Control());
    // The current query end is registered with the SDK only after this
    // operation returns. All other internal drains may publish older ends.
    if(operation_type!="query_end")PublishReadyQueries();
    if(operation_type=="cpu_memory_write") {
      const uint32_t address=operation.Control().at("address"),bytes=operation.Control().at("bytes");
      for(size_t page=address/NativeMemoryCoherence::kPage;page<=(uint64_t(address)+bytes-1)/NativeMemoryCoherence::kPage;++page)
        executed_write_epochs[page]=std::max(executed_write_epochs[page],operation.Control().value("coherence_epoch",uint64_t{0}));
    }
    // Direct execution copied CPU-sourced bytes at the API point; writing the
    // GPU result back now could overwrite newer CPU writes.
    if(copy&&direct&&operation.Control().at("source_kind")=="cpu_snapshot") {
    } else if(copy&&direct) {
      const auto data=renderer->ReadPhysical(copy_destination,copy_bytes);
      std::memcpy(memory->TranslatePhysical(copy_destination),data.data(),data.size());
    } else if(copy) {
      const auto data=renderer->ReadPhysical(copy_destination,copy_bytes);
      if(LatestCpuWrite(copy_source,copy_bytes)!=copy_source_epoch||LatestCpuWrite(copy_destination,copy_bytes)!=copy_destination_epoch)
        throw NativeTransactionFatal("CPU changed native memcpy memory during GPU execution");
      std::memcpy(memory->TranslatePhysical(copy_destination),data.data(),data.size());
      if(LatestCpuWrite(copy_source,copy_bytes)!=copy_source_epoch||LatestCpuWrite(copy_destination,copy_bytes)!=copy_destination_epoch)
        throw NativeTransactionFatal("CPU changed native memcpy memory during guest publication");
    }
    } catch(const NativeBatchCompletionError& error) {
      throw NativeTransactionFatal("Accepted native batch failed during an operation: "+std::string(error.what()));
    } catch(const NativeTransactionFatal&) {throw;}
      catch (const std::exception& error) {
      SaveExecutionFailure(operation,error.what());
      if(native_raster&&renderer->mutation_serial()!=serial)
        throw NativeTransactionFatal("Native operation changed authoritative storage before failing: "+std::string(error.what()));
      throw std::runtime_error("event " + std::to_string(operation.Event()) +
          " (" + std::string(operation_type) + "): " + error.what());
    }
    const double milliseconds=time_cpu_stages?std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-began).count():0;
    if(time_cpu_stages)execution_milliseconds+=milliseconds;
    const auto type=operation_type;
    const size_t timing=type=="draw"?0:type=="resolve"?1:type=="clearrect"?2:
        type=="cpu_memory_write"?3:(type=="query_begin"||type=="query_end")?4:
        type=="present"?5:6;
    ++operation_timings[timing].count;
    if(time_cpu_stages)operation_timings[timing].milliseconds+=milliseconds;
  }

  void RecordPresentationInterval(uint32_t frame,size_t operations) {
    const auto now=std::chrono::steady_clock::now();
    if(frame_journal.Active()) {
      NativeFrameJournal::Row row;
      row.ordinal=completed_frames.load(std::memory_order_relaxed)+1;row.api_frame=frame;
      row.steady_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
      row.interval_valid=previous_presentation.has_value();
      if(previous_presentation)row.interval_ms=std::chrono::duration<double,std::milli>(now-*previous_presentation).count();
      row.operations=operations;
      for(size_t i=0;i<row.operation_counts.size();++i)row.operation_counts[i]=operation_timings[i].count;
      row.batch_completion_count=batch_completion_count;row.batch_completion_ms=batch_completion_milliseconds;
      row.runtime_pressure={runtime_query_pressure.drains,runtime_query_pressure.gpu_drains,runtime_query_pressure.milliseconds};
      row.sdk_pressure={sdk_query_pressure.drains,sdk_query_pressure.gpu_drains,sdk_query_pressure.milliseconds};
      const auto scene=renderer->ObserveCompletedFrame(frame);
      row.scene={scene.available,scene.frame,scene.draws,scene.completion_ms,
          scene.cpu_uploads,scene.cpu_payload_bytes,scene.gpu_overlay_bytes,
          scene.overlay_copy_regions,scene.overlay_readback_bytes,scene.gpu_alias_loads,
          scene.queue_available,scene.queue_submissions,scene.queue_waits,scene.queue_polls,scene.queue_poll_retirements};
      const auto query_wait=Gow3GetQueryWaitStats();
      row.query_wait_ms=double(query_wait.nanoseconds-previous_journal_query_wait.nanoseconds)/1e6;
      row.query_waits=query_wait.waits-previous_journal_query_wait.waits;
      if(const auto results=query_wait.direct_results-previous_journal_query_wait.direct_results)
        row.direct_latency_us=double(query_wait.direct_latency_ns-previous_journal_query_wait.direct_latency_ns)/1e3/double(results);
      previous_journal_query_wait=query_wait;
      frame_journal.Record(row);
    }
    if(previous_presentation) {
      if(presentation_intervals.empty())presentation_interval_first_frame=frame;
      presentation_intervals.push_back(std::chrono::duration<double,std::milli>(now-*previous_presentation).count());
      if(presentation_intervals.size()==120) {
        frame_journal.MarkReport(NativeFrameJournal::IntervalSummary);
        auto ordered=presentation_intervals;
        std::sort(ordered.begin(),ordered.end());
        const auto percentile=[&](size_t percent) {return ordered[(ordered.size()*percent+99)/100-1];};
        const auto query_wait=Gow3GetQueryWaitStats();
        const double window_frames=double(ordered.size());
        const double query_wait_ms=double(query_wait.nanoseconds-previous_query_wait.nanoseconds)/1e6/window_frames;
        const double query_waits=double(query_wait.waits-previous_query_wait.waits)/window_frames;
        const auto direct_results=query_wait.direct_results-previous_query_wait.direct_results;
        const auto direct_waits=query_wait.direct_waits-previous_query_wait.direct_waits;
        const double direct_latency_us=direct_results?double(query_wait.direct_latency_ns-previous_query_wait.direct_latency_ns)/1e3/double(direct_results):0.;
        const double direct_backlog=direct_waits?double(query_wait.direct_backlog-previous_query_wait.direct_backlog)/double(direct_waits):0.;
        previous_query_wait=query_wait;
        REXLOG_INFO("Native frame intervals API {}..{}: samples={}, median={:.2f} ms, p95={:.2f} ms, p99={:.2f} ms, max={:.2f} ms, over50ms={}, over100ms={}, final_operations={}, query_wait_ms_per_frame={:.2f}, query_waits_per_frame={:.1f}, direct_result_latency_us={:.0f}, direct_backlog_at_wait={:.1f}",
            presentation_interval_first_frame,frame,ordered.size(),percentile(50),percentile(95),percentile(99),ordered.back(),
            std::count_if(ordered.begin(),ordered.end(),[](double value){return value>50.;}),
            std::count_if(ordered.begin(),ordered.end(),[](double value){return value>100.;}),operations,
            query_wait_ms,query_waits,direct_latency_us,direct_backlog);
        presentation_intervals.clear();
      }
    }
    previous_presentation=now;
  }

  bool HasCommittedPacketSpan() {
    std::lock_guard lock(pending_mutex);
    return packet_ownership.HasCommittedSpan();
  }

  [[noreturn]] void StopForFailure(uint32_t frame,const std::string& reason) {
    // Quiesce capture first, retaining every accepted immutable owner. This is
    // terminal completion, never an export/import or a change of scene owner.
    Fail(frame,reason);
    processor->ReportNativeSceneFailure(reason);
    StopShaderDiscovery();
    Gow3SetGraphicsBoundarySink({});
    try { CompletePending(CompletionCause::FailureCleanup); }
    catch(const std::exception& error) {
      throw NativeTransactionFatal("Native renderer stopped at API frame "+std::to_string(frame)+
          ": "+reason+"; accepted GPU completion also failed: "+error.what()+
          ". Automatic fallback is disabled.");
    }
    throw NativeTransactionFatal("Native renderer stopped at API frame "+std::to_string(frame)+
        ": "+reason+". Automatic fallback is disabled; select Xenia and restart to use it.");
  }

  void EnsureEdramInitialized() {
    if(edram_initialized)return;
    Initialize();
    if(!processor->InitializeNativeEdram([&](VkBuffer source) {renderer->InitializeEdramFromBuffer(source);}))
      throw std::runtime_error("Cannot initialize native EDRAM at the first ordered draw boundary");
    edram_initialized=true;
    REXLOG_INFO("Native EDRAM initialized from pristine shared host storage before the first draw packet");
  }

  // Direct execution, on the CP thread in API order: every operation up to and
  // including this query end, draws included. The result publishes when its
  // fence retires (PublishReadyQueries), never by draining here.
  void DirectQuery(LiveQueryPrefix prefix,uint64_t captured_write_epoch) {
    if(stopping||failed)return;
    const uint32_t frame_number=prefix.batch->frame;
    try {
      EnsureEdramInitialized();
      DrainDeferredPackets(frame_number,prefix.first_operation);
      BeginExecution(std::move(prefix.batch));
      if(prefix.first_operation!=executed_operations||prefix.operation_end<prefix.first_operation||
         prefix.operation_end-prefix.first_operation!=active_frame->operations.size()||active_frame->operations.empty())
        throw std::runtime_error("Direct query prefix is missing or repeats ordered operations");
      if(!prefix.generation) {
        // A plain batch: no query end to register.
        active_capture_epoch=captured_write_epoch;
        RecordCpuWriteCohorts(active_frame->operations,0,active_frame->operations.size(),CpuWriteCohortPrefix::Packet);
        for(const auto& operation:active_frame->operations)ExecuteOperation(operation);
        executed_operations=prefix.operation_end;
        active_frame.reset();
        return;
      }
      const auto& end=active_frame->operations.back();
      if(end.Kind()!="query_end"||end.Frame()!=frame_number)
        throw std::runtime_error("Direct query prefix does not end at its query");
      const Processor::NativeQueryBoundary boundary{prefix.end_address,prefix.generation,frame_number,true};
      current_query_end=PendingQuery{boundary,frame_number,end.Event()};
      if(pending_query_publications.size()>=query_capacity||renderer->DeferredQueryCount()>=query_capacity)
        CompleteQueryPressure(false);
      active_capture_epoch=captured_write_epoch;
      RecordCpuWriteCohorts(active_frame->operations,0,active_frame->operations.size(),CpuWriteCohortPrefix::Query);
      for(const auto& operation:active_frame->operations)ExecuteOperation(operation);
      executed_operations=prefix.operation_end;
      latest_executed_query_generation=prefix.generation;
      pending_query_publications.push_back(*current_query_end);
      current_query_end.reset();
      peak_deferred_queries=std::max(peak_deferred_queries,pending_query_publications.size());
      ++deferred_query_prefixes;
      PublishReadyQueries(true);
      active_frame.reset();
    } catch(const std::exception& error) {
      current_query_end.reset();
      Fail(frame_number,std::string("Direct execution: ")+error.what());
    }
  }

  // CP idle hook: publish retired query results. True while GPU work remains.
  bool DirectIdle() {
    if(stopping||failed||!renderer)return false;
    try {
      // The guest may be blocked on a query in a partly filled batch that
      // nothing else will fill. Submit when it is waiting, or after a short
      // idle stretch; otherwise keep batches whole.
      if(Gow3TakeDirectQueryWaiting()||++direct_idle_polls>=64) {
        direct_idle_polls=0;
        renderer->SubmitGpuWork();
      }
      PublishReadyQueries(true);
      return renderer->HasPendingGpuWork()||!pending_query_publications.empty();
    } catch(const std::exception& error) {
      Fail(executing_frame,std::string("Direct execution: ")+error.what());
      return false;
    }
  }

  void ExecutePacketBatch(LivePacketPrefix prefix) {
    BeginExecution(std::move(prefix.batch));
    if(prefix.first_operation!=executed_operations||prefix.operation_end<prefix.first_operation||
       prefix.operation_end-prefix.first_operation!=active_frame->operations.size())
      throw std::runtime_error("Native packet prefix is missing or repeats ordered operations");
    RecordCpuWriteCohorts(active_frame->operations,0,active_frame->operations.size(),CpuWriteCohortPrefix::Packet);
    for(const auto& operation:active_frame->operations)ExecuteOperation(operation);
    executed_operations=prefix.operation_end;
    active_frame.reset();
  }

  void DrainDeferredPackets(uint32_t frame,size_t end) {
    for(;;) {
      std::optional<LivePacketPrefix> prefix;
      {
        std::lock_guard lock(pending_mutex);
        if(deferred_packets.empty())break;
        if(deferred_packets.front().batch->frame!=frame||deferred_packets.front().operation_end>end)
          throw std::runtime_error("Deferred native packet crossed its ordered boundary");
        prefix=std::move(deferred_packets.front());deferred_packets.pop_front();
      }
      ExecutePacketBatch(std::move(*prefix));
    }
  }

  Processor::NativeDrawDisposition DrawBoundary(const Processor::NativeDrawBoundary& boundary) {
    using Disposition=Processor::NativeDrawDisposition;
    if(stopping)return Disposition::NativeCompleted; // SDK terminal shutdown admits no new work.
    // Direct execution renders every captured API call itself. Packets that the
    // guest submitted before entering its GPU-less mode are already covered.
    if(direct) {
      if(!processor->PrepareNativeDrawSubmission())
        StopForFailure(executing_frame,"Cannot retire a pre-direct draw packet");
      return Disposition::NativeCompleted;
    }
    // One API effect may emit multiple draw packets. Once that whole effect
    // completed natively, every remaining packet must stay suppressed even if
    // the producer has since failed or a diagnostic fatal error was requested.
    if(HasCommittedPacketSpan()) {
      try {
        std::optional<PacketOwnership::Match> remaining;
        {
          std::lock_guard lock(pending_mutex);
          remaining=packet_ownership.InspectExecuting(boundary.command_address,boundary.packet_bytes);
          if(!boundary.predicate_passed||!remaining||!remaining->native_committed||pending_packets.empty()||
             pending_packets.front().prefix.span.generation!=remaining->generation)
            throw std::runtime_error("Committed native draw span no longer matches the command stream");
        }
        // SDK resets this per-packet capability even though the native batch
        // has already completed. It still must fence prior reference work.
        if(!processor->PrepareNativeDrawSubmission())
          throw std::runtime_error("Cannot prepare remaining committed native draw packet");
        std::lock_guard lock(pending_mutex);
        packet_ownership.Consume(*remaining);
        ++matched_draw_packets;
        if(remaining->last_draw)pending_packets.pop_front();
        return AcceptedDisposition();
      }catch(const std::exception& error) {
        StopForFailure(executing_frame,"Cannot retire already completed native draw span: "+std::string(error.what()));
      }
    }
    if(failed) StopForFailure(executing_frame,"Native API capture stopped");
    std::optional<PacketOwnership::Match> match;
    QueuedPacket packet;
    bool current_packet_accepted=false;
    uint32_t frame_number=UINT32_MAX;
    try {
      EnsureEdramInitialized();
      if(native_raster) {
        const auto packet_limit=REXCVAR_GET(gow3_native_test_fallback_after_packets);
        const auto query_limit=REXCVAR_GET(gow3_native_test_fallback_query_draws);
        const auto query=renderer->ActiveQueryMetadata();
        const bool before_query=REXCVAR_GET(gow3_native_test_fallback_before_query_draw)&&
            processor->ActiveNativeQuery()&&(!query||!query->draws);
        if((packet_limit>0&&native_raster_packets>=uint64_t(packet_limit))||
           (query_limit>0&&query&&query->draws>=uint64_t(query_limit))||before_query) {
          StopForFailure(executing_frame==UINT32_MAX?output_frame:executing_frame,"Explicit diagnostic native fatal-error trigger");
        }
      }
      {
        std::lock_guard lock(pending_mutex);
        if(!boundary.predicate_passed)
          throw std::runtime_error("Native draw callback was reached without a passed packet predicate");
        match=packet_ownership.InspectExecuting(boundary.command_address,boundary.packet_bytes);
        if(!match)++unmatched_draw_packets;
        else if(pending_packets.empty()||pending_packets.front().prefix.span.generation!=match->generation)
          throw std::runtime_error("Native packet ownership has no matching operation prefix");
        if(match)packet=pending_packets.front();
      }
      if(!match) {
        SaveUnmatchedPacket(boundary);
        if(boundary.proven_no_effect) {++proven_no_effect_packets;return Disposition::ProvenNoEffect;}
        StopForFailure(executing_frame,"Draw packet has no exact native ownership span");
      }
      ++matched_draw_packets;
      frame_number=packet.prefix.batch->frame;
      if(!match->draw_index) {
        span_disposition=Disposition::ReferenceCoherent;
        const auto& operations=packet.prefix.batch->operations;
        if(native_raster) {
          if(!match->replacement_safe)
            throw std::runtime_error("Native packet span contains an unsupported control dependency");
          if(processor->ActiveNativeQuery()&&std::any_of(operations.begin(),operations.end(),[](const auto& operation) {
                return operation.Kind()=="clearrect";
              }))throw std::runtime_error("Native clear raster sample counting inside an active query is not implemented");
          if(!processor->PrepareNativeDrawSubmission())
            throw std::runtime_error("Reference query prevents native packet ownership");
          if(!operations.empty()&&operations.back().Kind()=="memory_copy") {
            const auto actual=DecodeMemoryExportPacket(boundary.vertex_shader_words,boundary.vertex_fetch95,
                boundary.export_constants,boundary.clip_control,boundary.vte_control,boundary.index_offset,boundary.index_count);
            const auto& operation=operations.back().Control();
            if(boundary.primitive!=1||boundary.index_address||actual.source!=operation.at("source").get<uint32_t>()||
               actual.destination!=operation.at("destination").get<uint32_t>()||actual.bytes!=operation.at("bytes").get<uint32_t>())
              throw std::runtime_error("Native memcpy packet state differs from its captured API operation");
          }
          DrainDeferredPackets(frame_number,packet.prefix.first_operation);
          active_capture_epoch=packet.captured_write_epoch;
          const auto prefix_serial=renderer->mutation_serial();
          try {ExecutePacketBatch(packet.prefix);}
          catch(const NativeTransactionFatal&) {throw;}
          catch(const std::exception& error) {
            if(renderer->mutation_serial()!=prefix_serial)
              throw NativeTransactionFatal("Owned API span accepted native writes before failing: "+std::string(error.what()));
            throw;
          }
          span_disposition=AcceptedDisposition();
          ++native_raster_packets;
        }

      }
      current_packet_accepted=span_disposition==Disposition::NativeCompleted||span_disposition==Disposition::NativePending;
      {
        std::lock_guard lock(pending_mutex);
        if(current_packet_accepted)packet_ownership.CommitSpan(*match);
        packet_ownership.Consume(*match);
        if(match->last_draw) {
          if(span_disposition==Disposition::ReferenceCoherent)
            deferred_packets.push_back(std::move(pending_packets.front().prefix));
          pending_packets.pop_front();
        }
      }
      if(native_raster&&current_packet_accepted&&match->last_draw) {
        const auto query_limit=REXCVAR_GET(gow3_native_test_fallback_query_draws);
        const auto query=renderer->ActiveQueryMetadata();
        if(query_limit>0&&query&&query->draws>=uint64_t(query_limit)) {
          StopForFailure(frame_number,"Explicit diagnostic fatal error after an accepted native query draw");
          span_disposition=Disposition::NativeCompleted;
        }
      }
      return span_disposition;
    } catch(const std::exception& error) {
      if(match)SaveFailedPacket(boundary,packet,*match,error.what());
      StopForFailure(frame_number,error.what());
    }
  }

  void QueryBoundary(const Processor::NativeQueryBoundary& boundary) {
    if(stopping)return;
    if(direct) {
      // A pre-GPU-less packet: GetData uses direct results, so release the
      // SDK ticket with a visible count.
      try {processor->PublishNativeQueryResult(boundary,1);}catch(const std::exception&) {}
      return;
    }
    if(failed) {
      StopForFailure(executing_frame,"Native API capture stopped before query completion");
      return;
    }
    uint32_t frame_number = UINT32_MAX;
    struct EndScope {
      std::optional<PendingQuery>& current;
      ~EndScope(){current.reset();}
    } end_scope{current_query_end};
    try {
      LiveQueryPrefix prefix;
      {
        std::lock_guard lock(pending_mutex);
        if (pending_prefixes.empty())
          throw std::runtime_error("Native query packet has no captured API prefix");
        prefix = std::move(pending_prefixes.front());
        pending_prefixes.pop_front();
      }
      frame_number = prefix.batch->frame;
      if (prefix.end_address != boundary.end_address || prefix.generation != boundary.generation)
        throw std::runtime_error("Native query packet does not match its API generation/address");
      DrainDeferredPackets(frame_number,prefix.first_operation);
      BeginExecution(std::move(prefix.batch));
      if (prefix.first_operation != executed_operations || prefix.operation_end < prefix.first_operation ||
          prefix.operation_end - prefix.first_operation != active_frame->operations.size())
        throw std::runtime_error("Native query prefix is missing or repeats ordered operations");
      if(active_frame->operations.empty())throw std::runtime_error("Native query prefix has no end operation");
      const auto& end=active_frame->operations.back();
      if(end.Kind()!="query_end"||end.Frame()!=frame_number||
         end.QueryReportAddress()!=boundary.end_address||end.QueryGeneration()!=boundary.generation)
        throw std::runtime_error("Native query prefix does not end at its exact API generation/address");
      current_query_end=PendingQuery{boundary,frame_number,end.Event()};
      if(deferred_queries&&boundary.native_authority&&
         (pending_query_publications.size()>=query_capacity||renderer->DeferredQueryCount()>=query_capacity))CompleteQueryPressure(false);
      RecordCpuWriteCohorts(active_frame->operations,0,active_frame->operations.size(),CpuWriteCohortPrefix::Query);
      for (const auto& operation : active_frame->operations) {
        if(native_raster) {
          const auto type=operation.Kind();
          if(type!="query_begin"&&type!="query_end"&&type!="cpu_memory_write")
            throw std::runtime_error("Native query boundary contains an operation without owned draw packets: "+std::string(type));
        }
        ExecuteOperation(operation);
      }
      executed_operations = prefix.operation_end;
      latest_executed_query_generation=boundary.generation;
      if(deferred_queries&&boundary.native_authority) {
        if(!native_raster)throw NativeTransactionFatal("Deferred query elected without native raster ownership");
        if(pending_query_publications.size()>=query_capacity)
          throw NativeTransactionFatal("Deferred query publication capacity was not drained");
        // Register both identities before any full-drain publication. Sealed
        // Scene records contain real results only after their covering fence.
        pending_query_publications.push_back(*current_query_end);
        try {processor->DeferNativeQueryResult(boundary);}
        catch(...) {pending_query_publications.pop_back();throw;}
        peak_deferred_queries=std::max(peak_deferred_queries,pending_query_publications.size());
        ++deferred_query_prefixes;
        const auto force=REXCVAR_GET(gow3_native_test_fallback_after_deferred_queries);
        if(force>0&&deferred_query_prefixes>=uint64_t(force)) {
          StopForFailure(frame_number,"Diagnostic fatal error after deferred query end");
          active_frame.reset();return;
        }
        PublishReadyQueries(true);
        active_frame.reset();return;
      }
      CompletePending(CompletionCause::ImmediateQueryBoundary);
      const auto& results = renderer->report().at("native_occlusion_results");
      if (results.empty()) throw std::runtime_error("Native query prefix produced no query result");
      const auto& result = results.back();
      if (result.at("frame") != frame_number || result.at("end_address") != boundary.end_address ||
          result.at("generation") != boundary.generation ||
          result.at("end_event") != active_frame->operations.back().Event())
        throw std::runtime_error("Native query did not complete at its ordered packet boundary");
      if(boundary.native_authority) {
        if(!native_raster)throw NativeTransactionFatal("Native query elected without native raster ownership");
        processor->PublishNativeQueryResult(boundary,result.at("samples").get<uint64_t>());
        LogQueryPublication(*current_query_end,result.at("samples").get<uint64_t>());
        active_frame.reset();
        return;
      }
      throw NativeTransactionFatal("Native query boundary lost exclusive query authority");
    } catch (const std::exception& error) {
      StopForFailure(frame_number,error.what());
    }
  }

  std::optional<NativeFrameCaptureSnapshot> SnapshotNativeFrame() {
    // GPU failures propagate to the normal fatal path. Only unsupported capture
    // extents decline locally; no unknown bytes are padded or treated as owned.
    const auto state=renderer->ExportState();
    uint64_t total=0,last=0;
    for(const auto& [address,bytes]:state.physical.ranges) {
      if(!bytes||((address|bytes)&3u)||address<last||uint64_t(address)+bytes>512u*1024*1024) {
        REXLOG_ERROR("Native capture refused an unaligned or invalid physical ownership range");
        return std::nullopt;
      }
      total+=bytes;last=uint64_t(address)+bytes;
    }
    if(total>512u*1024*1024) return std::nullopt;
    NativeFrameCaptureSnapshot snapshot;
    snapshot.edram=renderer->ReadFullEdram();
    snapshot.physical.reserve(state.physical.ranges.size());
    for(const auto& [address,bytes]:state.physical.ranges)
      snapshot.physical.push_back({address,renderer->ReadPhysical(address,bytes)});
    return snapshot;
  }

  void CaptureCompletedNativeFrame() {
    if(frame_capture_directory.empty()||frame_capture_finished)return;
    const uint32_t frame=active_frame->frame;
    if(!frame_capture_writer) {
      if(frame>=UINT32_MAX-2||renderer->ActiveQueryMetadata()||renderer->DeferredQueryCount())return;
      if(!frame_capture_trigger.empty()) {
        std::error_code error;
        if(!std::filesystem::is_regular_file(frame_capture_trigger,error))return;
      }
      if(!frame_capture_pixel.empty()) {
        bool used=false;
        for(const auto& operation:active_frame->operations)
          if(const auto* draw=operation.Draw();draw&&draw->pixel_shader&&
              draw->pixel_shader->hash.View()==frame_capture_pixel) {used=true;break;}
        if(!used)return;
      }
      const auto initial=SnapshotNativeFrame();
      if(!initial){frame_capture_finished=true;return;}
      try {
        frame_capture_first=frame+1;
        frame_capture_writer=std::make_unique<NativeFrameCaptureWriter>(
            frame_capture_directory,frame_capture_first,*initial,nlohmann::json{
              {"renderer","native"},{"boundary","after_completed_present_before_next_frame"},
              {"trigger_pixel",frame_capture_pixel},{"runtime_report",renderer->report()}});
        REXLOG_INFO("Native frame capture armed for API frames {}..{} in {}",
            frame_capture_first,frame_capture_first+1,frame_capture_directory.string());
      } catch(const std::exception& error) {
        frame_capture_finished=true;
        REXLOG_ERROR("Native frame capture could not start: {}",error.what());
      }
      return;
    }
    if(frame!=frame_capture_first+frame_capture_count||renderer->ActiveQueryMetadata()||renderer->DeferredQueryCount()) {
      frame_capture_finished=true;frame_capture_writer.reset();
      REXLOG_ERROR("Native frame capture refused a frame gap or unfinished query");
      return;
    }
    const auto completed=SnapshotNativeFrame();
    if(!completed){frame_capture_finished=true;frame_capture_writer.reset();return;}
    try {
      auto results=nlohmann::json::array();
      for(const auto& result:renderer->report().at("native_occlusion_results"))
        if(result.at("frame")==frame)results.push_back(result);
      frame_capture_writer->AppendCompletedFrame(*active_frame,*completed,results);
      if(++frame_capture_count==2) {
        frame_capture_writer->Finish();frame_capture_finished=true;frame_capture_writer.reset();
        REXLOG_INFO("Native frame capture complete: {}",frame_capture_directory.string());
      }
    } catch(const std::exception& error) {
      frame_capture_finished=true;frame_capture_writer.reset();
      REXLOG_ERROR("Native frame capture incomplete: {}",error.what());
    }
  }

  std::optional<Processor::NativeSwapSource> Present(uint32_t frame, uint32_t, uint32_t width, uint32_t height) {
    if(stopping)return std::nullopt;
    if(failed) {
      StopForFailure(frame,"Native API capture stopped before presentation");
      return std::nullopt;
    }
    if (frame == UINT32_MAX) StopForFailure(frame,"Swap packet has no native API frame identity");
    try {
      if(direct)EnsureEdramInitialized();
      // Duplicate swaps for an already rendered API frame may share its
      // immutable snapshot. A new frame must be present in the bounded queue.
      while (output_frame != frame) {
        {
          std::lock_guard lock(pending_mutex);
          if (pending.empty() || pending.front()->frame > frame)
            throw std::runtime_error("No API frame matches the GPU swap marker");
          active_frame = std::move(pending.front());
          pending.pop_front();
        }
        pending_changed.notify_all();
        BeginExecution(std::move(active_frame));
        if (executed_operations > active_frame->operations.size())
          throw std::runtime_error("Native query prefix extends beyond its completed API frame");
        RecordCpuWriteCohorts(active_frame->operations,executed_operations,active_frame->operations.size(),CpuWriteCohortPrefix::Presentation);
        for (size_t index = executed_operations; index < active_frame->operations.size(); ++index) {
          if(native_raster&&!direct) {
            const auto type=active_frame->operations.at(index).Kind();
            if(type!="present"&&type!="cpu_memory_write")
              throw std::runtime_error("Native presentation contains an operation without owned draw packets: "+std::string(type));
          }
          ExecuteOperation(active_frame->operations.at(index));
        }
        {
          std::lock_guard lock(pending_mutex);
          if(!pending_packets.empty()&&pending_packets.front().prefix.batch->frame<=active_frame->frame)
            throw std::runtime_error("Native frame finished with unmatched draw packet ownership");
          while(!deferred_packets.empty()&&deferred_packets.front().batch->frame<=active_frame->frame)
            deferred_packets.pop_front();
        }
        CompletePending(CompletionCause::PresentationTail);
        if (!output || output_frame != active_frame->frame)
          throw std::runtime_error("Native frame did not produce a complete frontbuffer");
        if(!native_raster)processor->PollNativeComparisonResults();
        const nlohmann::json empty_query_report{{"native_occlusion_results",nlohmann::json::array()}};
        const auto comparison = CompareNativeQueries(native_raster?empty_query_report:renderer->report(), active_frame->frame,
            [this](uint32_t address) -> std::optional<ReferenceQuerySample> {
              const auto reference = processor->ReadNativeComparisonResult(address);
              if (!reference) return std::nullopt;
              return ReferenceQuerySample{reference->samples, reference->submission,
                                          reference->guest_samples, reference->skipped_async_draws};
            });
        SaveQueryMismatch(comparison);
        RecordPresentationInterval(output_frame,active_frame->operations.size());
        last_completed_api_frame = output_frame;
        const auto completed = ++completed_frames;
        const bool initial_complex_frame = active_frame->operations.size() > 4 && ++complex_frames <= 3;
        // A slow first campaign frame must not hide behind a 120-frame sampling
        // interval. Keep this bounded so long gameplay sessions stay quiet.
        const bool initial_large_frame = active_frame->operations.size() > 512 && ++large_frames <= 12;
        const bool frame_report_due=completed <= 3 || completed % 120 == 0 || initial_complex_frame || initial_large_frame;
        const bool verbose_frame_report_due=frame_report_due&&verbose_frame_reports;
        if(verbose_frame_report_due)frame_journal.MarkReport(NativeFrameJournal::FrameDetails);
        if (verbose_frame_report_due) {
          if(time_cpu_stages) {
            REXLOG_INFO("Native API frame {} completed {} operations in {:.1f} ms; {} presentation",
                        output_frame, active_frame->operations.size(), execution_milliseconds, preview ? "native" : "reference");
            REXLOG_INFO("Native operation timings at API frame {} (count/ms): draw={}/{:.1f}, resolve={}/{:.1f}, clear={}/{:.1f}, cpu_write={}/{:.1f}, query={}/{:.1f}, present={}/{:.1f}, other={}/{:.1f}; transaction_flush={:.1f} ms included",
                        output_frame,operation_timings[0].count,operation_timings[0].milliseconds,
                        operation_timings[1].count,operation_timings[1].milliseconds,
                        operation_timings[2].count,operation_timings[2].milliseconds,
                        operation_timings[3].count,operation_timings[3].milliseconds,
                        operation_timings[4].count,operation_timings[4].milliseconds,
                        operation_timings[5].count,operation_timings[5].milliseconds,
                        operation_timings[6].count,operation_timings[6].milliseconds,transaction_flush_milliseconds);
          } else {
            REXLOG_INFO("Native API frame {} completed {} operations; CPU stage timings disabled; {} presentation",
                        output_frame,active_frame->operations.size(),preview?"native":"reference");
            REXLOG_INFO("Native operation counts at API frame {} (CPU stage timings disabled): draw={}, resolve={}, clear={}, cpu_write={}, query={}, present={}, other={}",
                        output_frame,operation_timings[0].count,operation_timings[1].count,operation_timings[2].count,
                        operation_timings[3].count,operation_timings[4].count,operation_timings[5].count,operation_timings[6].count);
          }
          if(time_cpu_stages)
            REXLOG_INFO("Native explicit batch completion at API frame {}: {} drains, {:.1f} ms included in draw timing",output_frame,batch_completion_count,batch_completion_milliseconds);
          else
            REXLOG_INFO("Native explicit batch completion at API frame {}: {} drains, {:.1f} ms; completion timer retained",output_frame,batch_completion_count,batch_completion_milliseconds);
          LogQueryPressure(output_frame);
          LogQueryReadiness("sample");
          REXLOG_INFO("Native state identity mode at API frame {}: {}",output_frame,planner.LazyStateMode());
          const auto handoffs=planner.ResourceHandoffStats();
          REXLOG_INFO("Native producer_resource_handoffs: {}",nlohmann::json({
              {"enabled",handoffs.enabled},{"handoffs",handoffs.handoffs},
              {"scope","Batched successful typed producer digest/owner copies; not GPU accepted or retired draw counts"}}).dump());
          if(const auto timings=renderer->report().find("draw_timings");timings!=renderer->report().end())
            REXLOG_INFO("Native draw stages: {}",timings->dump());
          if(diagnose_cpu_write_cohorts)
            REXLOG_INFO("Native cpu_write_cohorts: {}",CpuWriteCohortReport().dump());
          if(diagnose_completion_causes)
            REXLOG_INFO("Native completion_causes: {}",CompletionCauseReport().dump());
          REXLOG_INFO("Native cpu_write_queue_decisions: {}",nlohmann::json({
              {"enabled",queue_cpu_memory_writes},{"considered",cpu_write_queue_decisions.considered},
              {"queued",cpu_write_queue_decisions.queued},{"disabled",cpu_write_queue_decisions.disabled},
              {"bounds_refused",cpu_write_queue_decisions.bounds_refused},
              {"query_report_refused",cpu_write_queue_decisions.query_report_refused},
              {"null_payloads",cpu_write_queue_decisions.null_payloads}}).dump());
          LogSceneCounters();
          if(ownership) {
            std::lock_guard lock(pending_mutex);
            REXLOG_INFO("Native packet coverage at API frame {}: {} matched, {} unmatched, {} copy replacements, {} raster packets, {} proven no-effect; {} pending spans, {} deferred prefixes",
                        output_frame,matched_draw_packets,unmatched_draw_packets,replaced_copy_packets,
                        native_raster_packets,proven_no_effect_packets,pending_packets.size(),deferred_packets.size());
          }
        } else if(frame_report_due) {
          // Preserve sparse readiness observation in quiet mode without Scene JSON.
          LogQueryReadiness("sample");
        }
        if (comparison.at("queries").get<uint64_t>() && (++query_frames <= 3 || query_frames % 120 == 0))
          REXLOG_INFO("Native query comparison: {}", comparison.dump());
        executing_frame = UINT32_MAX;
        executed_operations = 0;
        // All ordered prefixes, GPU completion, reporting and diagnostics for
        // this full frame are finished. Only immutable CPU data moves to the
        // bounded disposal worker; graphics resources remain on their owner.
        CaptureCompletedNativeFrame();
        completed_frame_disposer.TryRetire(active_frame);
        active_frame.reset();
      }
      if (stopping || failed) return std::nullopt;
      if (!preview) { output.reset(); return std::nullopt; }
      if (!output) throw std::runtime_error("Native presentation image is absent");
      auto& texture = static_cast<plume::VulkanTexture&>(*output);
      if (texture.desc.width != width || texture.desc.height != height)
        throw std::runtime_error("Native frontbuffer dimensions do not match the host swap");
      struct RetainedImage {
        std::shared_ptr<RuntimeState> owner;
        std::shared_ptr<plume::RenderTexture> image;
      };
      Processor::NativeSwapSource source;
      source.device = description.device;
      source.image = texture.vk;
      source.view = texture.imageView;
      source.width = texture.desc.width;
      source.height = texture.desc.height;
      source.format = rex::graphics::xenos::TextureFormat(output_fetch[1] & 63);
      source.keep_alive = std::make_shared<RetainedImage>(RetainedImage{shared_from_this(), output});
      return source;
    } catch (const std::exception& error) {
      StopForFailure(frame,error.what());
    }
  }
};

std::shared_ptr<RuntimeState> running;
}  // namespace

void Gow3StartNativeRenderer(rex::graphics::GraphicsSystem* graphics, const std::filesystem::path& cache_root) {
  // Decided before guest boot: the guest D3D picks GPU-less mode during device
  // creation. Xenia rendering always keeps the PM4 command processor.
  REXCVAR_SET(native_direct_execution,NativeRasterRequested()&&REXCVAR_GET(gow3_direct_execution));
  gow3::renderer::dependency_bounded_direct_prefixes=REXCVAR_GET(gow3_native_direct_dependency_bound);
  // Hardware render targets replace the packed EDRAM/FSI representation, so
  // every option built on it is switched off before its conflict checks run.
  if(NativeRasterRequested()&&REXCVAR_GET(gow3_native_hardware_render_targets)) {
    static constexpr const char* kFsiOnly[]={
      "gow3_native_packed_zero_depth_clear",
      "gow3_native_packed_primary_stencil_clear",
      "gow3_native_packed_secondary_stencil_clear",
      "gow3_native_fused_atlas_depth_clear",
      "gow3_native_direct_atlas_depth_resolves",
      "gow3_native_fused_packed_atlas_resolves",
      "gow3_native_canonical_resolve_stores",
      "gow3_native_direct_packed_color_resolves",
      "gow3_native_atlas_depth_region_restores",
      "gow3_native_skip_full_color_clear_restore",
      "gow3_native_attachmentless_fsi",
      "gow3_native_discard_dummy_fsi_attachment",
      "gow3_native_packed_color_render_pass_groups",
      "gow3_native_packed_color_blend_pass_groups",
      "gow3_native_packed_color_action_pass_groups",
      "gow3_native_packed_depth_render_pass_groups",
      "gow3_native_fsi_draw_pass_groups",
      "gow3_native_fsi_draw_pass_queries",
      "gow3_native_stencil_material_cutoff",
      "gow3_native_opaque_color_overwrite",
      "gow3_native_forward_control_flow",
      "gow3_native_forward_vertex_control_flow",
      "gow3_native_forward_pixel_catalog",
      "gow3_native_specialize_fsi_pipeline_state",
      "gow3_native_fsi_state_material_composition",
      "gow3_native_fsi_state_inactive_early_catalog",
      "gow3_native_fsi_action_state_overflow_lookup",
      "gow3_native_fsi_state_cache_headroom",
      "gow3_native_fsi_state_forward_cfg",
      "gow3_native_readonly_depth_read_only_pass",
      "gow3_native_readonly_depth_lower_bound_query",
      "gow3_native_readonly_depth_skip_edram_barrier",
      "gow3_native_retained_fp24_depth_writer",
      "gow3_native_retained_opaque_writer",
      "gow3_native_reuse_retained_opaque_resources",
      "gow3_native_retained_d32_composition",
      "gow3_native_retain_primary_across_disjoint_queries",
      "gow3_native_retained_shared_depth_queries",
      "gow3_native_retained_opaque_catalog",
      "gow3_native_retained_opaque_phase",
      "gow3_native_retained_opaque_phase_render_pass_groups",
      "gow3_native_share_retained_opaque_phase_scratch",
      "gow3_native_poll_retained_opaque_phase_pressure",
      "gow3_native_share_retained_opaque_phase_attachments",
      "gow3_native_early_depth",
      "gow3_native_reuse_early_depth_readonly_boundaries",
      "gow3_native_early_depth_catalog",
      "gow3_native_early_depth_material_retention",
      "gow3_native_readonly_depth_no_write_metadata",
      "gow3_native_readonly_depth_conservative",
      "gow3_native_readonly_depth_attachment",
      "gow3_native_builtin_readonly_depth_queries",
      "gow3_native_builtin_atomic_depth",
      "gow3_native_exact_fsi_stores",
      "gow3_native_implicit_fsi_barriers",
      "gow3_native_coalesce_draw_barriers",
      "gow3_native_precise_fsi_barriers",
      "gow3_native_retain_fsi_raster_extent",
      "gow3_native_readonly_query_render_pass_groups",
      "gow3_native_pinned_null_constant_uploads"};
    for(const char* name:kFsiOnly)rex::cvar::SetFlagByName(name,"false");
    // Consecutive draws share command buffers so they can share render passes.
    rex::cvar::SetFlagByName("gow3_native_multi_draw_command_buffers","true");
  }
  if (!NativeRasterRequested()) return;
  if (running) {
    throw std::runtime_error("Native renderer is already running; changing renderer requires a restart");
  }
  std::shared_ptr<RuntimeState> state;
  bool provider_installation_started = false;
  try {
    auto* processor = graphics ? dynamic_cast<Processor*>(graphics->command_processor()) : nullptr;
    if (!processor) throw std::runtime_error("Native renderer requires the Vulkan host backend");
    if(NativeRasterRequested()&&(!REXCVAR_QUERY(bool,occlusion_query_enable)||
        REXCVAR_QUERY(int32_t,resolution_scale)>1||REXCVAR_QUERY(int32_t,draw_resolution_scale_x)>1||
        REXCVAR_QUERY(int32_t,draw_resolution_scale_y)>1||REXCVAR_QUERY(std::string,render_target_path_vulkan)=="fbo"))
      throw std::runtime_error("Native renderer requires 1x draw resolution, FSI storage, and occlusion queries enabled");
    if(NativeRasterRequested()&&!processor->CanUseNativeQueryAuthority())
      throw std::runtime_error("Native renderer requires1x FSI storage and available occlusion query resources");
    auto description = DescribeDevice(processor->GetVulkanDevice());
    const auto unsupported = CheckLiveVulkanDevice(description);
    if (!unsupported.empty()) throw std::runtime_error(unsupported);
    if(NativeRasterRequested()&&(!description.ordered_pixel_interlock||!description.features.fragmentStoresAndAtomics))
      throw std::runtime_error("Native raster authority requires enabled ordered pixel interlock and fragment storage writes");
    state = std::make_shared<RuntimeState>(processor, graphics->memory(), std::move(description), true, cache_root);
    if(state->direct&&(!state->native_raster||!state->deferred_queries||!state->retire_query_prefixes))
      throw std::runtime_error("Direct execution requires native raster with deferred, retired query prefixes");
    // Direct execution has no packets to own: no command spans, no packet prefixes.
    Gow3EnableGraphicsCommandSpans(state->ownership&&!state->direct);
    Gow3EnableDirectQueries(state->direct);
    if(state->direct) {
      std::weak_ptr<RuntimeState> weak=state;
      processor->SetDirectIdleHook([weak] {
        auto self=weak.lock();
        return self?self->DirectIdle():false;
      });
      REXLOG_INFO("Native direct execution: guest D3D runs GPU-less; API calls execute without the PM4 command processor");
    }
    Gow3SetGraphicsBoundarySink([state](GraphicsBoundaryEvent&& event) { state->Push(std::move(event)); });
    state->StartShaderPrewarm();
    // One synchronous CP transaction installs all providers before guest boot.
    // Startup failure propagates to the app and cannot launch a Xenia scene.
    provider_installation_started = true;
    Processor::StrictNativeProviders providers;
    providers.storage = REXCVAR_GET(gow3_native_shared_providers)
        ? rex::graphics::NativeCallbackStorage::SharedImmutable
        : rex::graphics::NativeCallbackStorage::CopyPerInvocation;
    providers.complete = [state](rex::graphics::NativeCompletionReason reason) {
      if(reason==rex::graphics::NativeCompletionReason::QueryPressure)state->CompleteQueryPressure(true);
      else state->CompletePending(state->diagnose_completion_causes?RuntimeState::CompletionCauseForSdk(reason):RuntimeState::CompletionCause::Unclassified);
    };
    providers.observe_accepted_work = [state]() noexcept {
      using Snapshot=gow3::diagnostic::MarkerFollowingCensus::Snapshot;
      if(state->failed||!state->renderer)return Snapshot{};
      const auto value=state->renderer->ObserveAcceptedWork();
      return Snapshot{value.generation,value.draws,value.transfers,
          value.accepted,value.completed,value.available};
    };
    if(REXCVAR_GET(gpu_command_summary))providers.observe_marker_readiness = [state]() noexcept {
      using Readiness=gow3::diagnostic::MarkerQueryCohortCensus::Readiness;
      if(state->failed||!state->renderer||state->current_query_end||
         state->next_ready_query>state->ready_query_publications.size())return Readiness{};
      const auto work=state->renderer->ObserveAcceptedWork();
      const auto queries=state->renderer->ObserveMarkerQueryReadiness();
      const auto returned=state->ready_query_publications.size()-state->next_ready_query;
      if(!work.available||!queries.available||returned>128||
         state->pending_query_publications.size()!=queries.closed+returned||
         state->pending_query_publications.size()>128)return Readiness{};
      return Readiness{{work.generation,work.draws,work.transfers,work.accepted,work.completed,work.available},
          uint32_t(queries.closed+returned),uint32_t(queries.ready+returned),queries.unknown,queries.active,
          !state->retire_query_prefixes&&queries.closed+returned>0&&work.completed<work.accepted,true};
    };
    if(REXCVAR_GET(gpu_command_summary)&&REXCVAR_GET(gpu_native_marker_operands))
      providers.observe_marker_allocation = [](uint32_t physical_target) noexcept {
        const auto a=gow3::renderer::ObserveMarkerCounterAllocation(physical_target);
        return gow3::diagnostic::MarkerOperandCensus::Allocation{a.device,a.virtual_counter,a.physical_counter,
          a.allocation_base,a.allocation_bytes,a.logical_bytes,a.registration_generation,a.command_epoch,a.available,a.live};
      };
    providers.present = [state](uint32_t frame,uint32_t buffer,uint32_t width,uint32_t height) {
      return state->Present(frame,buffer,width,height);
    };
    providers.query = [state](const Processor::NativeQueryBoundary& boundary) {state->QueryBoundary(boundary);};
    providers.draw = [state](const Processor::NativeDrawBoundary& boundary) {return state->DrawBoundary(boundary);};
    providers.defer_queries = state->deferred_queries;
    if(state->cache_coherence_waits)providers.coherence = [state] {return state->SubmitCacheCoherence();};
    if(REXCVAR_QUERY(bool,gpu_native_submit_completion_markers)) {
      if(!state->cache_coherence_waits)
        throw std::runtime_error("Native marker submission requires native cache-coherence submission");
      providers.immediate_markers.prepare=[state]{return state->PrepareImmediateMarkerInputs();};
      providers.immediate_markers.acquire=[](const rex::graphics::NativeCompletionMarkerPair& pair) {
        return gow3::renderer::AcquireNativeImmediateMarkerTargetGuard(pair);
      };
    }
    processor->InstallStrictNativeRendering(std::move(providers));
    try {Gow3EnableNativeQueryPollYield(REXCVAR_GET(gow3_native_query_poll_yield));}
    catch(const std::exception& error) {
      Gow3EnableNativeQueryPollYield(false);
      REXLOG_WARN("Native query poll yield disabled: {}",error.what());
    }
    try {Gow3EnableNativeGetDataInline(REXCVAR_GET(gow3_native_inline_getdata_register_helpers));}
    catch(const std::exception& error) {
      Gow3EnableNativeGetDataInline(false);
      REXLOG_WARN("Native GetData helper inline disabled; original body retained: {}",error.what());
    }
    if(state->query_readiness)Gow3SetQueryReadinessProbe([census=state->query_readiness](uint32_t query) {
      return census->Read(query);
    });
    if(state->published_results) {
      if(!state->retire_query_prefixes)
        throw std::runtime_error("Published-query consumer requires exact query-prefix retirement");
      Gow3SetPublishedQueryResults(state->published_results);
    }
    running = std::move(state);
    REXLOG_INFO("Native renderer enabled from guest boot: exclusive native raster, queries and presentation; automatic fallback disabled");
  } catch (const std::exception& error) {
    if (state) {
      state->stopping = true;
      state->pending_changed.notify_all();
      state->StopPlanner();
    }
    if(state)state->StopShaderDiscovery();
    Gow3SetGraphicsBoundarySink({});
    Gow3EnableGraphicsCommandSpans(false);
    if (state) {
      state->StopMemoryCallbacks();
      if (provider_installation_started) state->processor->ShutdownNativeRendering();
    }
    REXLOG_ERROR("Native renderer startup failed: {}; automatic fallback is disabled", error.what());
    throw;
  }
}

void Gow3StopNativeRenderer() {
  Gow3EnableNativeGetDataInline(false);
  auto state = std::exchange(running, {});
  if (!state) return;
  if(state->direct) {
    state->processor->SetDirectIdleHook({});
    Gow3EnableDirectQueries(false);
  }
  state->processor->ShutdownNativeRendering();
  state->stopping = true;
  state->pending_changed.notify_all();
  state->StopPlanner();
  state->StopShaderDiscovery();
  Gow3SetGraphicsBoundarySink({});
  Gow3EnableGraphicsCommandSpans(false);
  state->StopMemoryCallbacks();
  // CP may still be completing its current native packet. Keep ownership and
  // immutable snapshots until queued SDK shutdown releases its last provider.
  // Destruction happens after CP's current provider call and after any retained
  // presented images complete, while the host Vulkan device is still alive.
}

std::function<void(rex::graphics::ShaderStorageProgress&)> Gow3CreateNativePipelinePrewarmTask() {
  // UI-thread factory: never read global running from the background loader.
  auto state=running;if(!state||!state->pipeline_recipes)return {};
  return [state=std::move(state)](rex::graphics::ShaderStorageProgress& progress) {
    try {state->PrepareObservedPipelines(progress);}
    catch(const std::exception& error){REXLOG_WARN("Native observed pipeline preparation unavailable: {}",error.what());progress.Finish();}
  };
}
