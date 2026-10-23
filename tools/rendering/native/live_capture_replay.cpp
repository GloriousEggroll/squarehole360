// Cold full-workload oracle: API capture -> real GearsLivePlan -> typed Scene
// execution, or explicit JSON materialization of the exact same live plan.
#include "api_capture_reader.h"
#include "renderer/native/scene_executor.h"
#include "renderer/native/stencil_material_cutoff_compiler.h"
#include "renderer/native/constant_data.h"
#include "renderer/native/fsi_exact_stores_compiler.h"
#include "renderer/native/fsi_immutable_state_hoist_compiler.h"
#include "renderer/native/live_plan.h"
#include "renderer/native/shader_bindings.h"
#include "renderer/native/immutable_snapshots.h"
#include "renderer/native/deferred_texture_snapshots.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/retained_opaque_compiler.h"
#include "renderer/native/retained_opaque_writer.h"
#include "renderer/native/retained_opaque_catalog.h"
#include "renderer/native/retained_opaque_catalog_compiler.h"
#include "renderer/native/retained_opaque_phase_catalog.h"
#include "renderer/native/retained_opaque_phase_compiler.h"
#include "retained_opaque_scene_fixture.h"
#include "retained_opaque_phase_owner_fixture.h"
#include "retained_opaque_phase_marker_fixture.h"
#include "retained_opaque_phase_pass_failure_fixture.h"
#include "retained_queued_draw_export_fixture.h"
#include "retained_resource_pool_scene_fixture.h"
#include "retained_opaque_state_fixture.h"
#include "retained_shared_depth_scene_fixture.h"
#include "retained_fp24_depth_scene_fixture.h"
#include "retained_disjoint_query_scene_fixture.h"
#include "early_depth_catalog_scene_fixture.h"
#include "early_depth_material_retention_scene_fixture.h"
#include "overflow_pipeline_prewarm_fixture.h"
#include "fused_atlas_depth_clear_scene_fixture.h"
#include "canonical_store_scene_fixture.h"
#include "renderer/native/retained_fp24_depth.h"
#include "renderer/native/native_early_depth_shader.h"
#include "renderer/native/native_early_depth_catalog.h"
#include "renderer/native/opaque_color_overwrite_compiler.h"
#include "renderer/native/forward_control_flow_compiler.h"
#include "renderer/native/forward_vertex_compiler.h"
#include "renderer/native/forward_pixel_catalog_compiler.h"
#include "renderer/native/fsi_state_specialization_compiler.h"
#include "renderer/native/fsi_state_forward_cfg_compiler.h"
#include "renderer/native/fsi_state_forward_cfg.h"
#include "renderer/native/fsi_state_material_composition.h"
#include "renderer/native/fsi_material_shader_cache_headroom.h"
#include "renderer/native/fsi_state_recipe_cache.h"
#include "renderer/native/native_frame_capture_reader.h"
#include "renderer/native/pipeline_prewarm.h"
#include "renderer/native/builtin_readonly_query.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <cstring>

using namespace gow3::renderer;
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
std::vector<uint8_t> Read(const fs::path& path,size_t limit) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);const auto size=file.tellg();
    if(!file||size<0||uint64_t(size)>limit)throw std::runtime_error("Invalid input: "+path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if(!file)throw std::runtime_error("Truncated input: "+path.string());return bytes;
}
uint32_t Number(std::string_view text,uint32_t maximum) {
    size_t used=0;const auto value=std::stoull(std::string(text),&used);
    if(used!=text.size()||value>maximum)throw std::runtime_error("Invalid integer option");
    return uint32_t(value);
}
}
int main(int argc,char** argv)try {
    if(argc<5)throw std::runtime_error("Usage: gow3-native-live-capture-replay api.g3d <original scene directory> <compiled shaders> <new output directory> [--json-adapter] [--validate-only] [--native-fsi] [--anisotropy=5] [--debug-raw-images] [--draw-batch=64] [--async-gpu-submissions] [--last-event=N]");
    SceneExecutorConfig config;config.capture=argv[2];config.compiled=argv[3];config.output=argv[4];
    bool json_adapter=false,validate_only=false,lazy_device_states=false,wrap_frame_queries=false,fsi_state_expect_misses=false,full_edram_fixture=false,pure_fsi_state_requested=false,retained_partial_fixture=false,retained_catalog_partial_fixture=false;
    std::string retained_catalog_state_fixture;
    uint32_t retained_catalog_prewarm_limit=kRetainedOpaqueCatalogMaxPipelines;
    bool retained_queued_draw_fixture=false,retained_pool_fixture=false;
    bool fp24_scene_fixture=false;uint32_t fp24_prewarm_limit=4;
    uint32_t retained_shared_query_prewarm_limit=1;
    bool retained_shared_scene_fixture=false,retained_pooled_shared_scene_fixture=false;
    bool retained_disjoint_query_scene_fixture=false,early_catalog_scene_fixture=false,overflow_pipeline_fixture=false;
    bool early_material_scene_fixture=false;
    bool prewarm_phase_originals=false;
    bool retained_phase_validation_fixtures=false,retained_phase_queued_export_fixture=false,retained_phase_marker_fixture=false,retained_phase_pass_failure_fixture=false;
    bool fused_atlas_clear_scene_fixture=false;
    bool direct_atlas_resolve_scene_fixture=false;
    bool packed_primary_stencil_scene_fixture=false;
    bool direct_packed_color_scene_fixture=false;
    bool packed_secondary_stencil_scene_fixture=false;
    bool fused_packed_atlas_scene_fixture=false,canonical_store_scene_fixture=false;
    fs::path overflow_pipeline_recipes;
    uint32_t fsi_state_prewarm_limit=kFsiStateMaxPipelines;bool fsi_state_prewarm_limit_requested=false;
    uint32_t through=UINT32_MAX;uint32_t early_prewarm_limit=8,early_catalog_prewarm_limit=8;fs::path early_recipes;fs::path exact_oracle,forward_vertex_recipes,catalog_recipes,readonly_recipes,fsi_state_recipes,fsi_state_sources,fsi_state_observe;
    for(int i=5;i<argc;++i) {
        const std::string_view option=argv[i];
        if(option=="--lazy-device-states")lazy_device_states=true;
        else if(option=="--json-adapter")json_adapter=true;
        else if(option=="--validate-only")validate_only=true;
        else if(option=="--wrap-frame-queries") {
            wrap_frame_queries=true;config.defer_native_queries=true;
        }
        else if(option.starts_with("--exact-live-oracle="))exact_oracle=option.substr(std::string_view("--exact-live-oracle=").size());
        else if(std::string_view(argv[i])=="--stencil-export-restores")config.restore_depth_stencil_export=true;
        else if(option=="--fragment-invocation-census")config.replay_fragment_invocations=true;
        else if(option=="--native-stencil-material-cutoff")config.native_stencil_material_cutoff=true;
        else if(option.starts_with("--stencil-material-mode=")) {
            const auto mode=option.substr(std::string_view("--stencil-material-mode=").size());
            if(mode!="observe"&&mode!="cutoff")throw std::runtime_error("Stencil material mode must be observe or cutoff");
            config.replay_stencil_material_mode=mode=="observe"?1:2;
        }
        else if(option=="--gpu-draw-timestamps-every-frame") {
            config.gpu_draw_timestamps=config.gpu_draw_timestamps_every_frame=true;
            config.unified_gpu_queue=config.batch_transfers=true;
        }
        else if(option=="--gpu-draw-timestamps") {
            config.gpu_draw_timestamps=true;config.unified_gpu_queue=true;config.batch_transfers=true;
        }
        else if(option=="--native-fsi")config.native_fsi=true;
        else if(option=="--overflow-pipeline-prewarm-fixture")overflow_pipeline_fixture=true;
        else if(option=="--fused-atlas-clear-scene-fixture")fused_atlas_clear_scene_fixture=true;
        else if(option=="--direct-atlas-resolve-scene-fixture")direct_atlas_resolve_scene_fixture=true;
        else if(option=="--packed-primary-stencil-scene-fixture")packed_primary_stencil_scene_fixture=true;
        else if(option=="--direct-packed-color-scene-fixture")direct_packed_color_scene_fixture=true;
        else if(option=="--packed-secondary-stencil-scene-fixture")packed_secondary_stencil_scene_fixture=true;
        else if(option=="--canonical-store-scene-fixture")canonical_store_scene_fixture=true;
        else if(option=="--canonical-resolve-stores")config.canonical_resolve_stores=true;
        else if(option=="--fused-packed-atlas-scene-fixture")fused_packed_atlas_scene_fixture=true;
        else if(option.starts_with("--overflow-pipeline-recipes="))overflow_pipeline_recipes=option.substr(std::string_view("--overflow-pipeline-recipes=").size());
        else if(option=="--specialize-fsi-pipeline-state") {config.specialize_fsi_pipeline_state=true;pure_fsi_state_requested=true;}
        else if(option=="--fsi-state-forward-cfg") {config.specialize_fsi_pipeline_state=true;config.fsi_state_forward_cfg=true;}
        else if(option=="--native-frame-capture")config.native_completed_frame_capture=true;
        else if(option=="--fsi-state-material-composition") {config.specialize_fsi_pipeline_state=true;config.fsi_state_material_composition=true;}
        else if(option=="--fsi-state-inactive-early-catalog") config.fsi_state_inactive_early_catalog=true;
        else if(option=="--fsi-state-cache-headroom")config.fsi_state_cache_headroom=true;
        else if(option=="--full-edram-fixture")full_edram_fixture=true;
        else if(option=="--retained-fp24-depth-writer")config.retained_fp24_depth_writer=true;
        else if(option=="--retained-fp24-depth-scene-fixture")fp24_scene_fixture=true;
        else if(option.starts_with("--retained-fp24-depth-prewarm-limit="))fp24_prewarm_limit=Number(option.substr(std::string_view("--retained-fp24-depth-prewarm-limit=").size()),4);
        else if(option=="--retained-opaque-writer")config.retained_opaque_writer=true;
        else if(option=="--retained-opaque-catalog")config.retained_opaque_catalog=true;
        else if(option=="--retained-opaque-phase")config.retained_opaque_phase=true;
        else if(option=="--retained-opaque-phase-render-pass-groups")config.retained_opaque_phase_render_pass_groups=true;
        else if(option=="--no-retained-opaque-phase-render-pass-groups")config.retained_opaque_phase_render_pass_groups=false;
        else if(option=="--share-retained-opaque-phase-scratch")config.share_retained_opaque_phase_scratch=true;
        else if(option=="--no-share-retained-opaque-phase-scratch")config.share_retained_opaque_phase_scratch=false;
        else if(option=="--poll-retained-opaque-phase-pressure")config.poll_retained_opaque_phase_pressure=true;
        else if(option=="--no-poll-retained-opaque-phase-pressure")config.poll_retained_opaque_phase_pressure=false;
        else if(option=="--share-retained-opaque-phase-attachments")config.share_retained_opaque_phase_attachments=true;
        else if(option=="--no-share-retained-opaque-phase-attachments")config.share_retained_opaque_phase_attachments=false;
        else if(option=="--prewarm-observed-opaque-phase-originals")prewarm_phase_originals=true;
        else if(option=="--retained-opaque-phase-validation-fixtures") {
            retained_phase_validation_fixtures=true;config.defer_native_queries=true;
        }
        else if(option=="--retained-opaque-phase-queued-export-fixture") {
            retained_phase_queued_export_fixture=true;config.defer_native_queries=true;
        }
        else if(option=="--retained-opaque-phase-pass-failure-fixture") {
            retained_phase_pass_failure_fixture=true;config.defer_native_queries=true;
        }
        else if(option=="--retained-opaque-phase-marker-fixture") {
            retained_phase_marker_fixture=true;config.defer_native_queries=true;
        }
        else if(option=="--reuse-retained-opaque-resources")config.reuse_retained_opaque_resources=true;
        else if(option=="--retained-shared-depth-queries")config.retained_shared_depth_queries=true;
        else if(option=="--retained-d32-composition")config.retained_d32_composition=true;
        else if(option=="--retain-primary-across-disjoint-queries")config.retain_primary_across_disjoint_queries=true;
        else if(option=="--retained-disjoint-query-scene-fixture") {retained_disjoint_query_scene_fixture=true;config.defer_native_queries=true;}
        else if(option=="--retained-shared-depth-scene-fixture")retained_shared_scene_fixture=true;
        else if(option=="--retained-pooled-shared-depth-scene-fixture")retained_pooled_shared_scene_fixture=true;
        else if(option.starts_with("--retained-shared-depth-query-prewarm-limit="))retained_shared_query_prewarm_limit=
            Number(option.substr(std::string_view("--retained-shared-depth-query-prewarm-limit=").size()),1);
        else if(option=="--queue-retained-exports-for-draws")config.queue_retained_exports_for_draws=true;
        else if(option=="--no-queue-retained-exports-for-draws")config.queue_retained_exports_for_draws=false;
        else if(option=="--retained-queued-draw-export-fixture")retained_queued_draw_fixture=true;
        else if(option=="--retained-resource-pool-scene-fixture")retained_pool_fixture=true;
        else if(option.starts_with("--retained-opaque-catalog-state-fixture="))
            retained_catalog_state_fixture=option.substr(std::string_view("--retained-opaque-catalog-state-fixture=").size());
        else if(option.starts_with("--retained-opaque-catalog-prewarm-limit="))retained_catalog_prewarm_limit=
            Number(option.substr(std::string_view("--retained-opaque-catalog-prewarm-limit=").size()),kRetainedOpaqueCatalogMaxPipelines);
        else if(option=="--retained-opaque-partial-lifetime-fixture")retained_partial_fixture=true;
        else if(option=="--retained-opaque-catalog-partial-lifetime-fixture")retained_catalog_partial_fixture=true;
        else if(option=="--fsi-state-expect-resident-misses")fsi_state_expect_misses=true;
        else if(option.starts_with("--fsi-state-prewarm-limit=")) {
            fsi_state_prewarm_limit=Number(option.substr(std::string_view("--fsi-state-prewarm-limit=").size()),kFsiStateMaxPipelines);
            fsi_state_prewarm_limit_requested=true;
        }
        else if(option.starts_with("--fsi-state-recipes="))fsi_state_recipes=option.substr(std::string_view("--fsi-state-recipes=").size());
        else if(option.starts_with("--fsi-state-source-recipes="))fsi_state_sources=option.substr(std::string_view("--fsi-state-source-recipes=").size());
        else if(option.starts_with("--observe-fsi-state-recipes="))fsi_state_observe=option.substr(std::string_view("--observe-fsi-state-recipes=").size());
        else if(option=="--implicit-fsi-barriers")config.implicit_fsi_barriers=true;
        else if(option=="--builtin-readonly-depth-queries")config.builtin_readonly_depth_queries=true;
        else if(option=="--early-depth-catalog-scene-fixture") {early_catalog_scene_fixture=true;config.defer_native_queries=true;}
        else if(option=="--early-depth-material-retention-scene-fixture") {early_material_scene_fixture=true;config.defer_native_queries=true;}
        else if(option=="--native-early-depth")config.native_early_depth=true;
        else if(option=="--reuse-early-depth-readonly-boundaries")config.reuse_early_depth_readonly_boundaries=true;
        else if(option=="--native-early-depth-material-retention")config.native_early_depth_material_retention=true;
        else if(option=="--native-early-depth-catalog") {config.native_early_depth=true;config.native_early_depth_catalog=true;}
        else if(option.starts_with("--early-depth-catalog-prewarm-limit="))early_catalog_prewarm_limit=Number(option.substr(std::string_view("--early-depth-catalog-prewarm-limit=").size()),8);
        else if(option.starts_with("--early-depth-recipes="))early_recipes=option.substr(std::string_view("--early-depth-recipes=").size());
        else if(option.starts_with("--early-depth-prewarm-limit="))early_prewarm_limit=Number(option.substr(std::string_view("--early-depth-prewarm-limit=").size()),8);
        else if(option=="--native-readonly-depth-attachment")config.native_readonly_depth_attachment=true;
        else if(option=="--native-readonly-depth-read-only-pass")config.native_readonly_depth_read_only_pass=true;
        else if(option=="--native-readonly-depth-lower-bound-query")config.native_readonly_depth_lower_bound_query=true;
        else if(option=="--host-query-reset")config.host_query_reset=true;
        else if(option=="--readonly-query-render-pass-groups")config.readonly_query_render_pass_groups=true;
        else if(option=="--wrap-readonly-query-groups") {
            config.diagnostic_readonly_query_groups=true;config.defer_native_queries=true;
        }
        else if(option=="--native-readonly-depth-no-write-metadata")config.native_readonly_depth_no_write_metadata=true;
        else if(option=="--no-native-readonly-depth-no-write-metadata")config.native_readonly_depth_no_write_metadata=false;
        else if(option=="--native-readonly-depth-conservative")config.native_readonly_depth_conservative=true;
        else if(option=="--no-native-readonly-depth-conservative")config.native_readonly_depth_conservative=false;
        else if(option=="--native-readonly-depth-skip-edram-barrier")config.native_readonly_depth_skip_edram_barrier=true;
        else if(option.starts_with("--readonly-query-recipes="))readonly_recipes=option.substr(std::string_view("--readonly-query-recipes=").size());
        else if(option=="--builtin-atomic-depth")config.builtin_atomic_depth=true;
        else if(option=="--opaque-color-overwrite")config.opaque_color_overwrite=true;
        else if(option=="--forward-control-flow")config.forward_control_flow=true;
        else if(option=="--forward-vertex-control-flow")config.forward_vertex_control_flow=true;
        else if(option=="--forward-pixel-catalog")config.forward_pixel_catalog=true;
        else if(option.starts_with("--forward-pixel-catalog-recipes="))catalog_recipes=option.substr(std::string_view("--forward-pixel-catalog-recipes=").size());
        else if(option.starts_with("--forward-vertex-recipes="))forward_vertex_recipes=option.substr(std::string_view("--forward-vertex-recipes=").size());
        else if(option=="--queue-cpu-memory-writes")config.queue_cpu_memory_writes=true;
        else if(option=="--retire-query-prefixes") {
            config.defer_native_queries=true;config.retire_completed_query_prefixes=true;
        }
        else if(option=="--retain-fsi-raster-extent")config.retain_fsi_raster_extent=true;
        else if(option=="--cache-texture-layouts")config.cache_texture_layouts=true;
        else if(option.starts_with("--dormant-texture-entries="))config.max_dormant_texture_entries=
            Number(option.substr(std::string_view("--dormant-texture-entries=").size()),2048);
        else if(option=="--recycle-resolve-images")config.recycle_resolve_images=true;
        else if(option=="--recycle-resolve-descriptors")config.recycle_resolve_descriptors=true;
        else if(option=="--recycle-pass-transfer-descriptors")config.recycle_pass_transfer_descriptors=true;
        else if(option=="--recycle-pass-transfer-render-passes")config.recycle_pass_transfer_render_passes=true;
        else if(option=="--reuse-resolve-identity-views")config.reuse_resolve_identity_views=true;
        else if(option=="--diagnose-texture-eviction-reloads")config.diagnose_texture_eviction_reloads=true;
        else if(option=="--recycle-vertex-buffers")config.recycle_vertex_buffers=true;
        else if(option=="--raw-gpu-float-cpu-textures")config.raw_gpu_float_cpu_textures=true;
        else if(option=="--forward-cpu-upload-receipts")config.forward_cpu_upload_receipts=true;
        else if(option=="--share-cpu-texture-sampling-variants")config.share_cpu_texture_sampling_variants=true;
        else if(option=="--defer-cpu-texture-drains")config.defer_cpu_texture_drains=true;
        else if(option=="--cpu-image-slot-lookup")config.cpu_image_slot_lookup=true;
        else if(option=="--associative-texture-layouts")config.associative_texture_layouts=true;
        else if(option=="--block-page-versions")config.block_page_versions=true;
        else if(option=="--range-texture-versions")config.range_texture_versions=true;
        else if(option=="--coalesced-texture-ranges")config.coalesced_texture_ranges=true;
        else if(option=="--compact-cpu-texture-version-pages")config.compact_cpu_texture_version_pages=true;
        else if(option=="--cache-gpu-alias-states")config.cache_gpu_alias_states=true;
        else if(option=="--queued-volume-alias-loads")config.queued_volume_alias_loads=true;
        else if(option=="--no-cache-gpu-alias-states")config.cache_gpu_alias_states=false;
        else if(option=="--compact-resolve-channels")config.compact_resolve_channels=true;
        else if(option=="--coalesce-draw-barriers")config.coalesce_draw_barriers=true;
        else if(option=="--batch-draw-descriptors")config.batch_draw_descriptors=true;
        else if(option=="--skip-unused-descriptor-sets")config.skip_unused_descriptor_sets=true;
        else if(option=="--shader-lookup-lookaside")config.shader_lookup_lookaside=true;
        else if(option=="--cache-draw-descriptors")config.cache_draw_descriptors=true;
        else if(option=="--delta-constant-uploads")config.delta_constant_uploads=true;
        else if(option=="--pinned-null-constant-uploads")config.pinned_null_constant_uploads=true;
        else if(option=="--atlas-depth-region-restores")config.atlas_depth_region_restores=true;
        else if(option=="--fused-atlas-depth-clear")config.fused_atlas_depth_clear=true;
        else if(option=="--direct-atlas-depth-resolves")config.direct_atlas_depth_resolves=true;
        else if(option=="--fused-packed-atlas-resolves")config.fused_packed_atlas_resolves=true;
        else if(option=="--packed-primary-stencil-clear")config.packed_primary_stencil_clear=true;
        else if(option=="--direct-packed-color-resolves")config.direct_packed_color_resolves=true;
        else if(option=="--packed-secondary-stencil-clear")config.packed_secondary_stencil_clear=true;
        else if(option=="--packed-zero-depth-clear")config.packed_zero_depth_clear=true;
        else if(option=="--skip-full-color-clear-restore")config.skip_full_color_clear_restore=true;
        else if(option=="--reset-draw-command-pool")config.reset_draw_command_pool=true;
        else if(option=="--time-draw-recording")config.time_draw_recording=true;
        else if(option=="--time-cpu-stages")config.time_cpu_stages=true;
        else if(option=="--no-time-cpu-stages")config.time_cpu_stages=false;
        else if(option=="--multi-draw-command-buffers")config.multi_draw_command_buffers=true;
        else if(option=="--four-inflight-batches")config.max_inflight_batches=4;
        else if(option=="--packed-color-render-pass-groups")config.packed_color_render_pass_groups=true;
        else if(option=="--packed-color-blend-pass-groups")config.packed_color_blend_pass_groups=true;
        else if(option=="--no-packed-color-blend-pass-groups")config.packed_color_blend_pass_groups=false;
        else if(option=="--packed-color-action-pass-groups")config.packed_color_action_pass_groups=true;
        else if(option=="--no-packed-color-action-pass-groups")config.packed_color_action_pass_groups=false;
        else if(option=="--packed-depth-render-pass-groups")config.packed_depth_render_pass_groups=true;
        else if(option=="--no-packed-depth-render-pass-groups")config.packed_depth_render_pass_groups=false;
        else if(option=="--fsi-draw-pass-groups")config.fsi_draw_pass_groups=true;
        else if(option=="--fsi-draw-pass-queries")config.fsi_draw_pass_queries=true;
        else if(option=="--report-frame-mismatch")config.report_frame_capture_mismatch=true;
        else if(option=="--lazy-query-retirement")config.replay_lazy_query_retirement=true;
        else if(option=="--fsi-hoist-mode=state")config.fsi_hoist_mode=NativeFsiHoistMode::ImmutableState;
        else if(option=="--fsi-hoist-mode=incoming")config.fsi_hoist_mode=NativeFsiHoistMode::ImmutableStateAndIncomingDepth;
        else if(option=="--exact-fsi-stores")config.exact_fsi_stores=true;
        else if(option=="--discard-dummy-fsi-attachment")config.discard_dummy_fsi_attachment=true;
        else if(option=="--attachmentless-fsi")config.attachmentless_fsi=true;
        else if(option=="--skip-readonly-sample-barriers")config.skip_readonly_sample_barriers=true;
        else if(option=="--precise-fsi-barriers")config.precise_fsi_barriers=true;

        else if(option.starts_with("--async-submission-capacity=")) {
            config.async_submission_capacity=Number(option.substr(std::string_view("--async-submission-capacity=").size()),128);
            if(config.async_submission_capacity!=64&&config.async_submission_capacity!=128)
                throw std::runtime_error("Async submission capacity must be 64 or 128");
        }
        else if(option=="--debug-raw-images")config.debug_raw_images=true;
        else if(option=="--batch-transfers")config.batch_transfers=true;
        else if(option=="--unified-gpu-queue"){config.batch_transfers=true;config.unified_gpu_queue=true;}
        else if(option=="--gpu-physical-timestamps")config.gpu_physical_timestamps=true;
        else if(option.starts_with("--gpu-physical-first="))config.gpu_physical_first=std::stoul(std::string(option.substr(21)));
        else if(option.starts_with("--gpu-physical-end="))config.gpu_physical_end=std::stoul(std::string(option.substr(19)));
        else if(option.starts_with("--gpu-physical-interval="))config.gpu_physical_interval=std::stoul(std::string(option.substr(24)));
        else if(option=="--gpu-timestamps"){config.batch_transfers=true;config.unified_gpu_queue=true;config.gpu_timestamps=true;}
        else if(option=="--async-gpu-submissions") {
            config.batch_transfers=true;config.unified_gpu_queue=true;config.async_gpu_submissions=true;
        } else if(option.starts_with("--draw-batch=")) {
            config.max_draw_batch_size=Number(option.substr(13),64);
            if(!config.max_draw_batch_size)throw std::runtime_error("Zero draw batch size");
        } else if(option.starts_with("--anisotropy=")) {
            const int32_t value=option.substr(13)=="-1"?-1:int32_t(Number(option.substr(13),5));
            config.anisotropy_override=[value]{return value;};
        } else if(option.starts_with("--last-event="))through=Number(option.substr(13),UINT32_MAX);
        else throw std::runtime_error("Unknown live replay option: "+std::string(option));
    }
    if(pure_fsi_state_requested&&config.fsi_state_forward_cfg)throw std::runtime_error("Pure/composed FSI state options are exclusive");
    if(config.fsi_state_cache_headroom) {
        if(!config.fsi_state_material_composition)
            throw std::runtime_error("FSI shader cache headroom requires material composition");
        const auto limits=NativeFsiMaterialShaderCacheMaximum(true);
        config.max_shader_cache_entries=limits.entries;
        config.max_shader_cache_bytes=limits.bytes;
    }
    if(fsi_state_prewarm_limit_requested&&!config.fsi_state_material_composition)
        throw std::runtime_error("Optional material prewarm limit requires explicit material composition");
    if(config.fsi_state_inactive_early_catalog&&!config.fsi_state_material_composition)
        throw std::runtime_error("Inactive early catalogue admission requires material composition");
    if(config.retained_opaque_phase&&(!config.fsi_state_material_composition||config.native_early_depth_material_retention||
       config.reuse_early_depth_readonly_boundaries))
        throw std::runtime_error("Retained opaque phase requires canonical FSI material composition without the early-depth retention experiment");
    if(config.retained_opaque_phase_render_pass_groups&&(!config.retained_opaque_phase||!config.fsi_state_material_composition||
       !config.native_fsi||!config.defer_native_queries||!config.unified_gpu_queue||!config.async_gpu_submissions||
       !config.batch_transfers||config.multi_draw_command_buffers))
        throw std::runtime_error("Retained phase pass groups require canonical asynchronous phase rendering without generic grouping");
    if(config.share_retained_opaque_phase_scratch&&(!config.retained_opaque_phase||!config.fsi_state_material_composition||
       !config.native_fsi||!config.defer_native_queries||!config.unified_gpu_queue||!config.async_gpu_submissions||
       !config.batch_transfers||config.reuse_retained_opaque_resources))
        throw std::runtime_error("Shared retained phase scratch requires an explicit canonical asynchronous phase without legacy resource pooling");
    if(config.poll_retained_opaque_phase_pressure&&(!config.retained_opaque_phase||!config.fsi_state_material_composition||
       !config.native_fsi||!config.defer_native_queries||!config.unified_gpu_queue||!config.async_gpu_submissions||
       !config.batch_transfers||config.reuse_retained_opaque_resources))
        throw std::runtime_error("Retained phase pressure polling requires an explicit canonical asynchronous phase without legacy resource pooling");
    if(config.share_retained_opaque_phase_attachments&&(!config.retained_opaque_phase||!config.share_retained_opaque_phase_scratch||
       !config.fsi_state_material_composition||!config.native_fsi||!config.defer_native_queries||
       !config.unified_gpu_queue||!config.async_gpu_submissions||!config.batch_transfers||config.reuse_retained_opaque_resources))
        throw std::runtime_error("Shared retained phase attachments require an explicit canonical asynchronous phase with shared scratch and no legacy resource pooling");
    if(prewarm_phase_originals&&!config.fsi_state_material_composition)
        throw std::runtime_error("Captured opaque phase original prewarm requires canonical FSI material composition");
    if(retained_phase_validation_fixtures||retained_phase_queued_export_fixture||retained_phase_marker_fixture||retained_phase_pass_failure_fixture) {
        if(unsigned(retained_phase_validation_fixtures)+unsigned(retained_phase_queued_export_fixture)+unsigned(retained_phase_marker_fixture)+unsigned(retained_phase_pass_failure_fixture)>1||
           (retained_phase_queued_export_fixture&&!config.queue_retained_exports_for_draws))
            throw std::runtime_error("Select one opaque phase fixture; queued fixture requires explicit queued exports");
        if(retained_phase_pass_failure_fixture&&!config.retained_opaque_phase_render_pass_groups)
            throw std::runtime_error("Phase follower failure fixture requires explicit phase pass grouping");
        if(!config.retained_opaque_phase||!prewarm_phase_originals||!full_edram_fixture||
           json_adapter||validate_only||wrap_frame_queries||config.diagnostic_readonly_query_groups||
           through!=UINT32_MAX||!exact_oracle.empty()||fs::exists(config.capture/"gpu-history.json"))
            throw std::runtime_error("Opaque phase validation fixtures require complete typed phase startup, explicit originals and zero EDRAM, without wrapped queries or history");
        for(int i=5;i<argc;++i) {
            const std::string_view argument_text=argv[i];
            const auto option=argument_text.substr(0,argument_text.find('='));
            const auto selected=retained_phase_pass_failure_fixture?"--retained-opaque-phase-pass-failure-fixture":
                retained_phase_queued_export_fixture?"--retained-opaque-phase-queued-export-fixture":
                retained_phase_marker_fixture?"--retained-opaque-phase-marker-fixture":"--retained-opaque-phase-validation-fixtures";
            if(option=="--last-event"||
               (option.find("fixture")!=option.npos&&option!=selected&&option!="--full-edram-fixture"))
                throw std::runtime_error("Opaque phase validation fixtures cannot combine with another fixture or last-event override");
        }
    }
    if(config.packed_color_blend_pass_groups&&(!config.packed_color_render_pass_groups||
       !config.fsi_state_material_composition||!config.specialize_fsi_pipeline_state))
        throw std::runtime_error("Packed blend pass groups require parent packed groups and material state composition");
    if(config.packed_color_action_pass_groups&&(!config.packed_color_render_pass_groups||
       !config.fsi_state_material_composition||!config.specialize_fsi_pipeline_state))
        throw std::runtime_error("Packed action pass groups require parent packed groups and material state composition");
    if(config.fsi_state_material_composition) {
        if(!fsi_state_prewarm_limit&&!fsi_state_expect_misses)
            throw std::runtime_error("Zero material prewarm requires explicit resident-miss proof");
        if(pure_fsi_state_requested||config.fsi_state_forward_cfg||!config.native_fsi||
           !config.unified_gpu_queue||!config.async_gpu_submissions||!config.batch_transfers||!config.defer_native_queries||
           config.builtin_atomic_depth||config.builtin_readonly_depth_queries||
           config.attachmentless_fsi||config.implicit_fsi_barriers||config.exact_fsi_stores||
           config.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||config.opaque_color_overwrite||
           config.forward_control_flow||config.forward_vertex_control_flow||config.forward_pixel_catalog||
           config.retained_opaque_writer||config.retained_opaque_catalog||config.retained_fp24_depth_writer||
           config.retained_shared_depth_queries||config.retained_d32_composition||config.retain_primary_across_disjoint_queries||
           config.reuse_retained_opaque_resources||(config.queue_retained_exports_for_draws&&!config.retained_opaque_phase)||
           config.native_stencil_material_cutoff||config.replay_stencil_material_mode||config.replay_fragment_invocations||
           config.multi_draw_command_buffers||!config.debug_pixels.empty())
            throw std::runtime_error("FSI material composition requires its bounded original asynchronous Native provider");
        if(wrap_frame_queries&&fsi_state_expect_misses)
            throw std::runtime_error("Wrapped material composition proves query refusal, not resident misses");
    }
    if(retained_disjoint_query_scene_fixture&&(!config.retained_d32_composition||!config.retain_primary_across_disjoint_queries||
       !config.native_readonly_depth_attachment||!(config.retained_fp24_depth_writer!=config.retained_opaque_catalog)||config.retained_opaque_writer||
       json_adapter||validate_only||wrap_frame_queries||full_edram_fixture||through!=UINT32_MAX||!exact_oracle.empty()||
       fp24_scene_fixture||retained_partial_fixture||retained_catalog_partial_fixture||retained_queued_draw_fixture||retained_pool_fixture||
       retained_shared_scene_fixture||retained_pooled_shared_scene_fixture||!retained_catalog_state_fixture.empty()||
       fp24_prewarm_limit!=4||retained_catalog_prewarm_limit!=6||config.reuse_retained_opaque_resources||
       config.queue_retained_exports_for_draws||config.retained_shared_depth_queries))
        throw std::runtime_error("Disjoint Scene fixture requires complete independent catalogueV2 or FP24 startup, canonical D32 and explicit composition/retention controls");
    if((early_catalog_scene_fixture||early_material_scene_fixture)&&(!config.native_fsi||!config.native_early_depth||!config.native_early_depth_catalog||
       !config.async_gpu_submissions||!config.unified_gpu_queue||!config.batch_transfers||
       json_adapter||validate_only||wrap_frame_queries||full_edram_fixture||through!=UINT32_MAX||!exact_oracle.empty()||
       early_prewarm_limit!=8||early_catalog_prewarm_limit!=8||config.gpu_draw_timestamps||config.gpu_timestamps||config.gpu_physical_timestamps||
       config.retained_opaque_writer||config.retained_opaque_catalog||config.retained_fp24_depth_writer||
       config.retained_shared_depth_queries||config.retained_d32_composition||config.retain_primary_across_disjoint_queries||
       config.native_readonly_depth_attachment||config.builtin_readonly_depth_queries||config.builtin_atomic_depth||
       config.specialize_fsi_pipeline_state||config.exact_fsi_stores||config.implicit_fsi_barriers||config.attachmentless_fsi||
       config.opaque_color_overwrite||config.forward_control_flow||config.forward_vertex_control_flow||config.forward_pixel_catalog||
       config.multi_draw_command_buffers||fp24_scene_fixture||retained_partial_fixture||retained_catalog_partial_fixture||
       retained_queued_draw_fixture||retained_pool_fixture||retained_shared_scene_fixture||retained_pooled_shared_scene_fixture||
       retained_disjoint_query_scene_fixture||!retained_catalog_state_fixture.empty()))
        throw std::runtime_error("Early catalogue Scene fixture requires exclusive complete typed asynchronous V1+V2 startup");
    if(early_catalog_scene_fixture&&early_material_scene_fixture)
        throw std::runtime_error("Choose one early-depth controlled fixture");
    if(overflow_pipeline_fixture&&(!config.native_fsi||overflow_pipeline_recipes.empty()||json_adapter||validate_only||
       through!=UINT32_MAX||!config.async_gpu_submissions||!config.unified_gpu_queue||!config.batch_transfers||
       config.attachmentless_fsi||config.implicit_fsi_barriers||config.exact_fsi_stores||
       config.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||config.specialize_fsi_pipeline_state||
       config.builtin_atomic_depth||config.builtin_readonly_depth_queries||config.retained_opaque_writer||
       config.retained_opaque_catalog||config.retained_fp24_depth_writer||config.retained_shared_depth_queries||
       config.retained_d32_composition||config.retain_primary_across_disjoint_queries||config.opaque_color_overwrite||
       config.forward_control_flow||config.forward_vertex_control_flow||config.forward_pixel_catalog||config.native_early_depth_catalog||
       early_catalog_scene_fixture||early_material_scene_fixture||fp24_scene_fixture||retained_partial_fixture||retained_catalog_partial_fixture||
       retained_queued_draw_fixture||retained_pool_fixture||retained_shared_scene_fixture||retained_pooled_shared_scene_fixture||
       retained_disjoint_query_scene_fixture||!retained_catalog_state_fixture.empty()))
        throw std::runtime_error("Overflow PSO fixture requires complete typed asynchronous canonical replay with optional V1/D32 only");
    if(!overflow_pipeline_fixture&&!overflow_pipeline_recipes.empty())
        throw std::runtime_error("Overflow recipe input requires the explicit fixture mode");
    if(full_edram_fixture&&!config.native_fsi)throw std::runtime_error("Full EDRAM fixture requires packed Native");
    if(fsi_state_expect_misses&&!config.specialize_fsi_pipeline_state)throw std::runtime_error("Expected state misses require explicit specialization");
    if(fs::exists(config.output)&&!fs::is_empty(config.output))throw std::runtime_error("Live replay output must be new or empty");
    // Keep the original capture/scene/history unmodified. A missing history is
    // allowed to fail exactly where the normal renderer detects undefined data.
    std::vector<uint8_t> api;
    std::string capture_hash;
    std::vector<std::shared_ptr<LiveRenderFrame>> frames;
    size_t retained=0,materialized=0,deferred=0;
    if(config.native_completed_frame_capture) {
        if(!config.native_fsi||through!=UINT32_MAX||wrap_frame_queries||config.diagnostic_readonly_query_groups||
           config.diagnostic_zero_history||retained_partial_fixture||retained_catalog_partial_fixture||
           !retained_catalog_state_fixture.empty()||retained_queued_draw_fixture||retained_pool_fixture||
           fp24_scene_fixture||retained_shared_scene_fixture||retained_pooled_shared_scene_fixture||
           retained_disjoint_query_scene_fixture||early_catalog_scene_fixture||early_material_scene_fixture||
           overflow_pipeline_fixture||retained_phase_validation_fixtures||retained_phase_queued_export_fixture||
           retained_phase_marker_fixture||retained_phase_pass_failure_fixture||fused_atlas_clear_scene_fixture||
           direct_atlas_resolve_scene_fixture||packed_primary_stencil_scene_fixture||direct_packed_color_scene_fixture||
           packed_secondary_stencil_scene_fixture||fused_packed_atlas_scene_fixture||canonical_store_scene_fixture)
            throw std::runtime_error("Native captured frames require complete unmodified operations and real queries/history");
        if(!fs::equivalent(argv[1],config.capture/"native-frame-capture.json"))
            throw std::runtime_error("Native capture input must be the completed manifest in the scene directory");
        auto saved=ReadNativeFrameCapture(config.capture);
        frames=std::move(saved.frames);capture_hash=saved.manifest_sha256;retained=saved.retained_bytes;
        config.native_frame_capture_sha256=capture_hash;
    } else {
        api=Read(argv[1],256u*1024*1024);capture_hash=ContentHash(api);
        GearsLivePlan planner({},false,lazy_device_states);ImmutableSnapshots snapshots;
        validation::ReadApiCapture(api,[&](GraphicsBoundaryEvent&& event) {
            // Match the live boundary's selective/deferred snapshot path. These
            // are exact captured bytes, never guest addresses or synthetic data.
            if(const auto required=SnapshotShaderInputs(event.blocks))for(auto& block:event.blocks) {
                if(block.kind>=14&&block.kind<=29&&!required->first.contains(95-(block.kind-14)))block.bytes.clear();
                if(block.kind>=32&&block.kind<=95&&!required->second.contains((block.kind-32)/2))block.bytes.clear();
            }
            DeferredTextureSnapshotScope scope;
            std::map<std::pair<uint32_t,uint32_t>,LiveResourceBytes> textures;
            for(auto& block:event.blocks)if(block.kind>=32&&block.kind<=95&&!block.bytes.empty()) {
                const auto count=uint32_t(block.bytes.size());deferred+=count;
                auto source=std::make_shared<const std::vector<uint8_t>>(std::move(block.bytes));
                const auto [found,inserted]=textures.emplace(std::pair{block.address,count},source);
                if(!inserted&&*found->second!=*source)throw std::runtime_error("Captured texture aliases disagree");
                block.deferred_physical_bytes=count;
            }
            event.materialize_texture=scope.Bind([&](uint32_t address,uint32_t count) {
                materialized+=count;const auto source=textures.at({address,count});
                return count>=4096?snapshots.Get(address,*source):source;
            });
            for(auto& block:event.blocks)if(block.kind>=14&&block.kind<=95&&block.bytes.size()>=4096) {
                block.shared_bytes=snapshots.Get(block.address,block.bytes);block.bytes.clear();
            }
            if(auto frame=planner.Push(std::move(event))) {
                if(frame->retained_bytes>512u*1024*1024-retained)
                    throw std::runtime_error("Full replay frame owners exceed 512 MiB diagnostic bound");
                retained+=frame->retained_bytes;frames.push_back(std::move(frame));
            }
        });
    } // Operations and resources must survive planner/snapshot cache destruction.
    api.clear();api.shrink_to_fit();
    std::vector<const LiveOperation*> operations;size_t draws=0;
    for(const auto& frame:frames) {
        size_t frame_bytes=0;
        for(bool shader:{false,true})frame->ForEachResource(shader,[&](std::string_view hash,const LiveResourceBytes& bytes) {
            if(ContentHash(*bytes)!=hash)throw std::runtime_error("Live resource SHA does not match its immutable owner");
            frame_bytes+=bytes->size();
        });
        if(frame_bytes!=frame->retained_bytes)throw std::runtime_error("Live frame retained-byte accounting mismatch");
        for(const auto& operation:frame->operations) {
            if(operation.Kind()=="draw") {
                if(!operation.Draw())throw std::runtime_error("Live producer retained JSON for an ordinary draw");
                ++draws;
            }
            operations.push_back(&operation);
        }
    }
    const auto scene_bytes=Read(config.capture/"scene.json",64u*1024*1024);
    const Json original=Json::parse(scene_bytes);
    if(original.at("version")!=3||original.at("operations").size()!=operations.size())
        throw std::runtime_error("Original scene and live operation counts/version differ");
    for(size_t i=0;i<operations.size();++i) {
        const auto& expected=original.at("operations")[i];const auto& actual=*operations[i];
        if(actual.Event()!=expected.at("event")||actual.Frame()!=expected.at("frame")||actual.Kind()!=expected.at("type").get_ref<const std::string&>())
            throw std::runtime_error("Live operation event/order/frame/type differs at "+std::to_string(i));
    }
    if(canonical_store_scene_fixture) {
        validation::RunNativeCanonicalStoreSceneFixture(config,original);return 0;
    }
    if(fused_packed_atlas_scene_fixture) {
        validation::RunNativeFusedPackedAtlasResolveSceneFixture(config,original);
        std::cout<<"PASS: actual Scene fused packed atlas resolve fixture\n";return 0;
    }
    if(packed_secondary_stencil_scene_fixture) {
        validation::RunNativePackedSecondaryStencilClearSceneFixture(config,original);
        return 0;
    }
    if(direct_packed_color_scene_fixture) {
        validation::RunNativeDirectPackedColorResolveSceneFixture(config,original);
        return 0;
    }
    if(packed_primary_stencil_scene_fixture) {
        validation::RunNativePackedPrimaryStencilClearSceneFixture(config,original);
        return 0;
    }
    if(direct_atlas_resolve_scene_fixture) {
        validation::RunNativeDirectAtlasDepthResolveSceneFixture(config,original);
        return 0;
    }
    if(fused_atlas_clear_scene_fixture) {
        validation::RunNativeFusedAtlasDepthClearSceneFixture(config,original);
        return 0;
    }
    if(!exact_oracle.empty()) {
        std::ifstream oracle(exact_oracle);if(!oracle)throw std::runtime_error("Cannot open exact live oracle");
        std::string line;
        for(const auto& frame:frames) {
            if(!std::getline(oracle,line))throw std::runtime_error("Exact live oracle is missing a frame");
            const auto expected=Json::parse(line);
            if(expected.at("frame")!=frame->frame||expected.at("retained_bytes")!=frame->retained_bytes||
               expected.at("operations")!=frame->operations.ToJson())
                throw std::runtime_error("Full unnormalized live operation oracle differs");
            for(bool shader:{false,true}) {
                const auto& bank=expected.at(shader?"shaders":"payloads");
                if(bank.size()!=frame->ResourceCount(shader))throw std::runtime_error("Exact live oracle resource count differs");
                frame->ForEachResource(shader,[&](std::string_view hash,const LiveResourceBytes& bytes) {
                    const auto& entry=bank.at(std::string(hash));
                    if(entry.at("sha256").get_ref<const std::string&>()!=hash||entry.at("bytes")!=bytes->size())
                        throw std::runtime_error("Exact live oracle resource identity/extent differs");
                });
            }
        }
        if(std::getline(oracle,line))throw std::runtime_error("Exact live oracle contains extra frames");
    }
    if(config.retain_primary_across_disjoint_queries&&!config.retained_d32_composition)
        throw std::runtime_error("Disjoint retained queries require --retained-d32-composition");
    if(config.retained_d32_composition&&(!config.native_fsi||!config.native_readonly_depth_attachment||
       !config.defer_native_queries||!config.unified_gpu_queue||!config.async_gpu_submissions||!config.batch_transfers||
       (bool(config.retained_opaque_writer||config.retained_opaque_catalog)==bool(config.retained_fp24_depth_writer))||
       (config.retained_opaque_writer&&config.retained_opaque_catalog)||config.retained_shared_depth_queries||
       config.native_readonly_depth_skip_edram_barrier||config.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||config.exact_fsi_stores))
        throw std::runtime_error("Retained D32 composition requires one exact primary writer and canonical deferred D32 Native replay");
    if(retained_pool_fixture&&(!config.retained_opaque_catalog||!config.reuse_retained_opaque_resources||
       !config.queue_retained_exports_for_draws||!config.native_fsi||json_adapter||through!=UINT32_MAX||
       !retained_catalog_state_fixture.empty()||retained_partial_fixture||retained_catalog_partial_fixture||
       retained_queued_draw_fixture||retained_pooled_shared_scene_fixture||retained_catalog_prewarm_limit!=kRetainedOpaqueCatalogMaxPipelines))
        throw std::runtime_error("Resource pool Scene fixture requires exclusive complete typed V2 queued replay with resource reuse");
    std::optional<validation::RetainedOpaqueStateFixture> retained_state_fixture;
    if(retained_queued_draw_fixture&&!retained_catalog_state_fixture.empty())
        throw std::runtime_error("Queued export fixture cannot combine altered-input experiments");
    if(!retained_catalog_state_fixture.empty()) {
        if(!config.native_fsi||json_adapter||through!=UINT32_MAX||!wrap_frame_queries||!full_edram_fixture||
           (retained_catalog_state_fixture!="source-switch"&&retained_catalog_prewarm_limit!=kRetainedOpaqueCatalogMaxPipelines)||
           (retained_catalog_state_fixture=="source-switch"&&retained_catalog_prewarm_limit!=0&&retained_catalog_prewarm_limit!=2&&retained_catalog_prewarm_limit!=kRetainedOpaqueCatalogMaxPipelines)||
           retained_partial_fixture||(retained_catalog_partial_fixture&&retained_catalog_state_fixture!="source-switch"))
            throw std::runtime_error("Controlled catalogue state fixture requires complete typed Native replay, full final EDRAM read, wrapped queries and its allowed startup limit");
        retained_state_fixture=validation::MakeRetainedOpaqueStateFixture(operations,retained_catalog_state_fixture);
        operations=retained_state_fixture->operations;
    }
    Json provenance={{"api_sha256",capture_hash},{"original_scene_sha256",ContentHash(scene_bytes)},
        {"mode",json_adapter?"live_plan_materialized_json":"live_plan_typed"},{"operations",operations.size()},
        {"ordinary_draws",draws},{"frames",frames.size()},{"retained_bytes",retained},
        {"deferred_texture_bytes",deferred},{"materialized_texture_bytes",materialized},
        {"live_provenance_unmodified",true},{"planner_destroyed_before_replay",true},{"exact_unnormalized_oracle_checked",!exact_oracle.empty()}};
    if(config.native_completed_frame_capture) {
        provenance.erase("api_sha256");provenance["native_capture_sha256"]=capture_hash;
        provenance["mode"]=json_adapter?"native_completed_frame_json":"native_completed_frame_typed";
        provenance["planner_destroyed_before_replay"]=false;
        provenance["planner_reconstruction"]=false;
        provenance["retained_bytes_scope"]="distinct saved CPU and shader resources";
        provenance["synchronous_query_retirement"]=true;
        provenance["valid_for_live_timing"]=false;
    }
    if(retained_state_fixture) {provenance["live_provenance_unmodified"]=false;
        provenance["retained_opaque_state_fixture"]=retained_state_fixture->metadata;}
    if(validate_only){provenance["gpu_executed"]=false;std::cout<<provenance.dump(2)<<'\n';return 0;}
#ifdef GOW3_NATIVE_LIVE_REPLAY_CPU_ONLY
    throw std::runtime_error("This isolated CPU build only supports --validate-only");
#else
    fs::create_directories(config.output);
    {std::ofstream file(config.output/"live-input.json");file<<provenance.dump(2)<<'\n';
        if(!file)throw std::runtime_error("Cannot save live replay provenance");}
    config.payload_snapshot=[&](const std::string& hash) {
        for(const auto& frame:frames)if(const auto* resource=frame->FindResource(hash,false))return *resource;
        throw std::runtime_error("Live replay requested absent CPU payload "+hash);
    };
    // Ordinary replay continues to use the original captured-source .spv
    // files. The explicit experiment prepares one independently keyed candidate
    // before Scene creation and verifies canonical compiled bytes still match.
    std::map<std::string,std::shared_ptr<const CompiledNativeShader>> hoist_fsi_prepared;
    Json hoist_fsi_preparation=Json::array();uint64_t hoist_fsi_prepared_bytes=0;
    if(config.fsi_hoist_mode!=NativeFsiHoistMode::Disabled) {
        if(config.exact_fsi_stores||!config.native_fsi||config.implicit_fsi_barriers||config.builtin_atomic_depth||config.builtin_readonly_depth_queries||
           config.opaque_color_overwrite||config.forward_control_flow||config.forward_vertex_control_flow||config.forward_pixel_catalog||
           config.specialize_fsi_pipeline_state||config.fsi_state_forward_cfg||config.retained_opaque_writer||config.retained_opaque_catalog||
           config.retained_fp24_depth_writer||config.retained_shared_depth_queries||config.attachmentless_fsi||
           config.queue_retained_exports_for_draws||config.reuse_retained_opaque_resources)
            throw std::runtime_error("Hoisted FSI stores require their separate non-null Native provider");
        NativeShaderCompiler canonical;NativeFsiHoistCompiler derived(canonical,config.fsi_hoist_mode);
        for(const auto* operation:operations)if(const auto* draw=operation->Draw())if(draw->pixel_shader) {
            const auto& resource=*draw->pixel_shader;const auto hash=std::string(resource.hash.View());
            if(hoist_fsi_prepared.contains(hash))continue;
            if(!resource.owner||ContentHash(*resource.owner)!=hash||hoist_fsi_prepared.size()>=1024)
                throw std::runtime_error("Hoisted FSI replay source ownership/identity/bound failed");
            const auto original=canonical.Get(*resource.owner,NativeShaderMode::FragmentInterlock);
            const auto original_bytes=Read(config.compiled/(hash+".fsi.spv"),16u*1024*1024);
            if(original->source_sha256!=hash||original_bytes.size()!=original->spirv.size()*4||
               std::memcmp(original_bytes.data(),original->spirv.data(),original_bytes.size()))
                throw std::runtime_error("Hoisted FSI canonical module differs from the original compiled source");
            auto candidate=derived.Get(*resource.owner);
            if(candidate->source_sha256!=hash||candidate->stage!=NativeShaderStage::Pixel||
               candidate->mode!=NativeShaderMode::FragmentInterlock||candidate->specialization_mask!=original->specialization_mask||
               candidate->spirv.size()*4>64u*1024*1024-hoist_fsi_prepared_bytes)
                throw std::runtime_error("Hoisted FSI replay derived identity/ABI/bound failed");
            hoist_fsi_prepared_bytes+=candidate->spirv.size()*4;
            hoist_fsi_preparation.push_back({{"source",hash},{"canonical_spirv_sha256",ContentHash(original_bytes)},
                {"derived_spirv_sha256",ContentHash({reinterpret_cast<const uint8_t*>(candidate->spirv.data()),candidate->spirv.size()*4})},
                {"specialization_mask",candidate->specialization_mask},{"spirv_bytes",candidate->spirv.size()*4}});
            hoist_fsi_prepared.emplace(hash,std::move(candidate));
        }
        config.fsi_hoist_version=NativeFsiHoistCompiler::CacheVersion(config.fsi_hoist_mode);
        config.fsi_shader=[&hoist_fsi_prepared](const std::string& hash) {
            const auto& shader=*hoist_fsi_prepared.at(hash);std::vector<uint8_t> bytes(shader.spirv.size()*4);
            std::memcpy(bytes.data(),shader.spirv.data(),bytes.size());return bytes;
        };
        // Null-PS FSI and vertex modules continue using the original compiled
        // files. No compiler/provider or source owner escapes into the Scene.
        std::ofstream proof(config.output/"fsi-hoist-preparation.json");
        proof<<Json({{"mode",unsigned(config.fsi_hoist_mode)},{"version",config.fsi_hoist_version},{"prepared_sources",hoist_fsi_prepared.size()},
            {"prepared_spirv_bytes",hoist_fsi_prepared_bytes},{"canonical_null_pixel_shader",true},
            {"sources",hoist_fsi_preparation}}).dump(2)<<'\n';
        if(!proof)throw std::runtime_error("Cannot retain hoisted FSI preparation proof");
    }
    std::map<std::string,std::shared_ptr<const CompiledNativeShader>> exact_fsi_prepared;
    Json exact_fsi_preparation=Json::array();uint64_t exact_fsi_prepared_bytes=0;
    if(config.exact_fsi_stores) {
        if(!config.native_fsi||config.implicit_fsi_barriers||config.builtin_atomic_depth||config.builtin_readonly_depth_queries||
           config.opaque_color_overwrite||config.forward_control_flow||config.forward_vertex_control_flow||config.forward_pixel_catalog||
           config.specialize_fsi_pipeline_state||config.fsi_state_forward_cfg||config.retained_opaque_writer||config.retained_opaque_catalog||
           config.retained_fp24_depth_writer||config.retained_shared_depth_queries||config.attachmentless_fsi||
           config.queue_retained_exports_for_draws||config.reuse_retained_opaque_resources)
            throw std::runtime_error("Exact FSI stores require their separate non-null Native provider");
        NativeShaderCompiler canonical;NativeFsiExactStoresCompiler derived(canonical);
        for(const auto* operation:operations)if(const auto* draw=operation->Draw())if(draw->pixel_shader) {
            const auto& resource=*draw->pixel_shader;const auto hash=std::string(resource.hash.View());
            if(exact_fsi_prepared.contains(hash))continue;
            if(!resource.owner||ContentHash(*resource.owner)!=hash||exact_fsi_prepared.size()>=1024)
                throw std::runtime_error("Exact FSI replay source ownership/identity/bound failed");
            const auto original=canonical.Get(*resource.owner,NativeShaderMode::FragmentInterlock);
            const auto original_bytes=Read(config.compiled/(hash+".fsi.spv"),16u*1024*1024);
            if(original->source_sha256!=hash||original_bytes.size()!=original->spirv.size()*4||
               std::memcmp(original_bytes.data(),original->spirv.data(),original_bytes.size()))
                throw std::runtime_error("Exact FSI canonical module differs from the original compiled source");
            auto candidate=derived.Get(*resource.owner);
            if(candidate->source_sha256!=hash||candidate->stage!=NativeShaderStage::Pixel||
               candidate->mode!=NativeShaderMode::FragmentInterlock||candidate->specialization_mask!=original->specialization_mask||
               candidate->spirv.size()*4>64u*1024*1024-exact_fsi_prepared_bytes)
                throw std::runtime_error("Exact FSI replay derived identity/ABI/bound failed");
            exact_fsi_prepared_bytes+=candidate->spirv.size()*4;
            exact_fsi_preparation.push_back({{"source",hash},{"canonical_spirv_sha256",ContentHash(original_bytes)},
                {"derived_spirv_sha256",ContentHash({reinterpret_cast<const uint8_t*>(candidate->spirv.data()),candidate->spirv.size()*4})},
                {"specialization_mask",candidate->specialization_mask},{"spirv_bytes",candidate->spirv.size()*4}});
            exact_fsi_prepared.emplace(hash,std::move(candidate));
        }
        config.exact_fsi_stores_version=NativeFsiExactStoresCompiler::CacheVersion();
        config.fsi_shader=[&exact_fsi_prepared](const std::string& hash) {
            const auto& shader=*exact_fsi_prepared.at(hash);std::vector<uint8_t> bytes(shader.spirv.size()*4);
            std::memcpy(bytes.data(),shader.spirv.data(),bytes.size());return bytes;
        };
        // Null-PS FSI and vertex modules continue using the original compiled
        // files. No compiler/provider or source owner escapes into the Scene.
        std::ofstream proof(config.output/"exact-fsi-stores-preparation.json");
        proof<<Json({{"version",config.exact_fsi_stores_version},{"prepared_sources",exact_fsi_prepared.size()},
            {"prepared_spirv_bytes",exact_fsi_prepared_bytes},{"canonical_null_pixel_shader",true},
            {"sources",exact_fsi_preparation}}).dump(2)<<'\n';
        if(!proof)throw std::runtime_error("Cannot retain exact FSI preparation proof");
    }
    std::array<std::shared_ptr<const CompiledNativeShader>,2> fp24_vertices;
    std::shared_ptr<const CompiledNativeShader> fp24_builtin;
    if(config.retained_fp24_depth_writer) {
        if(!config.native_fsi||!config.unified_gpu_queue||!config.async_gpu_submissions||!config.batch_transfers||
           (config.native_readonly_depth_attachment&&!config.retained_d32_composition)||config.retained_opaque_writer||config.retained_opaque_catalog||
           config.retained_shared_depth_queries||config.reuse_retained_opaque_resources||config.queue_retained_exports_for_draws||
           config.attachmentless_fsi||config.specialize_fsi_pipeline_state||config.fsi_state_forward_cfg||config.implicit_fsi_barriers||
           config.builtin_atomic_depth||config.builtin_readonly_depth_queries||config.opaque_color_overwrite||config.forward_control_flow||
           config.forward_vertex_control_flow||config.forward_pixel_catalog||config.multi_draw_command_buffers)
            throw std::runtime_error("Retained FP24 writer requires its isolated asynchronous Native replay");
        if(fp24_prewarm_limit!=0&&fp24_prewarm_limit!=2&&fp24_prewarm_limit!=4)
            throw std::runtime_error("Retained FP24 startup limit must be0,2 or4");
        NativeShaderCompiler preparation;
        for(size_t i=0;i<fp24_vertices.size();++i)for(const auto& frame:frames)
            if(!fp24_vertices[i])if(const auto* source=frame->FindResource(kNativeRetainedFp24DepthSources[i].source,true))
                fp24_vertices[i]=preparation.Get(**source,NativeShaderMode::Raster);
        fp24_builtin=preparation.GetDepthOnlyFragmentInterlock();
        for(size_t i=0;i<fp24_vertices.size();++i) {
            const auto expected=Read(config.compiled/(std::string(kNativeRetainedFp24DepthSources[i].source)+".spv"),16u*1024*1024);
            if(!fp24_vertices[i]||expected.size()!=fp24_vertices[i]->spirv.size()*4||
               std::memcmp(expected.data(),fp24_vertices[i]->spirv.data(),expected.size())||
               ContentHash(expected)!=kNativeRetainedFp24DepthSources[i].spirv)
                throw std::runtime_error("Retained FP24 canonical original vertex source/module pin differs");
        }
        const auto builtin=Read(config.compiled/"depth-only.fsi.spv",16u*1024*1024);
        if(!fp24_builtin||builtin.size()!=fp24_builtin->spirv.size()*4||std::memcmp(builtin.data(),fp24_builtin->spirv.data(),builtin.size()))
            throw std::runtime_error("Retained FP24 canonical original builtin module differs");
    } else if(fp24_scene_fixture||fp24_prewarm_limit!=4)
        throw std::runtime_error("Retained FP24 diagnostic controls require the candidate flag");
    std::shared_ptr<const CompiledNativeShader> retained_vertex,retained_pixel;
    if(retained_partial_fixture&&!config.retained_opaque_writer)
        throw std::runtime_error("Partial retained ownership fixture requires --retained-opaque-writer");
    if(config.retained_opaque_writer) {
        if(!config.native_fsi||!config.unified_gpu_queue||!config.async_gpu_submissions||(config.native_readonly_depth_attachment&&!config.retained_d32_composition)||
           config.attachmentless_fsi||config.specialize_fsi_pipeline_state||config.fsi_state_forward_cfg||config.implicit_fsi_barriers||
           config.builtin_atomic_depth||config.builtin_readonly_depth_queries||config.opaque_color_overwrite||config.forward_control_flow||
           config.forward_vertex_control_flow||config.forward_pixel_catalog)
            throw std::runtime_error("Retained writer replay requires the exclusive asynchronous Native path");
        NativeShaderCompiler preparation;
        for(const auto& frame:frames) {
            if(!retained_vertex)if(const auto* resource=frame->FindResource(kRetainedOpaqueVertex,true))
                retained_vertex=preparation.Get(**resource,NativeShaderMode::Raster);
            if(!retained_pixel)if(const auto* resource=frame->FindResource(kRetainedOpaquePixel,true)) {
                retained_pixel=preparation.Get(**resource,NativeShaderMode::FragmentInterlock);
                config.retained_opaque_program=TryPrepareNativeRetainedOpaque(preparation,**resource);
            }
        }
        const auto matches=[&](const CompiledNativeShader& shader,const fs::path& path) {
            const auto expected=Read(path,16u*1024*1024);
            return expected.size()==shader.spirv.size()*4&&std::memcmp(expected.data(),shader.spirv.data(),expected.size())==0;
        };
        if(!retained_vertex||!retained_pixel||!config.retained_opaque_program||
           !matches(*retained_vertex,config.compiled/(std::string(kRetainedOpaqueVertex)+".spv"))||
           !matches(*retained_pixel,config.compiled/(std::string(kRetainedOpaquePixel)+".fsi.spv")))
            throw std::runtime_error("Retained writer original supplied canonical VS/PS oracle differs or candidate refused");
        if(retained_partial_fixture)validation::RetainedOpaquePartialOwnedFixture(config,operations,retained_vertex->spirv,retained_pixel->spirv,full_edram_fixture);
    }
    struct RetainedCatalogPrepared {
        NativeRetainedOpaqueCatalogPair pair;
        std::shared_ptr<const CompiledNativeShader> vertex,pixel;
    };
    std::vector<RetainedCatalogPrepared> retained_catalog_prepared;
    if(config.retained_opaque_catalog) {
        if(config.retained_opaque_writer||!config.native_fsi||!config.unified_gpu_queue||!config.async_gpu_submissions||
           (config.native_readonly_depth_attachment&&!config.retained_d32_composition)||config.attachmentless_fsi||config.specialize_fsi_pipeline_state||
           config.fsi_state_forward_cfg||config.implicit_fsi_barriers||config.builtin_atomic_depth||config.builtin_readonly_depth_queries||
           config.opaque_color_overwrite||config.forward_control_flow||config.forward_vertex_control_flow||config.forward_pixel_catalog)
            throw std::runtime_error("Retained catalog replay requires the exclusive asynchronous Native path");
        NativeShaderCompiler preparation;auto catalog=std::make_shared<NativeRetainedOpaqueCatalog>();
        const auto source=[&](std::string_view digest)->std::span<const uint8_t> {
            for(const auto& frame:frames)if(const auto* resource=frame->FindResource(digest,true))return **resource;
            throw std::runtime_error("Actual retained catalog source is absent");
        };
        const auto matches=[&](const CompiledNativeShader& shader,const fs::path& path) {
            const auto expected=Read(path,16u*1024*1024);
            return expected.size()==shader.spirv.size()*4&&std::memcmp(expected.data(),shader.spirv.data(),expected.size())==0;
        };
        for(const auto& pair:kRetainedOpaqueCatalogPairs) {
            auto vertex=preparation.Get(source(pair.vertex),NativeShaderMode::Raster);
            auto pixel=preparation.Get(source(pair.pixel),NativeShaderMode::FragmentInterlock);
            auto program=TryPrepareNativeRetainedOpaqueCatalog(preparation,source(pair.pixel));
            if(!program||program->vertex_sha256!=pair.vertex||program->source_sha256!=pair.pixel||
               !matches(*vertex,config.compiled/(std::string(pair.vertex)+".spv"))||
               !matches(*pixel,config.compiled/(std::string(pair.pixel)+".fsi.spv")))
                throw std::runtime_error("Retained catalog canonical VS/PS oracle differs or candidate source refused");
            catalog->programs.push_back(std::move(program));
            retained_catalog_prepared.push_back({pair,std::move(vertex),std::move(pixel)});
        }
        config.retained_opaque_catalog_programs=std::move(catalog);
        if(retained_pool_fixture) {
            std::vector<validation::RetainedOpaqueCatalogFixtureSource> sources;
            for(const auto& item:retained_catalog_prepared)sources.push_back({item.pair,item.vertex->spirv,item.pixel->spirv});
            validation::RetainedResourcePoolSceneFixture(config,operations,sources);
            // Dedicated actual owned fixture. Whole-capture image/source-switch
            // parity remains a separate run with its existing coverage gates.
            std::cout<<"Retained resource pool owned Scene fixture passed; whole-capture replay not executed\n";
            return 0;
        }
        if(retained_queued_draw_fixture) {
            std::vector<validation::RetainedOpaqueCatalogFixtureSource> sources;
            for(const auto& item:retained_catalog_prepared)sources.push_back({item.pair,item.vertex->spirv,item.pixel->spirv});
            validation::RetainedQueuedDrawExportFixture(config,operations,sources);
        }
        if(retained_catalog_partial_fixture) {
            if(retained_catalog_prewarm_limit!=kRetainedOpaqueCatalogMaxPipelines)
                throw std::runtime_error("Partial catalogue lifetime fixture requires all six startup PSOs");
            std::vector<validation::RetainedOpaqueCatalogFixtureSource> sources;
            for(const auto& item:retained_catalog_prepared)sources.push_back({item.pair,item.vertex->spirv,item.pixel->spirv});
            const bool source_switch=retained_catalog_state_fixture=="source-switch";
            const uint32_t partial_event=source_switch?Number(retained_state_fixture->metadata.at("records").back().at("event").dump(),UINT32_MAX):UINT32_MAX;
            validation::RetainedOpaqueCatalogPartialOwnedFixture(config,operations,sources,full_edram_fixture,partial_event,source_switch);
        }
    }else if(retained_catalog_prewarm_limit!=kRetainedOpaqueCatalogMaxPipelines||retained_catalog_partial_fixture||retained_queued_draw_fixture||retained_pool_fixture)
        throw std::runtime_error("Retained catalog prewarm limit requires explicit catalog experiment");
    std::shared_ptr<const CompiledNativeShader> opaque_vertex,opaque_pixel;
    if(config.opaque_color_overwrite) {
        if(!config.native_fsi||config.implicit_fsi_barriers)
            throw std::runtime_error("Opaque replay requires ordinary packed Native FSI");
        const auto source=[&](std::string_view hash) -> std::span<const uint8_t> {
            for(const auto& frame:frames)if(const auto* resource=frame->FindResource(hash,true))return **resource;
            throw std::runtime_error("Opaque replay source missing: "+std::string(hash));
        };
        NativeShaderCompiler preparation;
        const auto vertex_source=source(kOpaqueColorOverwriteVertex),pixel_source=source(kOpaqueColorOverwritePixel);
        opaque_vertex=preparation.Get(vertex_source,NativeShaderMode::Raster);
        opaque_pixel=preparation.Get(pixel_source,NativeShaderMode::FragmentInterlock);
        const auto matches=[&](const CompiledNativeShader& shader,const fs::path& path) {
            const auto expected=Read(path,16u*1024*1024);
            return expected.size()==shader.spirv.size()*4&&std::memcmp(expected.data(),shader.spirv.data(),expected.size())==0;
        };
        if(!matches(*opaque_vertex,config.compiled/(std::string(kOpaqueColorOverwriteVertex)+".spv"))||
           !matches(*opaque_pixel,config.compiled/(std::string(kOpaqueColorOverwritePixel)+".fsi.spv")))
            throw std::runtime_error("Opaque replay canonical compiler differs from supplied Native oracle modules");
        config.opaque_color_overwrite_program=TryPrepareOpaqueColorOverwrite(preparation,pixel_source);
        if(!config.opaque_color_overwrite_program)throw std::runtime_error("Opaque replay compiler/source proof refused");
    }
    std::shared_ptr<const CompiledNativeShader> forward_vertex,forward_pixel;
    if(config.forward_control_flow) {
        if(!config.native_fsi||config.implicit_fsi_barriers||config.opaque_color_overwrite)
            throw std::runtime_error("Forward-CFG replay requires ordinary packed Native FSI");
        const auto source=[&](std::string_view hash) -> std::span<const uint8_t> {
            for(const auto& frame:frames)if(const auto* resource=frame->FindResource(hash,true))return **resource;
            throw std::runtime_error("Forward-CFG replay source missing: "+std::string(hash));
        };
        NativeShaderCompiler preparation;
        const auto vertex_source=source(kForwardControlFlowVertex),pixel_source=source(kForwardControlFlowPixel);
        forward_vertex=preparation.Get(vertex_source,NativeShaderMode::Raster);
        forward_pixel=preparation.Get(pixel_source,NativeShaderMode::FragmentInterlock);
        const auto matches=[&](const CompiledNativeShader& shader,const fs::path& path) {
            const auto expected=Read(path,16u*1024*1024);
            return expected.size()==shader.spirv.size()*4&&std::memcmp(expected.data(),shader.spirv.data(),expected.size())==0;
        };
        if(!matches(*forward_vertex,config.compiled/(std::string(kForwardControlFlowVertex)+".spv"))||
           !matches(*forward_pixel,config.compiled/(std::string(kForwardControlFlowPixel)+".fsi.spv")))
            throw std::runtime_error("Forward-CFG replay canonical compiler differs from supplied Native oracle modules");
        config.forward_control_flow_program=TryPrepareForwardControlFlow(preparation,pixel_source);
        if(!config.forward_control_flow_program)throw std::runtime_error("Forward-CFG replay compiler/source proof refused");
    }
    struct ForwardVertexPrepared {NativeFsiPipelineRecipe recipe;std::vector<uint32_t> pixel;};
    std::shared_ptr<const CompiledNativeShader> pinned_vertex;
    std::vector<ForwardVertexPrepared> forward_vertex_prepared;
    if(config.forward_vertex_control_flow) {
        if(!config.native_fsi||config.implicit_fsi_barriers||config.opaque_color_overwrite||config.forward_control_flow||config.builtin_atomic_depth||forward_vertex_recipes.empty())
            throw std::runtime_error("Forward-VS replay requires ordinary packed Native FSI and --forward-vertex-recipes=source-cache-directory");
        NativeShaderCompiler preparation;
        for(const auto& frame:frames)if(const auto* resource=frame->FindResource(kForwardVertexSource,true)) {
            pinned_vertex=preparation.Get(**resource,NativeShaderMode::Raster);
            config.forward_vertex_program=TryPrepareForwardVertex(preparation,**resource);break;
        }
        if(!pinned_vertex||!config.forward_vertex_program)throw std::runtime_error("Forward-VS captured source/compiler proof refused");
        const auto expected=Read(config.compiled/(std::string(kForwardVertexSource)+".spv"),16u*1024*1024);
        if(expected.size()!=pinned_vertex->spirv.size()*4||std::memcmp(expected.data(),pinned_vertex->spirv.data(),expected.size())!=0)
            throw std::runtime_error("Forward-VS canonical compiler differs from supplied Native oracle");
        NativePipelineRecipeCache observed(forward_vertex_recipes,preparation.CacheVersion());
        for(const auto& item:observed.Load()) {
            if(forward_vertex_prepared.size()>=kForwardVertexMaxPipelines)break;
            if(!ForwardVertexAllowed(item.recipe.vertex,item.recipe.raster)||item.recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||item.recipe.depth_format!=-1)continue;
            const auto path=config.compiled/(item.recipe.pixel?*item.recipe.pixel+".fsi.spv":"depth-only.fsi.spv");
            if(!fs::exists(path))continue; // No uncaptured source/module is compiled.
            const auto bytes=Read(path,16u*1024*1024);
            if(bytes.size()<20||bytes.size()%4)throw std::runtime_error("Invalid original Native pixel oracle");
            std::vector<uint32_t> pixel(bytes.size()/4);std::memcpy(pixel.data(),bytes.data(),bytes.size());
            forward_vertex_prepared.push_back({item.recipe,std::move(pixel)});
        }
        if(forward_vertex_prepared.empty())throw std::runtime_error("Forward-VS replay has no captured matching recipes");
    }
    struct CatalogPrepared {NativeFsiPipelineRecipe recipe;std::vector<uint32_t> vertex,pixel;};
    std::vector<CatalogPrepared> catalog_prepared;
    if(config.forward_pixel_catalog) {
        if(!config.native_fsi||config.implicit_fsi_barriers||config.opaque_color_overwrite||config.forward_control_flow||config.forward_vertex_control_flow||config.builtin_atomic_depth||catalog_recipes.empty())
            throw std::runtime_error("Forward pixel catalog replay requires ordinary packed Native FSI and --forward-pixel-catalog-recipes=source-cache-directory");
        NativeShaderCompiler preparation;auto catalog=std::make_shared<ForwardPixelCatalog>();
        for(const auto source:kForwardPixelCatalogSources) {
            std::shared_ptr<const std::vector<uint8_t>> container;
            for(const auto& frame:frames)if(const auto* resource=frame->FindResource(source,true)){container=*resource;break;}
            if(!container)throw std::runtime_error("Catalog source has no actual capture coverage: "+std::string(source));
            auto canonical=preparation.Get(*container,NativeShaderMode::FragmentInterlock);
            const auto original=Read(config.compiled/(std::string(source)+".fsi.spv"),16u*1024*1024);
            if(original.size()!=canonical->spirv.size()*4||std::memcmp(original.data(),canonical->spirv.data(),original.size())!=0)
                throw std::runtime_error("Catalog canonical pixel differs from supplied Native oracle");
            auto program=TryPrepareForwardPixelCatalog(preparation,*container);
            if(!program)throw std::runtime_error("Actual catalog source/compiler proof refused: "+std::string(source));
            catalog->programs.emplace(std::string(source),std::move(program));
        }
        NativePipelineRecipeCache observed(catalog_recipes,preparation.CacheVersion());
        const auto words=[&](const fs::path& path) {
            const auto bytes=Read(path,16u*1024*1024);if(bytes.size()<20||bytes.size()%4)throw std::runtime_error("Invalid original shader oracle extent");
            std::vector<uint32_t> code(bytes.size()/4);std::memcpy(code.data(),bytes.data(),bytes.size());
            if(code[0]!=0x07230203)throw std::runtime_error("Invalid original shader oracle magic");return code;
        };
        for(const auto& item:observed.Load()) {
            if(catalog_prepared.size()>=kForwardPixelCatalogMaxPipelines)break;
            if(!item.recipe.pixel||!catalog->programs.contains(*item.recipe.pixel)||!ForwardPixelCatalogAllowed(*item.recipe.pixel,item.recipe.raster)||
                item.recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||item.recipe.depth_format!=-1)continue;
            const auto vs=config.compiled/(item.recipe.vertex+".spv"),ps=config.compiled/(*item.recipe.pixel+".fsi.spv");
            if(!fs::exists(vs)||!fs::exists(ps))continue; // Original modules only; no missing VS compilation.
            catalog_prepared.push_back({item.recipe,words(vs),words(ps)});
        }
        if(catalog_prepared.empty())throw std::runtime_error("Catalog has no actual observed captured recipes");
        config.forward_pixel_catalog_programs=std::move(catalog);
    }
    std::vector<NativeFsiPipelineRecipe> early_prepared;
    std::shared_ptr<const CompiledNativeShader> early_vertex,early_pixel;
    std::shared_ptr<const NativeEarlyDepthProgram> early_program;
    if(config.native_early_depth) {
        if(early_recipes.empty())throw std::runtime_error("Native early-depth replay requires --early-depth-recipes=canonical-cache-directory");
        NativeShaderCompiler preparation;
        for(const auto& frame:frames) {
            if(!early_vertex)if(const auto* resource=frame->FindResource(kNativeEarlyDepthVertexSource,true))
                early_vertex=preparation.Get(**resource,NativeShaderMode::Raster);
            if(!early_pixel)if(const auto* resource=frame->FindResource(kNativeEarlyDepthPixelSource,true))
                early_pixel=preparation.Get(**resource,NativeShaderMode::FragmentInterlock);
        }
        if(!early_vertex||!early_pixel||!NativeEarlyDepthVertexCertified(early_vertex->spirv,kNativeEarlyDepthVertexSource,preparation.CacheVersion()))
            throw std::runtime_error("Native early-depth original source/module certificate refused");
        early_program=TryPrepareNativeEarlyDepthShader(early_pixel->spirv,kNativeEarlyDepthPixelSource,preparation.CacheVersion());
        if(!early_program)throw std::runtime_error("Native early-depth original material certificate refused");
        NativePipelineRecipeCache observed(early_recipes,preparation.CacheVersion());
        for(const auto& item:observed.Load()) {
            if(early_prepared.size()>=8)break;
            if(item.recipe.pixel&&*item.recipe.pixel==kNativeEarlyDepthPixelSource&&item.recipe.vertex==kNativeEarlyDepthVertexSource)
                early_prepared.push_back(item.recipe);
        }
        if(early_prepared.empty())throw std::runtime_error("Native early-depth capture has no matching original recipes");
    }
    struct EarlyCatalogPrepared {
        NativeFsiPipelineRecipe recipe;
        std::shared_ptr<const CompiledNativeShader> vertex,pixel;
        std::shared_ptr<const NativeEarlyDepthProgram> program;
    };
    std::vector<EarlyCatalogPrepared> early_catalog_prepared;
    if(config.native_early_depth_catalog) {
        NativeShaderCompiler preparation;
        std::map<std::string,std::shared_ptr<const CompiledNativeShader>,std::less<>> originals;
        for(const auto& pin:NativeEarlyDepthCatalogPins())for(const auto& frame:frames) {
            for(const auto source:{pin.vertex_source,pin.pixel_source}) {
                if(originals.contains(std::string(source)))continue;
                if(const auto* resource=frame->FindResource(source,true))
                    originals.emplace(std::string(source),preparation.Get(**resource,
                        source==pin.vertex_source?NativeShaderMode::Raster:NativeShaderMode::FragmentInterlock));
            }
        }
        std::map<std::string,std::shared_ptr<const NativeEarlyDepthProgram>,std::less<>> programs;
        for(const auto& pin:NativeEarlyDepthCatalogPins()) {
            const auto vs=originals.find(std::string(pin.vertex_source)),ps=originals.find(std::string(pin.pixel_source));
            if(vs==originals.end()||ps==originals.end()||
               !NativeEarlyDepthCatalogVertexCertified(vs->second->spirv,pin.vertex_source,preparation.CacheVersion()))
                throw std::runtime_error("Early-depth catalogue captured original vertex certificate refused");
            auto program=TryPrepareNativeEarlyDepthCatalogShader(ps->second->spirv,pin.pixel_source,preparation.CacheVersion());
            if(!program)throw std::runtime_error("Early-depth catalogue captured original material certificate refused");
            programs.emplace(std::string(pin.pixel_source),std::move(program));
        }
        NativePipelineRecipeCache observed(early_recipes,preparation.CacheVersion());
        for(const auto& item:observed.Load()) {
            if(early_catalog_prepared.size()>=8)break;
            if(!item.recipe.pixel||!NativeEarlyDepthCatalogPair(item.recipe.vertex,*item.recipe.pixel))continue;
            early_catalog_prepared.push_back({item.recipe,originals.at(item.recipe.vertex),
                originals.at(*item.recipe.pixel),programs.at(*item.recipe.pixel)});
        }
        if(early_catalog_prepared.empty())throw std::runtime_error("Early-depth catalogue has no observed original recipes");
    }
    std::vector<NativeFsiPipelineRecipe> readonly_prepared;
    std::shared_ptr<const CompiledNativeShader> readonly_vertex,readonly_pixel;
    if(config.builtin_readonly_depth_queries||config.native_readonly_depth_attachment||config.retained_shared_depth_queries) {
        if(unsigned(config.builtin_readonly_depth_queries)+unsigned(config.native_readonly_depth_attachment)+unsigned(config.retained_shared_depth_queries)>1)
            throw std::runtime_error("Select one readonly query experiment");
        if(config.retained_shared_depth_queries&&(!config.retained_opaque_catalog||!config.defer_native_queries||
           !config.unified_gpu_queue||!config.async_gpu_submissions||!config.batch_transfers))
            throw std::runtime_error("Retained shared queries require asynchronous deferred Native V2");
        if(config.native_readonly_depth_attachment&&config.specialize_fsi_pipeline_state&&!config.fsi_state_material_composition)
            throw std::runtime_error("Native D32 and broad FSI state are separate experiments");
        if(!config.native_fsi||config.attachmentless_fsi||config.implicit_fsi_barriers||
           (config.builtin_atomic_depth&&!(config.packed_depth_render_pass_groups&&config.native_readonly_depth_attachment))||
           config.opaque_color_overwrite||config.forward_control_flow||config.forward_vertex_control_flow||config.forward_pixel_catalog||readonly_recipes.empty())
            throw std::runtime_error("Read-only query replay requires ordinary packed Native and --readonly-query-recipes=canonical-cache-directory");
        NativeShaderCompiler preparation;
        for(const auto& frame:frames)if(const auto* resource=frame->FindResource(kReadonlyQueryVertexSource,true)) {
            readonly_vertex=preparation.Get(**resource,NativeShaderMode::Raster);break;
        }
        readonly_pixel=preparation.GetDepthOnlyFragmentInterlock();
        const auto matches=[&](const CompiledNativeShader& shader,const fs::path& path) {
            const auto expected=Read(path,16u*1024*1024);
            return expected.size()==shader.spirv.size()*4&&std::memcmp(expected.data(),shader.spirv.data(),expected.size())==0;
        };
        if(!readonly_vertex||!AuditedReadonlyQueryVertex(kReadonlyQueryVertexSource,
            {reinterpret_cast<const uint8_t*>(readonly_vertex->spirv.data()),readonly_vertex->spirv.size()*4})||
           !matches(*readonly_vertex,config.compiled/(std::string(kReadonlyQueryVertexSource)+".spv"))||
           !matches(*readonly_pixel,config.compiled/"depth-only.fsi.spv")||!TryBuiltinReadonlyQuery(readonly_pixel->spirv))
            throw std::runtime_error("Read-only query actual source/canonical module oracle proof refused");
        NativePipelineRecipeCache observed(readonly_recipes,preparation.CacheVersion());
        for(const auto& item:observed.Load()) {
            if(readonly_prepared.size()>=128)break;
            if(item.recipe.vertex==kReadonlyQueryVertexSource&&!item.recipe.pixel&&
               item.recipe.raster[3]==uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST)&&!item.recipe.raster[5]&&
               item.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&item.recipe.depth_format==-1&&
               (!(config.native_readonly_depth_attachment||config.retained_shared_depth_queries)||
                item.recipe.raster==std::array<uint32_t,6>{uint32_t(plume::RenderCullMode::BACK),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE),1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0}))
                readonly_prepared.push_back(item.recipe);
        }
        if(readonly_prepared.empty())throw std::runtime_error("Read-only query capture has no matching actual canonical recipes");
    }
    struct FsiStatePrepared {NativeFsiStateRecipe recipe;std::vector<uint32_t> vertex,pixel;};
    std::vector<FsiStatePrepared> fsi_state_prepared;
    // Both caches outlive Scene and the atomic pointers retained by its coverage map.
    std::shared_ptr<NativeFsiStateRecipeCache> state_observer;
    std::shared_ptr<NativePipelineRecipeCache> state_source_observer;
    if(config.specialize_fsi_pipeline_state||!fsi_state_observe.empty()) {
        if(!config.native_fsi||config.implicit_fsi_barriers||config.builtin_atomic_depth||
           config.builtin_readonly_depth_queries||(config.native_readonly_depth_attachment&&!config.fsi_state_material_composition)||config.opaque_color_overwrite||config.forward_control_flow||
           config.forward_vertex_control_flow||config.forward_pixel_catalog)
            throw std::runtime_error("FSI state replay requires exclusive ordinary Native packed shaders");
        if(config.specialize_fsi_pipeline_state&&(fsi_state_recipes.empty()||fsi_state_sources.empty()))
            throw std::runtime_error("FSI state replay requires exact sidecar and authenticated generic source recipe directory");
        NativeShaderCompiler preparation;
        if(!fsi_state_observe.empty()) {
            if(fs::exists(fsi_state_observe))throw std::runtime_error("FSI state observation output must be new");
            fs::create_directories(fsi_state_observe);
            state_observer=std::make_shared<NativeFsiStateRecipeCache>(fsi_state_observe/"native-fsi-state-recipes"/"v1"/
                FsiStateSpecializationCompilerVersion(),FsiStateSpecializationCompilerVersion());
            state_source_observer=std::make_shared<NativePipelineRecipeCache>(fsi_state_observe/"native-pipeline-recipes"/"v1"/
                preparation.CacheVersion(),preparation.CacheVersion());
            config.observe_fsi_state=[owner=state_observer](const NativeFsiStateRecipe& recipe){return owner->Bind(recipe);};
            config.observe_pipeline=[owner=state_source_observer](ObservedNativePipeline recipe){owner->Observe(std::move(recipe));};
        }
        if(config.specialize_fsi_pipeline_state) {
            NativePipelineRecipeCache observed(fsi_state_sources,preparation.CacheVersion());
            const auto sources=observed.Load();
            const auto encoded=Read(fsi_state_recipes,NativeFsiStateSidecar::kMaxBytes);
            auto sidecar=NativeFsiStateSidecar::Parse(std::string_view(reinterpret_cast<const char*>(encoded.data()),encoded.size()),
                FsiStateSpecializationCompilerVersion());
            auto ranked=sidecar.Authenticated(sources);
            // Filter before the256 bound, captured-source collection and compilation.
            // The original broad/forward catalogs and canonical oracles stay unchanged.
            if(config.fsi_state_material_composition)std::erase_if(ranked,[&config](const auto& record) {
                const auto& r=record.recipe.generic;
                return !FsiStateMaterialCompositionSourceAllowed(r.vertex,r.pixel?std::string_view(*r.pixel):std::string_view{},
                    config.fsi_state_inactive_early_catalog,config.native_early_depth_catalog);
            });
            auto catalog=std::make_shared<FsiStateSpecializationCatalog>();uint64_t bytes=0;
            const auto captured=[&](std::string_view source)->LiveResourceBytes {
                for(const auto& frame:frames)if(const auto* resource=frame->FindResource(source,true))return *resource;
                return {};
            };
            const auto matches=[&](const CompiledNativeShader& shader,const fs::path& path) {
                const auto expected=Read(path,16u*1024*1024);
                return expected.size()==shader.spirv.size()*4&&std::memcmp(expected.data(),shader.spirv.data(),expected.size())==0;
            };
            for(const auto& record:ranked) {
                if(fsi_state_prepared.size()>=kFsiStateMaxPipelines)break;
                if(config.fsi_state_forward_cfg&&!FsiStateForwardCfgAllowed(record.recipe))continue;
                const auto& recipe=record.recipe.generic;const auto vertex=captured(recipe.vertex);
                const auto pixel=recipe.pixel?captured(*recipe.pixel):LiveResourceBytes{};
                if(!vertex||(recipe.pixel&&!pixel))continue; // Uncaptured sources never become parity oracles.
                const auto vs_path=config.compiled/(recipe.vertex+".spv");
                const auto ps_path=config.compiled/(recipe.pixel?*recipe.pixel+".fsi.spv":"depth-only.fsi.spv");
                if(!fs::exists(vs_path)||!fs::exists(ps_path))continue;
                auto canonical_vs=preparation.Get(*vertex,NativeShaderMode::Raster);
                auto canonical_ps=recipe.pixel?preparation.Get(*pixel,NativeShaderMode::FragmentInterlock):preparation.GetDepthOnlyFragmentInterlock();
                if(!matches(*canonical_vs,vs_path)||!matches(*canonical_ps,ps_path))
                    throw std::runtime_error("FSI state canonical compiler differs from original supplied Native oracle");
                const bool ready=recipe.pixel?catalog->programs.contains(*recipe.pixel):bool(catalog->depth_only);
                if(!ready) {
                    auto program=config.fsi_state_forward_cfg?TryPrepareFsiStateForwardCfg(preparation,*pixel):
                        (recipe.pixel?TryPrepareFsiStateSpecialization(preparation,*pixel):TryPrepareDepthOnlyFsiStateSpecialization(preparation));
                    if(!program)throw std::runtime_error("Actual FSI source/compiler restoration proof refused");
                    if(program->spirv.size()*4>16u*1024*1024-bytes)continue;
                    bytes+=program->spirv.size()*4;
                    if(recipe.pixel)catalog->programs.emplace(*recipe.pixel,std::move(program));else catalog->depth_only=std::move(program);
                }
                fsi_state_prepared.push_back({record.recipe,canonical_vs->spirv,canonical_ps->spirv});
            }
            if(fsi_state_prepared.empty())throw std::runtime_error("FSI sidecar has no authenticated captured tuples");
            if(config.fsi_state_material_composition&&catalog->depth_only)
                throw std::runtime_error("Material composition must not prepare a nullPS specialization module");
            config.fsi_state_programs=std::move(catalog);
        }
    }
    struct RetainedPhasePrepared {
        NativeFsiPipelineRecipe recipe;
        std::shared_ptr<const CompiledNativeShader> vertex,pixel;
    };
    std::vector<RetainedPhasePrepared> retained_phase_prepared;
    Json retained_phase_startup;
    if(config.retained_opaque_phase||prewarm_phase_originals) {
        // Compile only the intersection of authenticated observed recipes,
        // closed phase inventory, captured source and supplied canonical oracle.
        // Existing generic and material preparation remains authoritative.
        // The separate originals option is an explicit symmetric fixture;
        // enabling the rendering candidate alone never forces residency.
        NativeShaderCompiler preparation;
        NativePipelineRecipeCache observed(fsi_state_sources,preparation.CacheVersion());
        auto catalog=std::make_shared<NativeRetainedOpaquePhaseCatalog>();
        std::set<std::string> attempted_sources;uint64_t bytes=0,matching=0,eligible_matching=0,policy_excluded=0,uncaptured=0,unavailable=0;
        const auto captured=[&](std::string_view source)->LiveResourceBytes {
            for(const auto& frame:frames)if(const auto* resource=frame->FindResource(source,true))return *resource;
            return {};
        };
        const auto matches=[&](const CompiledNativeShader& shader,const fs::path& path) {
            const auto expected=Read(path,16u*1024*1024);
            return expected.size()==shader.spirv.size()*4&&std::memcmp(expected.data(),shader.spirv.data(),expected.size())==0;
        };
        for(const auto& item:observed.Load()) {
            const auto& recipe=item.recipe;
            if(!recipe.pixel||!NativeRetainedOpaquePhaseCatalogRecipe(recipe.vertex,*recipe.pixel,recipe.raster)||
               recipe.color_format!=int32_t(plume::RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)continue;
            ++matching;
            const bool eligible=NativeRetainedOpaquePhaseRecipe(recipe.vertex,*recipe.pixel,recipe.raster)!=nullptr;
            eligible?++eligible_matching:++policy_excluded;
            // The symmetric original oracle keeps the full65 source/raster
            // inventory. Optional phase work obeys the narrower eligibility.
            if(!eligible&&!prewarm_phase_originals)continue;
            const auto vertex=captured(recipe.vertex),pixel=captured(*recipe.pixel);
            const auto vs_path=config.compiled/(recipe.vertex+".spv"),ps_path=config.compiled/(*recipe.pixel+".fsi.spv");
            if(!vertex||!pixel||!fs::exists(vs_path)||!fs::exists(ps_path)){++uncaptured;continue;}
            auto vs=preparation.Get(*vertex,NativeShaderMode::Raster),ps=preparation.Get(*pixel,NativeShaderMode::FragmentInterlock);
            if(vs->source_sha256!=recipe.vertex||ps->source_sha256!=*recipe.pixel||
               vs->stage!=NativeShaderStage::Vertex||ps->stage!=NativeShaderStage::Pixel||
               !matches(*vs,vs_path)||!matches(*ps,ps_path))
                throw std::runtime_error("Retained opaque phase canonical bytes differ from supplied capture oracle");
            if(config.retained_opaque_phase&&eligible&&attempted_sources.insert(*recipe.pixel).second) {
                auto program=TryPrepareNativeRetainedOpaquePhaseConservative(preparation,*pixel);
                if(program&&program->source_sha256!=*recipe.pixel)
                    throw std::runtime_error("Retained opaque phase producer source identity mismatch");
                const uint64_t size=program?program->spirv.size()*sizeof(uint32_t):0;
                if(program&&catalog->programs.size()<kRetainedOpaquePhaseMaxPrograms&&size<=kRetainedOpaquePhaseMaxModuleBytes-bytes) {
                    bytes+=size;catalog->programs.push_back(std::move(program));
                }
            }
            const bool ready=std::any_of(catalog->programs.begin(),catalog->programs.end(),
                [&](const auto& program){return program&&program->source_sha256==*recipe.pixel;});
            if(config.retained_opaque_phase&&eligible&&!ready){++unavailable;if(!prewarm_phase_originals)continue;}
            if(retained_phase_prepared.size()>=kRetainedOpaquePhaseMaxPipelines)
                throw std::runtime_error("Retained opaque phase exact recipe bound exceeded");
            retained_phase_prepared.push_back({recipe,std::move(vs),std::move(ps)});
        }
        const auto eligible_prepared=std::count_if(retained_phase_prepared.begin(),retained_phase_prepared.end(),
            [](const auto& item){return NativeRetainedOpaquePhaseRecipe(item.recipe.vertex,*item.recipe.pixel,item.recipe.raster)!=nullptr;});
        retained_phase_startup={{"matching_observed_recipes",matching},{"eligible_observed_recipes",eligible_matching},
            {"policy_excluded_recipes",policy_excluded},{"eligible_prepared_recipes",eligible_prepared},{"uncaptured_recipes",uncaptured},
            {"unavailable_program_recipes",unavailable},{"prepared_programs",catalog->programs.size()},
            {"prepared_program_bytes",bytes},{"prepared_recipes",retained_phase_prepared.size()},
            {"maximum_programs",kRetainedOpaquePhaseMaxPrograms},{"maximum_program_bytes",kRetainedOpaquePhaseMaxModuleBytes},
            {"maximum_pipelines",kRetainedOpaquePhaseMaxPipelines},{"canonical_preparation_added",prewarm_phase_originals}};
        config.retained_opaque_phase_programs=std::move(catalog);
    }
    const bool retained_phase_requested=config.retained_opaque_phase;
    const bool retained_requested=config.retained_opaque_writer;
    const bool retained_catalog_requested=config.retained_opaque_catalog;
    const bool retained_shared_queries_requested=config.retained_shared_depth_queries;
    const bool queued_retained_draw_exports=config.queue_retained_exports_for_draws;
    const bool fsi_state_requested=config.specialize_fsi_pipeline_state;
    const bool fsi_material_composition_requested=config.fsi_state_material_composition;
    const bool fsi_inactive_early_catalog_requested=config.fsi_state_inactive_early_catalog;
    const bool fsi_early_catalog_active=config.native_early_depth_catalog;
    std::shared_ptr<const CompiledNativeShader> stencil_material_vertex,stencil_material_pixel;
    const bool stencil_material_native=config.native_stencil_material_cutoff;
    const bool stencil_material_requested=config.replay_stencil_material_mode!=0||stencil_material_native;
    if(stencil_material_native&&config.replay_stencil_material_mode)throw std::runtime_error("Stencil material modes are exclusive");
    if(stencil_material_requested) {
        NativeShaderCompiler canonical;
        for(const auto& frame:frames) {
            if(!stencil_material_vertex)if(const auto* resource=frame->FindResource(kStencilMaterialVertex,true))
                stencil_material_vertex=canonical.Get(**resource,NativeShaderMode::Raster);
            if(!stencil_material_pixel)if(const auto* resource=frame->FindResource(kStencilMaterialPixel,true)) {
                stencil_material_pixel=canonical.Get(**resource,NativeShaderMode::FragmentInterlock);
                const auto program=CompileNativeStencilMaterial(canonical,**resource,stencil_material_native?
                    NativeStencilMaterialMode::Uninstrumented:NativeStencilMaterialMode(config.replay_stencil_material_mode));
                if(stencil_material_native)config.native_stencil_material_program=program;
                else config.replay_stencil_material_program=program;
            }
        }
        if(!stencil_material_vertex||!stencil_material_pixel||!(stencil_material_native?config.native_stencil_material_program:config.replay_stencil_material_program))
            throw std::runtime_error("Stencil material replay requires both exact source containers in the capture");
        for(const auto& item:std::array<std::pair<const CompiledNativeShader*,const char*>,2>{{
                {stencil_material_vertex.get(),".spv"},{stencil_material_pixel.get(),".fsi.spv"}}}) {
            const auto bytes=Read(config.compiled/(item.first->source_sha256+item.second),256u*1024);
            if(bytes.size()!=item.first->spirv.size()*4||std::memcmp(bytes.data(),item.first->spirv.data(),bytes.size()))
                throw std::runtime_error("Stencil material replay original compiled provider differs");
        }
    }
#ifndef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
    if(config.replay_stencil_material_mode)throw std::runtime_error("Stencil material diagnostic not compiled");
#endif
    const bool fragment_census=config.replay_fragment_invocations;
    const bool physical_timestamps=config.gpu_physical_timestamps;
    const bool draw_timestamps=config.gpu_draw_timestamps,opaque_requested=config.opaque_color_overwrite,
        forward_requested=config.forward_control_flow,forward_vertex_requested=config.forward_vertex_control_flow,catalog_requested=config.forward_pixel_catalog;
    const auto output=config.output;
    const bool multi_draw_requested=config.multi_draw_command_buffers;
    const bool descriptor_content_requested=config.cache_draw_descriptors;
    const bool constant_delta_requested=config.delta_constant_uploads;
    const bool pinned_null_constants_requested=config.pinned_null_constant_uploads;
    const bool discard_dummy_requested=config.discard_dummy_fsi_attachment;
    const bool attachmentless_requested=config.attachmentless_fsi;
    const bool readonly_requested=config.builtin_readonly_depth_queries;
    const bool d32_requested=config.native_readonly_depth_attachment;
    const bool page_version_union_requested=config.compact_cpu_texture_version_pages;
    const auto hoist_fsi_requested=config.fsi_hoist_mode;
    const bool exact_fsi_requested=config.exact_fsi_stores;
    if(retained_disjoint_query_scene_fixture) {
        if(!readonly_vertex||!readonly_pixel)
            throw std::runtime_error("Disjoint Scene fixture requires authenticated canonical query vertex/builtin modules");
        std::vector<validation::RetainedOpaqueCatalogFixtureSource> sources;
        for(const auto& item:retained_catalog_prepared)sources.push_back({item.pair,item.vertex->spirv,item.pixel->spirv});
        std::array<std::span<const uint32_t>,2> vertices{};
        if(config.retained_fp24_depth_writer) {
            if(!fp24_vertices[0]||!fp24_vertices[1])throw std::runtime_error("Disjoint Scene fixture lacks both authenticated depth writer sources");
            vertices={fp24_vertices[0]->spirv,fp24_vertices[1]->spirv};
        }
        validation::RetainedDisjointQuerySceneFixture(config,operations,sources,vertices,readonly_vertex->spirv,readonly_pixel->spirv);
        std::cout<<"Retained disjoint query controlled Scene fixture passed; whole-capture replay not executed\n";
        return 0;
    }
    if(retained_pooled_shared_scene_fixture) {
        if(!retained_shared_queries_requested||!config.reuse_retained_opaque_resources||!queued_retained_draw_exports||
           retained_shared_query_prewarm_limit!=1||retained_catalog_prewarm_limit!=6||retained_shared_scene_fixture||retained_pool_fixture||
           retained_partial_fixture||retained_catalog_partial_fixture||retained_queued_draw_fixture||!retained_catalog_state_fixture.empty()||
           json_adapter||validate_only||wrap_frame_queries||through!=UINT32_MAX)
            throw std::runtime_error("Combined pool/shared Scene fixture requires complete independent Native startup and separate proof controls");
        std::vector<validation::RetainedOpaqueCatalogFixtureSource> sources;
        for(const auto& item:retained_catalog_prepared)sources.push_back({item.pair,item.vertex->spirv,item.pixel->spirv});
        validation::RetainedPooledSharedDepthSceneFixture(config,operations,sources,readonly_vertex->spirv,readonly_pixel->spirv);
        return 0; // Dedicated controlled fixture; whole-capture replay is not executed.
    }
    if(retained_shared_scene_fixture) {
        if(!retained_shared_queries_requested||retained_shared_query_prewarm_limit!=1||retained_catalog_prewarm_limit!=6||
           retained_partial_fixture||retained_catalog_partial_fixture||retained_queued_draw_fixture||!retained_catalog_state_fixture.empty())
            throw std::runtime_error("Shared Scene fixture requires full natural V2 startup and separate proof controls");
        std::vector<validation::RetainedOpaqueCatalogFixtureSource> sources;
        for(const auto& item:retained_catalog_prepared)sources.push_back({item.pair,item.vertex->spirv,item.pixel->spirv});
        validation::RetainedSharedDepthSceneFixture(config,operations,sources,readonly_vertex->spirv,readonly_pixel->spirv);
        // This is a separate controlled owned-Scene proof. The unchanged
        // whole-capture replay, natural selection and172image/2query parity
        // must be executed separately without this fixture flag.
        std::ofstream completed(output/"retained-shared-depth-scene-fixture-complete.json");
        completed<<Json({{"fixture_complete",true},{"whole_capture_replay_executed",false},
            {"scope","Controlled owned-Scene paired fixtures only; natural full-capture parity and performance remain separate required validation."}}).dump(2)<<'\n';
        if(!completed)throw std::runtime_error("Cannot retain shared owned-Scene fixture completion marker");
        return 0;
    }
    const bool fp24_requested=config.retained_fp24_depth_writer;
    if(fp24_scene_fixture) {
        if(json_adapter||wrap_frame_queries||validate_only||through!=UINT32_MAX||fp24_prewarm_limit!=4||
           retained_partial_fixture||retained_catalog_partial_fixture||retained_queued_draw_fixture||retained_pool_fixture||
           retained_shared_scene_fixture||retained_pooled_shared_scene_fixture||!retained_catalog_state_fixture.empty())
            throw std::runtime_error("FP24 owned Scene fixture requires independent complete typed startup controls");
        const std::array<std::span<const uint32_t>,2> vertices{fp24_vertices[0]->spirv,fp24_vertices[1]->spirv};
        validation::RetainedFp24DepthSceneFixture(config,operations,vertices,fp24_builtin->spirv);
        return 0; // Explicit controlled donor fixture; whole capture is a separate oracle.
    }
    if(early_catalog_scene_fixture||early_material_scene_fixture) {
        std::vector<validation::EarlyDepthCatalogSceneInput> inputs;
        for(const auto& recipe:early_prepared)inputs.push_back({recipe,early_vertex->spirv,early_pixel->spirv,early_program});
        for(const auto& item:early_catalog_prepared)inputs.push_back({item.recipe,item.vertex->spirv,item.pixel->spirv,item.program});
        if(early_material_scene_fixture)validation::RunNativeEarlyDepthMaterialRetentionSceneFixtures(config,operations,inputs);
        else validation::RunNativeEarlyDepthCatalogSceneFixtures(config,operations,inputs);
        std::cout<<"Early catalogue controlled Scene fixture passed; whole-capture replay not executed\n";
        return 0;
    }
    std::optional<validation::OverflowPipelinePrewarmFixture> overflow_fixture;
    if(overflow_pipeline_fixture)overflow_fixture.emplace(frames,overflow_pipeline_recipes,config.compiled);
    if(retained_phase_validation_fixtures||retained_phase_queued_export_fixture||retained_phase_marker_fixture||retained_phase_pass_failure_fixture) {
        if(retained_phase_prepared.size()!=kRetainedOpaquePhaseMaxPipelines)
            throw std::runtime_error("Opaque phase validation fixtures require all exact observed startup recipes");
        std::vector<validation::RetainedOpaquePhaseOwnerFixtureSource> sources;
        sources.reserve(retained_phase_prepared.size());
        for(const auto& item:retained_phase_prepared)sources.push_back({item.recipe,item.vertex->spirv,item.pixel->spirv});
        if(retained_phase_pass_failure_fixture) {
            validation::RetainedOpaquePhasePassFailureFixture(config,operations,sources,full_edram_fixture);
            throw std::runtime_error("Phase follower fixture returned without its expected fatal Scene destructor");
        }
        if(retained_phase_marker_fixture) {
            validation::RetainedOpaquePhaseMarkerFixture(config,operations,sources,full_edram_fixture);
            std::cout<<"Opaque phase marker fixture passed; whole-capture replay not executed\n";
            return 0;
        }
        if(retained_phase_queued_export_fixture) {
            validation::RetainedOpaquePhaseQueuedExportFixture(config,operations,sources,full_edram_fixture);
            std::cout<<"Opaque phase queued-export fixture passed; whole-capture replay not executed\n";
            return 0;
        }
        validation::RetainedOpaquePhaseOwnerFixture(config,operations,sources,full_edram_fixture);
        validation::RetainedOpaquePhasePrefixFixture(config,operations,sources,full_edram_fixture);
        std::ofstream file(output/"retained-opaque-phase-validation-complete.json");
        file<<Json({{"passed",true},{"owner_fixture",true},{"prefix_fixture",true},
            {"whole_capture_replay_executed",false},{"performance_claim",false}}).dump(2)<<'\n';
        if(!file)throw std::runtime_error("Cannot retain opaque phase validation completion marker");
        return 0; // Dedicated controlled Scenes; full parity is a separate prerequisite.
    }
    SceneExecutor replay(std::move(config));
    if(stencil_material_requested&&!stencil_material_native) {
        const NativeFsiPipelineRecipe recipe{std::string(kStencilMaterialVertex),std::string(kStencilMaterialPixel),
            {3,2,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
        replay.PrewarmFsiPipeline(recipe,stencil_material_vertex->spirv,stencil_material_pixel->spirv);
        // Repeated startup recipes must deduplicate to one exact variant PSO.
        replay.PrewarmFsiPipeline(recipe,stencil_material_vertex->spirv,stencil_material_pixel->spirv);
    }
    if(fp24_requested) {
        // Every exact ordinary source/raster oracle is resident FIRST.
        for(size_t i=0;i<fp24_vertices.size();++i)for(uint32_t front:{1u,2u})
            replay.PrewarmFsiPipeline({std::string(kNativeRetainedFp24DepthSources[i].source),std::nullopt,{3,front,1,4,0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1},fp24_vertices[i]->spirv,fp24_builtin->spirv);
        uint32_t attempted=0;
        for(size_t i=0;i<fp24_vertices.size();++i)for(uint32_t front:{1u,2u}) {
            if(attempted++>=fp24_prewarm_limit)continue;
            if(!replay.PrewarmRetainedFp24DepthPipeline({std::string(kNativeRetainedFp24DepthSources[i].source),std::nullopt,{3,front,1,4,0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1},fp24_vertices[i]->spirv,fp24_builtin->spirv))
                throw std::runtime_error("Retained FP24 actual optional startup admission refused");
        }
    }
    if(retained_shared_queries_requested) {
        if(readonly_prepared.empty())throw std::runtime_error("Retained shared queries lack the exact observed generic recipe");
        for(const auto& recipe:readonly_prepared)replay.PrewarmFsiPipeline(recipe,readonly_vertex->spirv,readonly_pixel->spirv);
    }
    if(retained_requested) {
        for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
            const NativeFsiPipelineRecipe recipe{std::string(kRetainedOpaqueVertex),std::string(kRetainedOpaquePixel),
                {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            replay.PrewarmFsiPipeline(recipe,retained_vertex->spirv,retained_pixel->spirv);
            if(!replay.PrewarmRetainedOpaquePipeline(recipe,retained_vertex->spirv,retained_pixel->spirv))
                throw std::runtime_error("Actual retained writer startup pipeline refused");
        }
    }
    if(retained_catalog_requested) {
        // Every ordinary oracle is prepared before any optional admission.
        for(const auto& item:retained_catalog_prepared)
            for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
                const NativeFsiPipelineRecipe recipe{std::string(item.pair.vertex),std::string(item.pair.pixel),
                    {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                    int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
                replay.PrewarmFsiPipeline(recipe,item.vertex->spirv,item.pixel->spirv);
            }
        uint32_t attempted=0;
        for(const auto& item:retained_catalog_prepared)
            for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
                if(attempted>=retained_catalog_prewarm_limit)break;
                const NativeFsiPipelineRecipe recipe{std::string(item.pair.vertex),std::string(item.pair.pixel),
                    {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                    int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
                ++attempted;
                if(!replay.PrewarmRetainedOpaquePipeline(recipe,item.vertex->spirv,item.pixel->spirv))
                    throw std::runtime_error("Retained catalog actual optional startup admission refused");
            }
    }
    if(retained_shared_queries_requested&&retained_shared_query_prewarm_limit) {
        if(!replay.PrewarmRetainedSharedDepthQueryPipeline(readonly_prepared.front(),readonly_vertex->spirv,readonly_pixel->spirv))
            throw std::runtime_error("Retained shared-depth startup resident query pipeline refused");
    }
    if(opaque_requested) {
        for(const auto& raster:{std::array<uint32_t,6>{3,1,1,4,0,0},std::array<uint32_t,6>{3,2,1,4,0,0}}) {
            NativeFsiPipelineRecipe recipe{std::string(kOpaqueColorOverwriteVertex),std::string(kOpaqueColorOverwritePixel),
                raster,int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            replay.PrewarmFsiPipeline(recipe,opaque_vertex->spirv,opaque_pixel->spirv);
        }
        for(const auto& raster:{std::array<uint32_t,6>{3,1,1,4,0,0},std::array<uint32_t,6>{3,2,1,4,0,0}}) {
            NativeFsiPipelineRecipe recipe{std::string(kOpaqueColorOverwriteVertex),std::string(kOpaqueColorOverwritePixel),
                raster,int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            if(!replay.PrewarmOpaqueColorOverwritePipeline(recipe,opaque_vertex->spirv))
                throw std::runtime_error("Opaque replay startup pipeline preparation refused");
        }
    }
    if(forward_requested) {
        for(const auto& raster:{std::array<uint32_t,6>{3,1,1,4,0,0},std::array<uint32_t,6>{3,2,1,4,0,0}}) {
            NativeFsiPipelineRecipe recipe{std::string(kForwardControlFlowVertex),std::string(kForwardControlFlowPixel),
                raster,int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            replay.PrewarmFsiPipeline(recipe,forward_vertex->spirv,forward_pixel->spirv);
        }
        for(const auto& raster:{std::array<uint32_t,6>{3,1,1,4,0,0},std::array<uint32_t,6>{3,2,1,4,0,0}}) {
            NativeFsiPipelineRecipe recipe{std::string(kForwardControlFlowVertex),std::string(kForwardControlFlowPixel),
                raster,int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            if(!replay.PrewarmForwardControlFlowPipeline(recipe,forward_vertex->spirv))
                throw std::runtime_error("Forward-CFG replay startup pipeline preparation refused");
        }
    }
    if(forward_vertex_requested) {
        // Original PS oracle bytes are used unchanged by both paths. Generic
        // pipelines are installed first; candidate VS PSOs follow before Run.
        for(const auto& item:forward_vertex_prepared)replay.PrewarmFsiPipeline(item.recipe,pinned_vertex->spirv,item.pixel);
        for(const auto& item:forward_vertex_prepared)if(!replay.PrewarmForwardVertexPipeline(item.recipe,pinned_vertex->spirv))
            throw std::runtime_error("Forward-VS replay startup pipeline preparation refused");
    }
    if(catalog_requested) {
        for(const auto& item:catalog_prepared)replay.PrewarmFsiPipeline(item.recipe,item.vertex,item.pixel);
        for(const auto& item:catalog_prepared)if(!replay.PrewarmForwardPixelCatalogPipeline(item.recipe,item.vertex))
            throw std::runtime_error("Catalog replay startup pipeline preparation refused");
    }
    if(readonly_requested) {
        for(const auto& recipe:readonly_prepared)replay.PrewarmFsiPipeline(recipe,readonly_vertex->spirv,readonly_pixel->spirv);
        for(const auto& recipe:readonly_prepared)if(!replay.PrewarmReadonlyDepthQueryPipeline(recipe,readonly_vertex->spirv,readonly_pixel->spirv))
            throw std::runtime_error("Read-only query replay startup resident pipeline refused");
    }
    if(config.native_early_depth) {
        for(const auto& recipe:early_prepared)replay.PrewarmFsiPipeline(recipe,early_vertex->spirv,early_pixel->spirv);
        size_t admitted=0;
        for(const auto& recipe:early_prepared) {
            if(admitted>=early_prewarm_limit)break;
            if(replay.PrewarmNativeEarlyDepthPipeline(recipe,early_vertex->spirv,early_pixel->spirv,early_program))++admitted;
        }
        if(early_prewarm_limit&&!admitted)throw std::runtime_error("Native early-depth startup admitted no candidate pipeline");
    }
    if(config.native_early_depth_catalog) {
        for(const auto& item:early_catalog_prepared)replay.PrewarmFsiPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
        size_t admitted=0;
        for(const auto& item:early_catalog_prepared) {
            if(admitted>=early_catalog_prewarm_limit)break;
            if(replay.PrewarmNativeEarlyDepthCatalogPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv,item.program))++admitted;
        }
        if(early_catalog_prewarm_limit&&!admitted)throw std::runtime_error("Early-depth catalogue admitted no candidate pipeline");
    }
    if(d32_requested) {
        for(const auto& recipe:readonly_prepared)replay.PrewarmFsiPipeline(recipe,readonly_vertex->spirv,readonly_pixel->spirv);
        for(const auto& recipe:readonly_prepared)if(!replay.PrewarmNativeReadonlyDepthPipeline(recipe,readonly_vertex->spirv,readonly_pixel->spirv))
            throw std::runtime_error("Native readonly D32 startup resident pipeline refused");
    }
    if(fsi_state_requested) {
        // Every generic oracle is resident first. Variant admission respects the
        // same hard budgets and may refuse cold extra states without eviction.
        for(const auto& item:fsi_state_prepared)replay.PrewarmFsiPipeline(item.recipe.generic,item.vertex,item.pixel);
        if(prewarm_phase_originals) {
            // Explicit fixture-only preparation, identical in OFF and ON.
            // Runtime already prepares observed originals; this closes the
            // replay's smaller sidecar subset without enabling a legacy mode.
            const auto owners=[&] {
                const auto& report=replay.report();const auto& p=report.at("pipeline_cache");const auto& s=report.at("shader_cache");
                return Json{{"pipelines",p.at("resident")},{"pipeline_creations",p.at("creations")},{"pipeline_misses",p.at("misses")},
                    {"pipeline_evictions",p.at("evictions")},{"pipeline_drains",p.at("pressure_drains")},
                    {"modules",s.at("resident")},{"module_creations",s.at("creations")},{"module_misses",s.at("misses")},
                    {"module_evictions",s.at("evictions")},{"module_drains",s.at("pressure_drains")},{"module_bytes",s.at("spirv_bytes")}};
            };
            const auto before=owners();
            for(const auto& item:retained_phase_prepared)replay.PrewarmFsiPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
            const auto after=owners();bool preserved=!replay.mutation_serial()&&!replay.HasPendingGpuWork();
            for(const char* key:{"pipeline_evictions","pipeline_drains","module_evictions","module_drains"})
                preserved=preserved&&before.at(key)==after.at(key);
            // Re-probe all material originals and every new exact fixture
            // recipe. No miss, creation, drain or owner count may change.
            for(const auto& item:fsi_state_prepared)replay.PrewarmFsiPipeline(item.recipe.generic,item.vertex,item.pixel);
            for(const auto& item:retained_phase_prepared)replay.PrewarmFsiPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
            const auto probed=owners();preserved=preserved&&after==probed;
            Json proof{{"fixture_only",true},{"recipes",retained_phase_prepared.size()},{"before",before},{"after",after},
                {"after_probes",probed},{"prior_owners_retained",preserved},{"variant_preparation_here",false}};
            std::ofstream file(output/"opaque-phase-originals-prewarm.json");file<<proof.dump(2)<<'\n';
            if(!file||!preserved)throw std::runtime_error("Captured opaque phase originals lost prior owners or created guest work");
        }
        std::vector<FsiStatePrepared> admitted;
        if(fsi_material_composition_requested) {
            const auto owners=[&] {
                const auto& report=replay.report();const auto& pipelines=report.at("pipeline_cache");const auto& shaders=report.at("shader_cache");
                return Json{{"pipelines",pipelines.at("resident")},{"pipeline_creations",pipelines.at("creations")},
                    {"pipeline_misses",pipelines.at("misses")},{"pipeline_evictions",pipelines.at("evictions")},
                    {"pipeline_drains",pipelines.at("pressure_drains")},{"modules",shaders.at("resident")},
                    {"module_creations",shaders.at("creations")},{"module_misses",shaders.at("misses")},
                    {"module_evictions",shaders.at("evictions")},{"module_drains",shaders.at("pressure_drains")},
                    {"module_bytes",shaders.at("spirv_bytes")}};
            };
            const auto before=owners();std::vector<size_t> accepted;
            for(size_t i=0;i<fsi_state_prepared.size()&&accepted.size()<fsi_state_prewarm_limit;++i) {
                const auto& item=fsi_state_prepared[i];
                if(replay.PrewarmFsiStatePipeline(item.recipe,item.vertex))accepted.push_back(i);
            }
            const auto after=owners();
            bool retained=true;
            for(const char* key:{"pipeline_evictions","pipeline_drains","module_evictions","module_drains"})
                retained=retained&&before.at(key)==after.at(key);
            for(const char* key:{"pipelines","pipeline_creations","pipeline_misses","modules","module_creations","module_misses","module_bytes"})
                retained=retained&&before.at(key).get<uint64_t>()<=after.at(key).get<uint64_t>();
            // Probe every original generic key, not only aggregate occupancy.
            // Cache hit counters may change; misses/creations/owners may not.
            for(const auto& item:fsi_state_prepared)replay.PrewarmFsiPipeline(item.recipe.generic,item.vertex,item.pixel);
            if(config.native_early_depth)for(const auto& recipe:early_prepared)
                replay.PrewarmFsiPipeline(recipe,early_vertex->spirv,early_pixel->spirv);
            if(config.native_early_depth_catalog)for(const auto& item:early_catalog_prepared)
                replay.PrewarmFsiPipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
            if(d32_requested)for(const auto& recipe:readonly_prepared)
                replay.PrewarmFsiPipeline(recipe,readonly_vertex->spirv,readonly_pixel->spirv);
            const auto probed=owners();retained=retained&&probed==after&&!replay.mutation_serial()&&!replay.HasPendingGpuWork();
            if(!fsi_state_prewarm_limit)retained=retained&&before==after&&accepted.empty();
            Json proof{{"policy",std::string(kFsiStateMaterialCompositionPolicy)},{"prewarm_limit",fsi_state_prewarm_limit},
                {"source_prepared_tuples",fsi_state_prepared.size()},{"optional_admitted",accepted.size()},
                {"before_optional",before},{"after_optional",after},{"after_original_key_probes",probed},
                {"original_owners_retained",retained},{"physical_budget_fixture",false},
                {"scope","Normal-budget startup occupancy plus re-probe of every generic material/early/D32 key; unchanged eviction/drain/miss/creation counters after probes. Not a lowered physical-budget test."}};
            std::ofstream file(output/"fsi-state-prewarm-ownership.json");file<<proof.dump(2)<<'\n';
            if(!file||!retained)throw std::runtime_error("Optional material prewarm lost an original cache owner or created guest work");
            for(size_t i:accepted)admitted.push_back(std::move(fsi_state_prepared[i]));
        } else {
            for(auto& item:fsi_state_prepared)if(replay.PrewarmFsiStatePipeline(item.recipe,item.vertex))admitted.push_back(std::move(item));
        }
        fsi_state_prepared=std::move(admitted);
        if(fsi_state_prepared.empty()&&(!fsi_material_composition_requested||fsi_state_prewarm_limit))
            throw std::runtime_error("FSI replay admitted no exact specialized tuple");
    }
    if(overflow_fixture)overflow_fixture->Prepare(replay);
    if(retained_phase_requested) {
        const auto owners=[&] {
            const auto& report=replay.report();const auto& pipelines=report.at("pipeline_cache");const auto& shaders=report.at("shader_cache");
            return Json{{"pipeline_evictions",pipelines.at("evictions")},{"pipeline_drains",pipelines.at("pressure_drains")},
                {"module_evictions",shaders.at("evictions")},{"module_drains",shaders.at("pressure_drains")}};
        };
        const auto before=owners();uint64_t admitted=0,refused=0;
        // All existing generic, early-depth, D32 and FSI material admission has
        // finished. A missing ordinary owner is a refusal, not a late prewarm.
        for(const auto& item:retained_phase_prepared) {
            if(!NativeRetainedOpaquePhaseRecipe(item.recipe.vertex,*item.recipe.pixel,item.recipe.raster))continue;
            const bool ready=replay.PrewarmRetainedOpaquePhasePipeline(item.recipe,item.vertex->spirv,item.pixel->spirv);
            ready?++admitted:++refused;
        }
        const auto after=owners();
        const bool preserved=before==after&&!replay.mutation_serial()&&!replay.HasPendingGpuWork();
        retained_phase_startup["optional_admitted"]=admitted;retained_phase_startup["optional_refused"]=refused;
        retained_phase_startup["before_optional"]=before;retained_phase_startup["after_optional"]=after;
        retained_phase_startup["no_eviction_or_guest_work"]=preserved;
        std::ofstream file(output/"retained-opaque-phase-prewarm.json");file<<retained_phase_startup.dump(2)<<'\n';
        if(!file||!preserved)throw std::runtime_error("Retained opaque phase startup evicted prior owners or created guest work");
    }
    // A full FINAL read is independent of how the initial memory is supplied.
    // SceneRun imports authenticated captured history; do not prepend a redundant
    // zero seed and call that imported history synthetic.
    if(stencil_material_native) {
        const NativeFsiPipelineRecipe recipe{std::string(kStencilMaterialVertex),std::string(kStencilMaterialPixel),
            {3,2,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
        if(!replay.PrewarmStencilMaterialPipeline(recipe,stencil_material_vertex->spirv,stencil_material_pixel->spirv)||
           !replay.PrewarmStencilMaterialPipeline(recipe,stencil_material_vertex->spirv,stencil_material_pixel->spirv))
            throw std::runtime_error("Native stencil material prewarm refused");
    }
    Json initial_edram_history;
    const auto history_path=fs::path(argv[2])/"gpu-history.json";
    bool captured_edram_history=false;
    if(full_edram_fixture&&fs::exists(history_path)) {
        const auto metadata_bytes=Read(history_path,16u*1024*1024);
        const auto history=Json::parse(metadata_bytes);
        captured_edram_history=!history.at("edram").is_null();
        if(captured_edram_history)initial_edram_history={{"kind","reference_gpu_frame_boundary"},
            {"metadata_path",history_path.string()},{"metadata_sha256",ContentHash(metadata_bytes)},
            {"edram",history.at("edram")},{"trace_sha256",history.at("trace_sha256")}};
    }
    if(full_edram_fixture&&!captured_edram_history) {
        replay.InitializeEdram(std::vector<uint8_t>(10u*1024*1024,0));
        initial_edram_history={{"kind","explicit_zero_seed_without_captured_edram"},{"initial_byte",0}};
    }
    replay.RunLiveCapture(operations,through,json_adapter,wrap_frame_queries);
    if(full_edram_fixture) {
        const auto bytes=replay.ReadFullEdram();
        if(bytes.size()!=10u*1024*1024)throw std::runtime_error("Full EDRAM fixture extent changed");
        std::ofstream binary(output/"final-edram.bin",std::ios::binary);
        binary.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        if(!binary)throw std::runtime_error("Cannot retain full EDRAM fixture bytes");
        std::ofstream metadata(output/"edram-parity-fixture.json");
        metadata<<Json({{"full_final_edram_read",true},{"synthetic_initial_history",!captured_edram_history},
            {"initial_history",initial_edram_history},{"bytes",bytes.size()},{"sha256",ContentHash(bytes)},
            {"scope","Full final10MiB equivalence oracle; initial EDRAM comes from authenticated capture history when supplied, otherwise the explicit zero seed. No uncaptured zero-history claim."}}).dump(2)<<'\n';
        if(!metadata)throw std::runtime_error("Cannot retain full EDRAM fixture identity");
    }
    if(overflow_fixture) {
        const auto proof=overflow_fixture->Finish(replay,full_edram_fixture,wrap_frame_queries);
        std::ofstream file(output/"overflow-pipeline-prewarm-fixture.json");file<<proof.dump(2)<<'\n';
        if(!file)throw std::runtime_error("Cannot retain overflow PSO lifetime/reuse coverage");
    }
    if(pinned_null_constants_requested) {
        replay.FlushGpuWork();
        const auto report=replay.report();
        const auto& stats=report.at("pinned_null_constant_uploads");
        const auto draws=stats.at("retired_draws").get<uint64_t>();
        const auto selected=stats.at("selected").get<uint64_t>();
        const bool valid=stats.at("enabled")==true&&selected>128&&draws>selected&&
            stats.at("candidates").get<uint64_t>()==selected+stats.at("refusals").get<uint64_t>()&&
            stats.at("float_baseline_bytes").get<uint64_t>()==draws*8192&&
            stats.at("float_written_bytes").get<uint64_t>()+stats.at("float_omitted_bytes").get<uint64_t>()==draws*8192&&
            stats.at("float_omitted_bytes").get<uint64_t>()==selected*8016&&
            stats.at("shared_written_bytes").get<uint64_t>()==draws*2624&&
            stats.at("allocation_payload_bytes")==10816&&stats.at("full_range_flushes")==draws&&
            report.at("gpu_work_queue").at("draw_lists")==draws;
        std::ofstream file(output/"pinned-null-constant-upload-coverage.json");
        file<<Json({{"coverage_passed",valid},{"counters",stats},
            {"scope","Retired canonical null draws and ordinary full-upload draws; unchanged allocation/shared bytes/full flush. Image/query/full EDRAM parity requires paired capture comparison."}}).dump(2)<<'\n';
        if(!file||!valid)throw std::runtime_error("Pinned null constant actual coverage missing or inconsistent");
    }
    if(constant_delta_requested) {
        replay.FlushCommittedState();
        const auto& stats=replay.report().at("constant_delta_uploads");
        const auto draws=stats.at("submitted_draws").get<uint64_t>();
        const auto baseline=stats.at("submitted_float_baseline_bytes").get<uint64_t>();
        const auto written=stats.at("submitted_float_written_bytes").get<uint64_t>();
        const auto skipped=stats.at("submitted_float_unchanged_blocks").get<uint64_t>();
        const bool valid=stats.at("enabled")==true&&draws>128&&written>0&&skipped>0&&
            baseline==draws*kFloatConstantBytes&&written+skipped*64==baseline&&written<baseline&&
            stats.at("submitted_shared_written_bytes").get<uint64_t>()>0&&
            stats.at("submitted_full_range_flushes").get<uint64_t>()==draws&&
            stats.at("metadata_bytes").get<uint64_t>()<=stats.at("maximum_metadata_bytes").get<uint64_t>();
        std::ofstream file(output/"constant-delta-upload-coverage.json");
        file<<Json({{"coverage_passed",valid},{"counters",stats},
            {"scope","Actual retired-slot constant writes with all input bits compared, complete shared ABI/full mapped-range flush; image/query/full EDRAM equivalence is a separate paired requirement."}}).dump(2)<<'\n';
        if(!file||!valid)throw std::runtime_error("Constant delta actual submitted coverage is missing or inconsistent");
    }
    if(fp24_requested) {
        replay.FlushCommittedState();const auto& stats=replay.report().at("retained_fp24_depth_writer");
        bool sources=true;uint64_t selected_sum=0;
        for(size_t i=0;i<stats.at("sources").size();++i) {
            const uint64_t count=stats.at("sources").at(i).at("selected").get<uint64_t>();selected_sum+=count;
            sources=sources&&(fp24_prewarm_limit==0?count==0:fp24_prewarm_limit==2?(i==0?count>0:count==0):count>0);
        }
        const bool mechanism=fp24_prewarm_limit?stats.value("selected",uint64_t(0))>0&&stats.value("reuses",uint64_t(0))>0&&
            stats.value("maximum_run",uint64_t(0))>1&&stats.value("restores",uint64_t(0))>0:
            stats.value("selected",uint64_t(1))==0&&stats.value("restores",uint64_t(1))==0;
        const bool valid=sources&&stats.at("sources").size()==2&&stats.value("prepared",uint64_t(99))==fp24_prewarm_limit&&mechanism&&
            stats.value("candidates",uint64_t(0))>0&&selected_sum==stats.value("selected",uint64_t(-1))&&
            stats.at("restores")==stats.at("materializations")&&stats.at("conversion_restores")==stats.at("restores")&&stats.at("conversion_exports")==stats.at("materializations")&&
            (!wrap_frame_queries||stats.at("selected")==stats.at("queried_selected"))&&
            stats.value("source_refusals",uint64_t(1))==0&&stats.value("resource_refusals",uint64_t(1))==0&&stats.value("prewarm_refusals",uint64_t(1))==0&&
            (fp24_prewarm_limit==4?stats.value("residency_refusals",uint64_t(1))==0:stats.value("residency_refusals",uint64_t(0))>0)&&
            stats.value("module_bytes",uint64_t(-1))<=65536&&stats.value("modules_created",uint64_t(99))==(fp24_prewarm_limit?1u:0u)&&
            stats.at("peak_retained_image_and_scratch_bytes").get<uint64_t>()<=stats.at("maximum_retained_image_and_scratch_bytes").get<uint64_t>();
        std::ofstream file(output/"retained-fp24-depth-coverage.json");file<<Json({{"coverage_passed",valid},{"prewarm_limit",fp24_prewarm_limit},
            {"frame_query_wrapping",wrap_frame_queries},{"full_edram_fixture",full_edram_fixture},{"counters",stats},
            {"scope","Actual two-source CPU-owned nullPS FP24 depth-writer selection and exact depth authority; paired whole images/query identities/full10MiB, controlled state/source-switch fixtures and owned shutdown remain separate required oracles."}}).dump(2)<<'\n';
        if(!file||!valid)throw std::runtime_error("Retained FP24 actual selection, residency or authority coverage failed");
    }
    if(retained_requested) {
        replay.FlushCommittedState(); // This public boundary must restore canonical packed memory.
        const auto& stats=replay.report().at("retained_opaque_writer");
        const bool positive=stats.value("prepared",uint64_t(0))==2&&stats.value("selected",uint64_t(0))>0&&
            stats.value("maximum_run",uint64_t(0))>1&&stats.value("reuses",uint64_t(0))>0&&
            stats.value("restores",uint64_t(0))>0&&stats.value("materializations",uint64_t(0))==stats.value("restores",uint64_t(0))&&
            (!wrap_frame_queries||stats.at("queried_selected")==stats.at("selected"))&&
            stats.value("source_refusals",uint64_t(1))==0&&stats.value("resource_refusals",uint64_t(1))==0&&
            stats.value("residency_refusals",uint64_t(1))==0&&stats.value("prewarm_refusals",uint64_t(1))==0&&
            stats.value("peak_retained_image_bytes",uint64_t(0))<=stats.value("maximum_retained_image_bytes",uint64_t(0));
        Json coverage{{"requested",true},{"positive_actual_retained_writer",positive},{"frame_query_wrapping",wrap_frame_queries},
            {"full_edram_fixture",full_edram_fixture},{"counters",stats},
            {"scope","Actual retained pinned material selection/reuse/materialization; original VS/PS supplied bytes checked before startup; paired image/full 10MiB/real-query equivalence and clean owned shutdown remain separate required oracles"}};
        std::ofstream file(output/"retained-opaque-writer-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!positive)throw std::runtime_error("Retained opaque actual run/reuse/commit coverage is missing or refused");
    }
    if(retained_catalog_requested) {
        replay.FlushCommittedState();const auto& stats=replay.report().at("retained_opaque_catalog");
        const bool unsupported_state=retained_state_fixture&&retained_state_fixture->expects_state_refusal;
        bool source_coverage=true;uint64_t sources_positive=0,source_candidates=0,source_selected=0,source_queried=0;
        for(const auto& source:stats.at("programs")) {
            const auto selected=source.value("selected",uint64_t(0));sources_positive+=selected>0;
            if(retained_catalog_prewarm_limit==2&&
               (source.at("pixel")==kRetainedOpaqueCatalogPairs[0].pixel?selected==0:selected!=0))
                source_coverage=false;
            source_candidates+=source.value("candidates",uint64_t(0));source_selected+=selected;
            source_queried+=source.value("queried_selected",uint64_t(0));
            source_coverage=source_coverage&&(unsupported_state?
                (source.value("candidates",uint64_t(1))==0&&selected==0):
                (source.value("candidates",uint64_t(0))>0&&
                 (retained_catalog_prewarm_limit<kRetainedOpaqueCatalogMaxPipelines||selected>0)))&&
                (!wrap_frame_queries||source.at("selected")==source.at("queried_selected"));
        }
        const bool all_prepared=retained_catalog_prewarm_limit==kRetainedOpaqueCatalogMaxPipelines;
        const bool mechanism=unsupported_state?
            stats.value("candidates",uint64_t(1))==0&&stats.value("selected",uint64_t(1))==0&&
                stats.value("restores",uint64_t(1))==0&&stats.value("materializations",uint64_t(1))==0:
            all_prepared?
            stats.value("selected",uint64_t(0))>0&&stats.value("reuses",uint64_t(0))>0&&
                stats.value("maximum_run",uint64_t(0))>1&&stats.value("restores",uint64_t(0))>0&&
                (retained_catalog_state_fixture!="source-switch"||stats.value("shader_switches",uint64_t(0))>=2)&&
                stats.value("residency_refusals",uint64_t(1))==0:
            stats.value("residency_refusals",uint64_t(0))>0&&
                (retained_catalog_prewarm_limit==0?stats.value("selected",uint64_t(1))==0:
                 stats.value("selected",uint64_t(0))>0&&stats.value("reuses",uint64_t(0))>0&&
                 stats.value("restores",uint64_t(0))>0&&sources_positive==1);
        const bool positive=stats.at("programs").size()==kRetainedOpaqueCatalogMaxPrograms&&source_coverage&&mechanism&&stats.value("prepared",uint64_t(99))==retained_catalog_prewarm_limit&&
            stats.value("prepared",uint64_t(99))<=kRetainedOpaqueCatalogMaxPipelines&&stats.value("modules_created",uint64_t(99))<=kRetainedOpaqueCatalogMaxPrograms&&
            stats.value("module_bytes",uint64_t(-1))<=kRetainedOpaqueCatalogMaxModuleBytes&&
            stats.value("materializations",uint64_t(0))==stats.value("restores",uint64_t(0))&&
            source_candidates==stats.value("candidates",uint64_t(-1))&&source_selected==stats.value("selected",uint64_t(-1))&&source_queried==stats.value("queried_selected",uint64_t(-1))&&
            stats.value("source_refusals",uint64_t(1))==0&&stats.value("resource_refusals",uint64_t(1))==0&&stats.value("prewarm_refusals",uint64_t(1))==0&&
            stats.value("peak_retained_image_bytes",uint64_t(0))<=stats.value("maximum_retained_image_bytes",uint64_t(0));
        Json coverage{{"requested",true},{"coverage_passed",positive},
            {"positive_actual_retained_catalog",positive&&stats.value("selected",uint64_t(0))>0},{"frame_query_wrapping",wrap_frame_queries},
            {"prewarm_limit",retained_catalog_prewarm_limit},{"sources_positive",sources_positive},{"counters",stats},{"full_edram_fixture",full_edram_fixture},
            {"synthetic_state_fixture",bool(retained_state_fixture)},{"expected_state_refusal",unsupported_state},
            {"natural_shader_switches_observed",retained_catalog_state_fixture.empty()?Json(stats.value("shader_switches",uint64_t(0))):Json(nullptr)},
            {"separate_source_switch_proof_required",!(positive&&retained_catalog_state_fixture=="source-switch"&&all_prepared&&stats.value("shader_switches",uint64_t(0))>=2)},
            {"positive_source_switch_fixture",positive&&retained_catalog_state_fixture=="source-switch"&&all_prepared&&stats.value("shader_switches",uint64_t(0))>=2},
            {"scope","Exact three-source catalogue: natural all-source selection may have zero switches; a separate controlled all-prepared triplet must prove positive retained source-switch continuity. Explicit missing-resident and unsupported-state cases must stay ordinary Native. Paired full10MiB/images/real queries and clean shutdown are separate required oracles."}};
        std::ofstream file(output/"retained-opaque-catalog-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!positive)throw std::runtime_error("Retained catalog actual source coverage, residency, or conservation failed");
    }
    if(retained_shared_queries_requested) {
        replay.FlushCommittedState();const auto& stats=replay.report().at("retained_shared_depth_queries");
        const bool selected=stats.value("selected",uint64_t(0))>0;
        const bool mechanism=retained_shared_query_prewarm_limit?
            selected&&stats.at("selected")==stats.at("queried_selected")&&stats.at("selected")==stats.at("promotions")&&
                stats.value("residency_refusals",uint64_t(1))==0&&stats.value("framebuffer_refusals",uint64_t(1))==0:
            !selected&&stats.value("residency_refusals",uint64_t(0))>0&&stats.value("promotions",uint64_t(1))==0;
        const bool valid=stats.value("prepared",uint64_t(9))==retained_shared_query_prewarm_limit&&
            stats.value("module_bytes",uint64_t(-1))<=64u*1024&&stats.value("prewarm_refusals",uint64_t(1))==0&&
            stats.value("candidates",uint64_t(0))>0&&mechanism;
        Json coverage{{"coverage_passed",valid},{"positive_actual_shared_depth_queries",valid&&selected},
            {"prewarm_limit",retained_shared_query_prewarm_limit},{"frame_query_wrapping",wrap_frame_queries},{"counters",stats},
            {"scope","Only queried audited nullPS draws on a dirty exact retained V2 depth image. Color token and dirty authority remain unchanged. No extra restore/conversion occurs for selected queries. Exact query identities/full10MiB/images, empty-query all-work watermark and owned pressure/failure fixtures remain separate required oracles."}};
        std::ofstream file(output/"retained-shared-depth-query-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!valid)throw std::runtime_error("Retained shared-depth actual selection, promotion or missing-resident coverage failed");
    }
    if(queued_retained_draw_exports) {
        const auto& stats=replay.report().at(retained_phase_requested?"retained_opaque_phase":retained_catalog_requested?"retained_opaque_catalog":"retained_opaque_writer");
        const bool unsupported_state=retained_state_fixture&&retained_state_fixture->expects_state_refusal;
        const bool selected=stats.value("queued_draw_materializations",uint64_t(0))>0&&
            stats.value("queued_draw_pending",uint64_t(0))>0&&
            stats.value("queued_draw_materializations",uint64_t(-1))<=stats.value("materializations",uint64_t(0));
        const bool refused=stats.value("queued_draw_materializations",uint64_t(1))==0&&
            stats.value("queued_draw_pending",uint64_t(1))==0&&
            stats.value("selected",uint64_t(1))==0&&stats.value("restores",uint64_t(1))==0&&
            stats.value("materializations",uint64_t(1))==0;
        const bool positive=!json_adapter&&(unsupported_state?refused:selected);
        std::ofstream file(output/"retained-queued-draw-export-coverage.json");
        file<<Json({{"enabled",true},{"coverage_passed",positive},{"positive_actual_queued_exports",positive&&selected},
            {"expected_state_refusal",unsupported_state},{"counters",stats},
            {"scope","Actual accepted FIFO exports before typed draws. CPU/foreign/coherence/readback/control and destructor completion remain strong; full memory/images/real query parity are separate required oracles."}}).dump(2)<<'\n';
        if(!file||!positive)throw std::runtime_error("Queued retained typed draw export coverage missing");
    }
    if(state_source_observer){state_source_observer->StopAccepting();state_source_observer->Drain();}
    if(state_observer){state_observer->StopAccepting();state_observer->Drain();}
    if(fsi_state_requested&&fsi_material_composition_requested) {
        const auto& state=replay.report().at("fsi_state_specialization");
        const auto& policy=replay.report().at("fsi_state_material_composition");
        const uint64_t examined=policy.at("examined"),source_refusals=policy.at("source_refusals"),
            query_refusals=policy.at("query_refusals"),state_refusals=policy.at("state_refusals"),admitted=policy.at("admitted");
        const uint64_t candidates=state.at("candidates"),selected=state.at("selected");
        bool conserved=policy.at("identity")==std::string(kFsiStateMaterialCompositionPolicy)&&
            policy.at("inactive_early_catalog_enabled")==fsi_inactive_early_catalog_requested&&
            state.at("enabled")==true&&state.at("composition_enabled")==false&&
            state.at("identity")==std::string(kFsiStateSpecializationIdentity)&&
            state.at("variant_version")==FsiStateSpecializationCompilerVersion()&&
            examined==source_refusals+query_refusals+state_refusals+admitted&&admitted==candidates&&selected<=admitted&&
            state.at("queried_selected")==0&&state.at("resident_handoffs")==0&&state.at("coverage_pointer_hits")==0&&
            (fsi_state_prewarm_limit?state.at("prepared").get<uint64_t>()>0:state.at("prepared")==0)&&
            state.at("gameplay_compilations")==0&&state.at("gameplay_pipeline_creations")==0;
        uint64_t row_candidates=0,row_selected=0,visited_prepared=0,unvisited_prepared=0;
        uint64_t inactive_candidates=0,inactive_selected=0;
        bool fallback_positive=false,visited_coverage=true;
        for(const auto& row:state.at("coverage")) {
            const auto& vertex=row.at("vertex");const auto& pixel=row.at("pixel");
            const uint64_t rc=row.at("candidates"),rs=row.at("selected");
            const bool source_ok=vertex.is_string()&&pixel.is_string()&&
                FsiStateMaterialCompositionSourceAllowed(vertex.get_ref<const std::string&>(),pixel.get_ref<const std::string&>(),
                    fsi_inactive_early_catalog_requested,fsi_early_catalog_active);
            conserved=conserved&&source_ok&&rs<=rc&&row.at("queried_candidates")==0&&row.at("queried_selected")==0;
            if(vertex.is_string()&&pixel.is_string()&&FsiStateInactiveEarlyCatalogPair(
                    vertex.get_ref<const std::string&>(),pixel.get_ref<const std::string&>())) {
                inactive_candidates+=rc;inactive_selected+=rs;
            }
            row_candidates+=rc;row_selected+=rs;fallback_positive=fallback_positive||rc>rs;
            if(rs) {
                bool prepared_source=false;
                for(const auto& prepared:fsi_state_prepared) {
                    const auto& r=prepared.recipe;
                    if(row.at("vertex")==r.generic.vertex&&row.at("pixel")==Json(*r.generic.pixel)&&
                       row.at("raster")==r.generic.raster&&row.at("fsi_state")==r.state.words)prepared_source=true;
                }
                conserved=conserved&&prepared_source;
            }
        }
        conserved=conserved&&row_candidates==candidates&&row_selected==selected&&
            inactive_candidates==policy.at("inactive_catalog_admitted")&&
            inactive_selected==policy.at("inactive_catalog_selected")&&
            ((fsi_inactive_early_catalog_requested&&!fsi_early_catalog_active)||!inactive_candidates);
        Json tuples=Json::array();
        for(const auto& prepared:fsi_state_prepared) {
            const auto& r=prepared.recipe;uint64_t observed=0,accepted=0;
            for(const auto& row:state.at("coverage"))if(row.at("vertex")==r.generic.vertex&&
                row.at("pixel")==Json(*r.generic.pixel)&&row.at("raster")==r.generic.raster&&row.at("fsi_state")==r.state.words) {
                observed+=row.at("candidates").get<uint64_t>();accepted+=row.at("selected").get<uint64_t>();
            }
            if(observed) {++visited_prepared;visited_coverage=visited_coverage&&(accepted>0||fsi_state_expect_misses);}
            else ++unvisited_prepared;
            auto tuple=FsiStateRecipeJson(r);tuple["actual_candidates"]=observed;tuple["actual_selected"]=accepted;
            tuple["visited_unqueried"]=observed>0;tuples.push_back(std::move(tuple));
        }
        const bool positive=!wrap_frame_queries&&conserved&&selected>0&&visited_prepared>0&&visited_coverage&&
            (!fsi_state_expect_misses||fallback_positive);
        const bool query_refusal=wrap_frame_queries&&conserved&&selected==0&&admitted==0&&query_refusals>0;
        const bool missing_residency=!wrap_frame_queries&&!fsi_state_prewarm_limit&&fsi_state_expect_misses&&
            conserved&&selected==0&&admitted>0&&fallback_positive&&state.at("residency_refusals").get<uint64_t>()>0;
        Json coverage=state;coverage["material_composition"]=policy;coverage["counter_conservation"]=conserved;
        coverage["prepared_exact_tuples"]=std::move(tuples);coverage["visited_prepared_tuples"]=visited_prepared;
        coverage["unvisited_prepared_tuples"]=unvisited_prepared;coverage["positive_exact_tuple_coverage"]=positive;
        coverage["wrapped_query_refusal"]=query_refusal;coverage["expected_resident_misses"]=fsi_state_expect_misses;
        coverage["positive_generic_fallback_candidates"]=fallback_positive;coverage["prewarm_limit"]=fsi_state_prewarm_limit;
        coverage["zero_startup_residency_refusal"]=missing_residency;coverage["coverage_passed"]=positive||query_refusal||missing_residency;
        coverage["scope"]="Bounded original FSI material selection only; unvisited tuples are not positive evidence. Full memory/image/query parity and unchanged shipping counters require paired qualification.";
        std::ofstream file(output/"fsi-state-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!(positive||query_refusal||missing_residency))throw std::runtime_error("FSI material composition lacks conserved positive selection, query refusal or explicit zero-residency refusal");
    } else if(fsi_state_requested) {
        const auto& state=replay.report().at("fsi_state_specialization");
        Json coverage=state;coverage["required_exact_tuples"]=Json::array();
        bool positive=state.value("selected",uint64_t(0))>0&&state.value("prepared",uint64_t(0))>0&&
            state.value("gameplay_compilations",uint64_t(1))==0&&state.value("gameplay_pipeline_creations",uint64_t(1))==0;
        for(const auto& prepared:fsi_state_prepared) {
            const auto& recipe=prepared.recipe;bool found=false;
            for(const auto& row:state.at("coverage"))if(row.at("vertex")==recipe.generic.vertex&&
                row.at("pixel")== (recipe.generic.pixel?Json(*recipe.generic.pixel):Json(nullptr))&&
                row.at("raster")==recipe.generic.raster&&row.at("fsi_state")==recipe.state.words&&
                row.value("selected",uint64_t(0))>0&&(!wrap_frame_queries||row.value("queried_selected",uint64_t(0))>0))found=true;
            auto required=FsiStateRecipeJson(recipe);required["actual_selected_and_queried"]=found;
            coverage["required_exact_tuples"].push_back(std::move(required));positive=positive&&found;
        }
        bool fallback_positive=false;
        for(const auto& row:state.at("coverage"))if(row.value("candidates",uint64_t(0))>row.value("selected",uint64_t(0))&&
            (!wrap_frame_queries||row.value("queried_candidates",uint64_t(0))>row.value("queried_selected",uint64_t(0))))fallback_positive=true;
        coverage["expected_resident_misses"]=fsi_state_expect_misses;coverage["positive_generic_fallback_candidates"]=fallback_positive;
        positive=positive&&(!fsi_state_expect_misses||fallback_positive);
        coverage["positive_exact_tuple_coverage"]=positive;
        std::ofstream file(output/"fsi-state-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file)throw std::runtime_error("Cannot save FSI exact tuple coverage");
        if(!positive)throw std::runtime_error("FSI specialized tuple has no positive actual/query selection; prune sidecar before parity/performance validation");
    }
    if(readonly_requested) {
        const auto& stats=replay.report().at("builtin_readonly_queries");
        const bool positive=stats.at("prepared").get<uint64_t>()>0&&stats.at("selected").get<uint64_t>()>0&&
            stats.at("queried_selected")==stats.at("selected")&&stats.at("module_refusals")==0&&
            stats.at("vertex_refusals")==0&&stats.at("residency_refusals")==0;
        Json coverage{{"requested",true},{"positive_actual_readonly_queries",positive},{"counters",stats},
            {"scope","Actual audited nullPS read-only query selection; exact image/query parity is a separate paired oracle"}};
        std::ofstream file(output/"builtin-readonly-query-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!positive)throw std::runtime_error("Read-only query replay coverage is missing or refused");
    }

    if(d32_requested) {
        const auto& stats=replay.report().at("native_readonly_depth_attachment");
        const auto& no_write=replay.report().at("native_readonly_depth_no_write_metadata");
        if(stats.at("lower_bound_query")!=config.native_readonly_depth_lower_bound_query)
            throw std::runtime_error("Readonly lower-bound query mode was not applied");
        const bool metadata_coverage=no_write.at("enabled")==config.native_readonly_depth_no_write_metadata&&
            no_write.at("refusals")==0&&(config.native_readonly_depth_no_write_metadata?
                no_write.at("selected").get<uint64_t>()>0:no_write.at("selected")==0);
        const bool positive=stats.at("prepared").get<uint64_t>()>0&&stats.at("selected").get<uint64_t>()>0&&
            stats.at("conversions").get<uint64_t>()>0&&metadata_coverage&&
            stats.at("promotions").get<uint64_t>()+no_write.at("selected").get<uint64_t>()==stats.at("selected").get<uint64_t>()&&
            stats.at("queried_selected")==stats.at("selected")&&stats.at("promotion_refusals")==0;
        const bool read_only_positive=!config.native_readonly_depth_read_only_pass||
            (stats.at("read_only_pass_active")==true&&stats.at("read_only_depth_barriers_avoided").get<uint64_t>()>0);
        const bool barrier_positive=!config.native_readonly_depth_skip_edram_barrier||
            (stats.at("skip_edram_barrier_enabled")==true&&stats.at("skipped_edram_barriers")==stats.at("selected")&&
             stats.at("retained_edram_barriers")==0&&stats.at("skipped_edram_barriers").get<uint64_t>()>0);
        Json coverage{{"requested",true},{"positive_actual_native_readonly_depth",positive},{"skip_edram_barrier_requested",config.native_readonly_depth_skip_edram_barrier},
            {"positive_skipped_edram_barriers",config.native_readonly_depth_skip_edram_barrier&&barrier_positive},{"counters",stats},
            {"no_write_metadata",no_write},{"no_write_metadata_coverage",metadata_coverage},
            {"scope","Exact readonly Native D32 selection and accepted version promotion; paired image/full EDRAM/query oracle remains required"}};
        std::ofstream file(output/"native-readonly-depth-attachment-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!positive||!barrier_positive||!read_only_positive)throw std::runtime_error("Native readonly D32 replay coverage is missing or refused");
    }
    if(hoist_fsi_requested!=NativeFsiHoistMode::Disabled) {
        const auto& stats=replay.report().at("fsi_immutable_state_hoist");
        const auto selected=stats.at("selected_draws").get<uint64_t>();
        const auto queried=stats.at("queried_draws").get<uint64_t>();
        const auto state_draws=stats.at("immutable_state_load_draws").get<uint64_t>();
        const auto incoming=stats.at("incoming_depth_state_draws").get<uint64_t>();
        const bool conserved=stats.at("enabled")==true&&stats.at("mode")==unsigned(hoist_fsi_requested)&&
            stats.at("variant_version")==NativeFsiHoistCompiler::CacheVersion(hoist_fsi_requested)&&
            stats.at("canonical_null_pixel_shader")==true&&queried<=selected&&state_draws==selected&&incoming<=selected&&
            (hoist_fsi_requested!=NativeFsiHoistMode::ImmutableState||incoming==0);
        const bool positive=conserved&&!hoist_fsi_prepared.empty()&&selected>0&&
            (hoist_fsi_requested!=NativeFsiHoistMode::ImmutableStateAndIncomingDepth||incoming>0)&&
            (!wrap_frame_queries||queried==selected);
        Json coverage{{"requested",true},{"mode",unsigned(hoist_fsi_requested)},{"counters",stats},
            {"counter_conservation",conserved},{"prepared_sources",hoist_fsi_prepared.size()},
            {"prepared_spirv_bytes",hoist_fsi_prepared_bytes},{"positive_host_state_selection",positive},
            {"fragment_execution_measured",false},
            {"scope","Accepted non-null derived FSI draws and host depth-or-stencil state; paired full EDRAM/image/query parity and isolated helper GPU proof remain required"}};
        std::ofstream file(output/"fsi-hoist-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!positive)throw std::runtime_error("FSI hoist replay lacks positive host state selection or conserved counters");
    }
    if(config.fsi_draw_pass_groups) {
        replay.FlushGpuWork();
        std::ofstream file(output/"fsi-draw-pass-groups-coverage.json");file<<replay.report().at("fsi_draw_pass_groups").dump(2)<<'\n';
    }
    if(config.packed_depth_render_pass_groups) {
        replay.FlushGpuWork();const auto& stats=replay.report().at("packed_depth_render_pass_groups");
        const uint64_t selected=stats.at("selected"),groups=stats.at("closed_render_pass_groups"),avoided=stats.at("render_passes_avoided");
        const bool conserved=stats.at("enabled")==true&&selected==groups+avoided&&
            stats.at("edram_barriers_avoided")==avoided&&stats.at("dummy_barriers_avoided")==avoided&&
            stats.at("eligible").get<uint64_t>()==selected+stats.at("preparation_refusals").get<uint64_t>()&&
            stats.at("candidates").get<uint64_t>()==stats.at("eligible").get<uint64_t>()+stats.at("refusals").get<uint64_t>()&&
            stats.at("maximum_group_draws").get<uint64_t>()<=16;
        const bool positive=conserved&&selected>0&&avoided>0;
        const bool query_refusal=wrap_frame_queries&&conserved&&selected==0&&
            stats.at("query_refusals").get<uint64_t>()>0;
        Json coverage{{"requested",true},{"coverage_passed",positive||query_refusal},{"counters",stats},
            {"counter_conservation",conserved},{"positive_actual_groups",positive},{"wrapped_query_refusal",query_refusal},
            {"scope","Canonical packed writer subpass grouping only; counts are not timings. Actual complete images,10MiB memory and query parity remain separate required evidence."}};
        std::ofstream file(output/"packed-depth-render-pass-groups-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!(positive||query_refusal))throw std::runtime_error("Packed writer groups lack positive grouping or wrapped-query refusal");
    }
    if(config.native_early_depth) {
        const auto& stats=replay.report().at("native_early_depth");
        const auto selected=stats.at("selected").get<uint64_t>();
        const auto queried=stats.at("queried_selected").get<uint64_t>();
        const bool conserved=stats.at("enabled")==true&&stats.at("supported")==true&&
            queried==0&&stats.at("promotions")==selected&&
            selected<=stats.at("candidates").get<uint64_t>()&&
            stats.at("hardware_depth_write")==false&&stats.at("canonical_packed_epilogue_authoritative")==true;
        const bool positive=conserved&&selected>0&&stats.at("cache_hits").get<uint64_t>()>0;
        const bool query_refusal=wrap_frame_queries&&conserved&&selected==0&&stats.at("prepared").get<uint64_t>()>0;
        const bool residency_refusal=!early_prewarm_limit&&conserved&&selected==0&&stats.at("residency_refusals").get<uint64_t>()>0;
        uint64_t phase_replacements=0;
        if(retained_phase_requested&&conserved&&selected==0&&stats.at("candidates")==0&&
           stats.at("prepared").get<uint64_t>()>0) {
            const auto& phase=replay.report().at("retained_opaque_phase");
            const auto& counts=phase.at("selected_by_recipe");
            if(phase.at("enabled")==true&&phase.at("supported")==true&&phase.at("queried_selected")==0&&
               counts.is_array()&&counts.size()==kRetainedOpaquePhaseRecipes.size())
                for(size_t i=0;i<kRetainedOpaquePhaseRecipes.size();++i) {
                    const auto& recipe=kRetainedOpaquePhaseRecipes[i];
                    if(recipe.vertex==kNativeEarlyDepthVertexSource&&recipe.pixel==kNativeEarlyDepthPixelSource)
                        phase_replacements+=counts[i].get<uint64_t>();
                }
        }
        const bool phase_covered=phase_replacements>0;
        Json coverage{{"requested",true},{"counters",stats},{"counter_conservation",conserved},
            {"positive_actual_selection",positive},{"wrapped_query_refusal",query_refusal},
            {"retained_phase_replaced_base_early",phase_covered},{"retained_phase_base_early_draws",phase_replacements},
            {"zero_startup_residency_refusal",residency_refusal},{"fragment_rejection_measured",false},
            {"scope","Host selection/lifetime counters only; paired complete EDRAM/images and controlled early-test GPU proof remain required"}};
        std::ofstream file(output/"native-early-depth-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!(positive||query_refusal||residency_refusal||phase_covered))
            throw std::runtime_error("Native early-depth replay has no positive selection or explicit refusal coverage");
    }
    if(config.native_early_depth_catalog) {
        const auto& stats=replay.report().at("native_early_depth_catalog");
        const auto selected=stats.at("selected").get<uint64_t>();
        const bool conserved=stats.at("enabled")==true&&stats.at("supported")==true&&
            stats.at("queried_selected")==0&&stats.at("promotions")==selected&&
            selected<=stats.at("candidates").get<uint64_t>()&&stats.at("hardware_depth_write")==false&&
            stats.at("canonical_packed_epilogue_authoritative")==true;
        const bool positive=conserved&&selected>0&&stats.at("cache_hits").get<uint64_t>()>0;
        const bool query_refusal=wrap_frame_queries&&conserved&&selected==0&&stats.at("prepared").get<uint64_t>()>0;
        const bool residency_refusal=!early_catalog_prewarm_limit&&conserved&&selected==0&&stats.at("residency_refusals").get<uint64_t>()>0;
        Json coverage{{"requested",true},{"counters",stats},{"counter_conservation",conserved},
            {"positive_actual_selection",positive},{"wrapped_query_refusal",query_refusal},
            {"zero_startup_residency_refusal",residency_refusal},{"fragment_rejection_measured",false},
            {"scope","Actual host catalogue selection only; full-memory/image/query hardware parity required"}};
        std::ofstream file(output/"native-early-depth-catalog-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!(positive||query_refusal||residency_refusal))throw std::runtime_error("Early-depth catalogue lacks positive selection/refusal coverage");
    }
    if(exact_fsi_requested) {
        const auto& stats=replay.report().at("exact_fsi_stores");
        const auto selected=stats.at("selected_draws").get<uint64_t>();
        const auto queried=stats.at("queried_draws").get<uint64_t>();
        const auto candidate=stats.at("depth_identity_candidate_draws").get<uint64_t>();
        const auto certified=stats.at("depth_identity_certified_draws").get<uint64_t>();
        const auto uncertified=stats.at("depth_identity_uncertified_draws").get<uint64_t>();
        const auto color_draws=stats.at("color_overwrite_state_draws").get<uint64_t>();
        const auto color_targets=stats.at("color_overwrite_state_targets").get<uint64_t>();
        const bool conserved=stats.at("enabled")==true&&queried<=selected&&candidate<=selected&&
            certified<=candidate&&uncertified==candidate-certified&&color_draws<=selected&&
            color_targets>=color_draws&&color_targets<=selected*4;
        const bool positive=conserved&&!exact_fsi_prepared.empty()&&selected>0&&
            certified>0&&color_draws>0&&(!wrap_frame_queries||queried==selected);
        Json coverage{{"requested",true},{"counters",stats},{"counter_conservation",conserved},
            {"prepared_sources",exact_fsi_prepared.size()},{"prepared_spirv_bytes",exact_fsi_prepared_bytes},
            {"positive_host_state_selection",positive},{"fragment_branches_measured",false},
            {"scope","Accepted non-null FSI variant and host certified depth/full-word color state; isolated GPU helper coverage and paired full EDRAM/image/query parity remain required"}};
        std::ofstream file(output/"exact-fsi-stores-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file||!positive)throw std::runtime_error("Exact FSI replay lacks positive host state selection or conserved counters");
    }
    if(page_version_union_requested) {
        const auto& versions=replay.report().at("cpu_texture_footprint_cache").at("page_versions");
        const auto calls=versions.at("calls").get<uint64_t>(),hits=versions.at("memo_hits").get<uint64_t>(),
            scans=versions.at("rescans").get<uint64_t>(),union_scans=versions.at("union_rescans").get<uint64_t>(),
            baseline=versions.at("baseline_page_visits").get<uint64_t>(),
            performed=versions.at("performed_page_visits").get<uint64_t>(),
            avoided=versions.at("avoided_page_visits").get<uint64_t>();
        const bool conserved=versions.at("union_enabled").get<bool>()&&calls>=hits&&calls-hits==scans&&
            union_scans==scans&&baseline>=performed&&baseline-performed==avoided;
        const bool positive=conserved&&scans>0&&performed>0&&avoided>0;
        // Save the exact observed scan coverage even when this capture has no
        // duplicate page visits. Parity alone cannot prove useful activation.
        Json coverage{{"requested",true},{"counters",versions},{"counter_conservation",conserved},
            {"positive_rescan_savings",positive},
            {"scope","Actual replay rescans and visit arithmetic; no live timing or FPS claim"}};
        std::ofstream file(output/"page-version-coverage.json");file<<coverage.dump(2)<<'\n';
        if(!file)throw std::runtime_error("Cannot save page version scan coverage");
        if(!conserved)throw std::runtime_error("Page-union replay scan counters are inconsistent");
        if(!positive)throw std::runtime_error("Page-union replay has no positive actual rescan savings; parity alone is insufficient coverage");
    }

    if(descriptor_content_requested) {
        const auto& cache=replay.report().at("draw_descriptor_content_cache");
        if(!cache.value("enabled",false)||cache.value("skipped_images",uint64_t(0))==0||
           cache.value("skipped_samplers",uint64_t(0))==0||cache.value("aborts",uint64_t(0))!=0)
            throw std::runtime_error("Descriptor content replay needs positive image/sampler skips and no aborted transaction");
    }
    if(multi_draw_requested) {
        const auto& groups=replay.report().at("draw_command_groups");
        const auto& queue=replay.report().at("gpu_work_queue");
        if(groups.value("maximum_group_draws",uint32_t(0))<2||
           groups.value("logical_grouped_draws",uint64_t(0))<=groups.value("closed_groups",uint64_t(0))||
           groups.value("physical_lists",uint64_t(0))>=queue.value("draw_lists",uint64_t(0))+queue.value("transfer_lists",uint64_t(0)))
            throw std::runtime_error("Multi-draw replay never shared a physical command buffer; parity would be vacuous");
    }
    if(discard_dummy_requested&&!attachmentless_requested) {
        const auto& dummy=replay.report().at("dummy_fsi_render_pass");
        if(dummy.value("installs",uint64_t(0))==0||dummy.value("selected_draws",uint64_t(0))==0||
           dummy.value("refusals",uint64_t(0))!=0)
            throw std::runtime_error("Dummy FSI discard replay did not use the exact candidate pass");
    }
    if(attachmentless_requested) {
        const auto& empty=replay.report().at("attachmentless_fsi_render_pass");
        std::ofstream coverage(output/"attachmentless-fsi-coverage.json");
        coverage<<empty.dump(2)<<'\n';
        if(!coverage)throw std::runtime_error("Cannot write attachmentless FSI coverage");
        if(!empty.value("active",false)||empty.value("installs",uint64_t(0))==0||
           empty.value("selected_draws",uint64_t(0))==0||empty.value("refusals",uint64_t(0))!=0||
           replay.report().at("fsi_raster_attachment").value("creations",uint64_t(0))!=0||
           (wrap_frame_queries&&empty.value("queried_draws",uint64_t(0))==0))
            throw std::runtime_error("Attachmentless FSI replay has insufficient actual candidate coverage");
    }
    if(wrap_frame_queries) {
        const auto& queries=replay.report().at("diagnostic_frame_queries");
        if(queries.empty()||std::none_of(queries.begin(),queries.end(),[](const auto& query) {
            return query.at("samples").template get<uint64_t>()>0;
        }))throw std::runtime_error("Diagnostic frame queries had no actual GPU samples");
    }
    if(opaque_requested&&replay.report().at("opaque_color_overwrite").value("selected",uint64_t(0))==0)
        throw std::runtime_error("Opaque replay did not select an experimental draw; parity would be vacuous");
    if(forward_requested&&replay.report().at("forward_control_flow").value("selected",uint64_t(0))==0)
        throw std::runtime_error("Forward-CFG replay did not select an experimental draw; parity would be vacuous");
    if(forward_requested&&wrap_frame_queries&&
       replay.report().at("forward_control_flow").value("queried_selected",uint64_t(0))==0)
        throw std::runtime_error("Forward-CFG replay did not select a queried experimental draw");
    if(forward_vertex_requested&&replay.report().at("forward_vertex_control_flow").value("selected",uint64_t(0))==0)
        throw std::runtime_error("Forward-VS replay candidate was not selected; parity would be vacuous");
    if(forward_vertex_requested&&wrap_frame_queries&&replay.report().at("forward_vertex_control_flow").value("queried_selected",uint64_t(0))==0)
        throw std::runtime_error("Forward-VS replay did not exercise actual queried draws");
    if(catalog_requested) {
        const auto& catalog=replay.report().at("forward_pixel_catalog");
        // Preserve coverage even when a source must be pruned after this run.
        std::ofstream coverage(output/"forward-pixel-catalog-coverage.json");coverage<<catalog.dump(2)<<'\n';
        if(!coverage)throw std::runtime_error("Cannot save catalog source coverage");
        if(!catalog.value("program_ready",false)||catalog.at("sources").size()!=kForwardPixelCatalogSources.size())
            throw std::runtime_error("Catalog replay did not prepare the complete source allowlist");
        for(const auto& [source,counts]:catalog.at("sources").items()) {
            if(counts.value("prepared",uint64_t(0))==0||counts.value("selected",uint64_t(0))==0||
               (wrap_frame_queries&&counts.value("queried_selected",uint64_t(0))==0))
                throw std::runtime_error("Catalog source lacks positive actual/query selection; prune before benchmarking: "+source);
        }
    }
    if(stencil_material_native) {
        const auto stats=replay.report().at("native_stencil_material_cutoff");
        std::ofstream file(output/"native-stencil-material-cutoff.json");file<<stats.dump(2)<<'\n';file.close();
        if(!file||stats.at("prepared")!=1||!stats.at("program_ready").get<bool>()||stats.at("counter_descriptors")!=0||
           stats.at("counter_buffers")!=0||stats.at("counter_readbacks")!=0)
            throw std::runtime_error("Native stencil material setup or zero-counter invariant failed");
        if(wrap_frame_queries) {
            if(stats.at("selected")!=0||stats.at("query_refusals")!=stats.at("candidates"))
                throw std::runtime_error("Native stencil material all-query refusal failed");
        } else if(stats.at("selected")==0)throw std::runtime_error("Native stencil material selection vacuous");
    }
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
    if(stencil_material_requested&&!stencil_material_native) {
        const auto stats=replay.report().at("stencil_material_cutoff");
        std::ofstream file(output/"stencil-material-census.json");file<<stats.dump(2)<<'\n';file.close();
        if(!file||stats.at("prepared")!=1||stats.at("pending")!=0||stats.at("accepted")!=stats.at("retired"))
            throw std::runtime_error("Stencil material diagnostic preparation/retirement incomplete");
        if(wrap_frame_queries) {
            if(stats.at("selected")!=0||stats.at("query_refusals")!=stats.at("candidates"))
                throw std::runtime_error("Stencil material all-query refusal failed");
        } else if(stats.at("selected")==0)
            throw std::runtime_error("Stencil material diagnostic selection vacuous; see refusal reasons");
    }
#endif
    if(fragment_census) {
        const auto census=replay.FragmentInvocationReport();
        std::ofstream file(output/"fragment-invocations.json");file<<census.dump(2)<<'\n';file.close();
        if(!file)throw std::runtime_error("Cannot save fragment invocation census");
        if(!census.value("supported",false)||census.value("incomplete",true)||census.value("completed",uint64_t(0))==0)
            throw std::runtime_error("Fragment invocation census unsupported/incomplete/vacuous; see saved diagnostic");
    }
    if(physical_timestamps) {
        std::ofstream file(output/"gpu-physical.json");file<<replay.GpuPhysicalTimingReport(true).dump(2)<<'\n';
    }
    if(draw_timestamps) {
        std::ofstream file(output/"gpu-draws.json");file<<replay.GpuDrawTimingReport(true).dump(2)<<'\n';
        if(!file)throw std::runtime_error("Cannot save draw timestamp report");
    }
    if(retained_state_fixture) {
        // Scene verifies original event/type/frame envelopes. This explicit tool
        // mode additionally changes device-state bytes, so keep its provenance honest.
        auto report=replay.report();report["live_provenance_unmodified"]=false;
        report["retained_opaque_state_fixture"]=retained_state_fixture->metadata;
        std::ofstream witness(output/"retained-opaque-state-fixture.json");witness<<retained_state_fixture->metadata.dump(2)<<'\n';
        std::ofstream saved(output/"replay.json");saved<<report.dump(2)<<'\n';
        if(!witness||!saved)throw std::runtime_error("Cannot retain controlled state fixture provenance");
        std::cout<<report.dump(2)<<'\n';
    }else std::cout<<replay.report().dump(2)<<'\n';return 0;
#endif
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
