#include "retained_opaque_scene_fixture.h"
#include "renderer/native/retained_opaque_writer.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/live_draw.h"
#include "renderer/native/pipeline_prewarm.h"
#include "plume_render_interface.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace gow3::renderer::validation {
namespace {
bool CapturedEdram(const SceneExecutorConfig& config) {
    const auto path=config.capture/"gpu-history.json";
    if(!std::filesystem::exists(path))return false;
    if(std::filesystem::file_size(path)>16u*1024*1024)throw std::runtime_error("Partial history metadata extent");
    std::ifstream file(path);nlohmann::json history;file>>history;
    if(!file)throw std::runtime_error("Cannot parse partial history metadata");
    return !history.at("edram").is_null();
}
}

void RetainedOpaquePartialOwnedFixture(const SceneExecutorConfig& original,
    std::span<const LiveOperation* const> operations,std::span<const uint32_t> vertex,
    std::span<const uint32_t> original_pixel,bool seed_full_edram) {
    if(!original.retained_opaque_writer||!original.retained_opaque_program||original.output.empty())
        throw std::runtime_error("Partial retained fixture requires a prepared candidate and output");
    uint32_t through=UINT32_MAX;unsigned consecutive=0;
    for(const auto* operation:operations) {
        const auto* draw=operation?operation->Draw():nullptr;
        const bool pinned=draw&&draw->vertex_shader&&draw->pixel_shader&&
            draw->vertex_shader->hash.View()==kRetainedOpaqueVertex&&draw->pixel_shader->hash.View()==kRetainedOpaquePixel;
        consecutive=pinned?consecutive+1:0;
        if(consecutive==3) {
            if(operation->Event()>=UINT32_MAX)throw std::runtime_error("Partial retained event exceeds diagnostic bound");
            through=uint32_t(operation->Event());break;
        }
    }
    if(through==UINT32_MAX)throw std::runtime_error("Actual capture has no three consecutive pinned writer draws");
    auto config=original;config.output=original.output/"partial-owned-retained-fixture";
    config.present={}; // Standalone owned device, with the ordinary file-backed outputs.
    nlohmann::json witness;
    {
        SceneExecutor partial(std::move(config));
        for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
            const NativeFsiPipelineRecipe recipe{std::string(kRetainedOpaqueVertex),std::string(kRetainedOpaquePixel),
                {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            partial.PrewarmFsiPipeline(recipe,vertex,original_pixel);
            if(!partial.PrewarmRetainedOpaquePipeline(recipe,vertex,original_pixel))
                throw std::runtime_error("Partial retained startup candidate refused");
        }
        // SceneRun will import actual captured EDRAM. A redundant zero seed must
        // not be described as the initial history of that captured run.
        if(seed_full_edram&&!CapturedEdram(original))
            partial.InitializeEdram(std::vector<uint8_t>(10u*1024*1024,0));
        partial.RunLiveCapturePartialForRetainedFixture(operations,through);
        witness=partial.report();
        const auto& stats=witness.at("retained_opaque_writer");
        const auto& pending=witness.at("partial_owned_retained_fixture");
        if(stats.at("selected").get<uint64_t>()!=3||stats.at("restores")!=1||
           stats.at("reuses").get<uint64_t>()<2||stats.at("materializations")!=0||
           stats.at("maximum_run").get<uint64_t>()<3||stats.at("retained_image_bytes").get<uint64_t>()==0||
           stats.at("source_refusals")!=0||stats.at("resource_refusals")!=0||stats.at("residency_refusals")!=0||
           !pending.at("pending_gpu_work").get<bool>()||!pending.at("dirty_retained_authority").get<bool>()||
           !partial.HasPendingGpuWork())
            throw std::runtime_error("Partial owned retained fixture lacks accepted dirty pending generation");
        // Do not explicitly flush, export, end frame, or destroy the borrowed
        // graphics device. Normal Scene destruction must store/drain the dirty
        // generation and destroy images/pipelines before its OWNED VkDevice.
    }
    witness["destructor_returned"]=true;witness["through_event"]=through;
    witness["scope"]="Actual three-draw typed capture prefix and owned pending shutdown; full-memory/image/query equivalence is a separate paired complete replay";
    std::ofstream file(original.output/"retained-opaque-partial-owned-lifetime.json");file<<witness.dump(2)<<'\n';
    if(!file)throw std::runtime_error("Cannot retain partial owned writer lifetime witness");
}

void RetainedOpaqueCatalogPartialOwnedFixture(const SceneExecutorConfig& original,
    std::span<const LiveOperation* const> operations,std::span<const RetainedOpaqueCatalogFixtureSource> sources,
    bool seed_full_edram,uint32_t through_override,bool require_source_switch) {
    if(!original.retained_opaque_catalog||!original.retained_opaque_catalog_programs||original.output.empty()||
       sources.size()!=kRetainedOpaqueCatalogMaxPrograms)
        throw std::runtime_error("Partial catalogue fixture requires all exact startup programmes");
    uint32_t through=UINT32_MAX;unsigned consecutive=0;
    for(const auto* operation:operations) {
        const auto* draw=operation?operation->Draw():nullptr;
        const bool admitted=draw&&draw->vertex_shader&&draw->pixel_shader&&
            RetainedOpaqueCatalogPair(draw->vertex_shader->hash.View(),draw->pixel_shader->hash.View());
        consecutive=admitted?consecutive+1:0;
        if(consecutive==3) {
            if(operation->Event()>=UINT32_MAX)throw std::runtime_error("Partial catalogue event exceeds bounded fixture");
            through=uint32_t(operation->Event());break;
        }
    }
    if(through_override!=UINT32_MAX) {
        if(!require_source_switch||std::none_of(operations.begin(),operations.end(),[&](const auto* operation) {
            return operation&&operation->Event()==through_override&&operation->Draw();
        }))throw std::runtime_error("Controlled source-switch shutdown event invalid");
        through=through_override;
    }
    if(through==UINT32_MAX)throw std::runtime_error("Capture has no three consecutive catalogue source draws");
    auto config=original;config.output=original.output/"partial-owned-retained-catalog-fixture";config.present={};
    nlohmann::json witness;
    {
        SceneExecutor partial(std::move(config));
        for(const auto& source:sources)for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
            const NativeFsiPipelineRecipe recipe{std::string(source.pair.vertex),std::string(source.pair.pixel),
                {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            partial.PrewarmFsiPipeline(recipe,source.vertex,source.pixel);
        }
        for(const auto& source:sources)for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
            const NativeFsiPipelineRecipe recipe{std::string(source.pair.vertex),std::string(source.pair.pixel),
                {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            if(!partial.PrewarmRetainedOpaquePipeline(recipe,source.vertex,source.pixel))
                throw std::runtime_error("Partial catalogue startup admission refused");
        }
        // SceneRun will import actual captured EDRAM. A redundant zero seed must
        // not be described as the initial history of that captured run.
        if(seed_full_edram&&!CapturedEdram(original))
            partial.InitializeEdram(std::vector<uint8_t>(10u*1024*1024,0));
        partial.RunLiveCapturePartialForRetainedFixture(operations,through);witness=partial.report();
        const auto& stats=witness.at("retained_opaque_catalog");const auto& pending=witness.at("partial_owned_retained_fixture");
        if(stats.at("prepared")!=kRetainedOpaqueCatalogMaxPipelines||stats.at("modules_created")!=kRetainedOpaqueCatalogMaxPrograms||
           stats.at("selected").get<uint64_t>()<3||stats.at("current_run").get<uint64_t>()<3||
           stats.at("reuses").get<uint64_t>()<2||stats.at("restores").get<uint64_t>()!=stats.at("materializations").get<uint64_t>()+1||
           stats.at("retained_image_bytes").get<uint64_t>()==0||stats.at("source_refusals")!=0||stats.at("resource_refusals")!=0||
           stats.at("residency_refusals")!=0||stats.at("prewarm_refusals")!=0||
           (require_source_switch&&stats.at("shader_switches").get<uint64_t>()<2)||
           !pending.at("pending_gpu_work").get<bool>()||!pending.at("dirty_retained_authority").get<bool>()||!partial.HasPendingGpuWork())
            throw std::runtime_error("Partial catalogue lacks exact accepted dirty pending generation");
        // No explicit drain/export: all catalogue modules, current images and
        // packet leases must die before this owned Scene's Vulkan device.
    }
    witness["destructor_returned"]=true;witness["through_event"]=through;witness["source_switch_fixture"]=require_source_switch;
    witness["live_provenance_unmodified"]=!require_source_switch;
    witness["scope"]=require_source_switch?
        "Controlled complete actual-donor triplet with two retained source switches and owned dirty pending shutdown; identical altered OFF/ON full-memory/image/query equality remains a separate oracle":
        "Actual admitted capture prefix with current run at least3 and owned pending shutdown; source-switch/image/full10MiB/query equality remain separate complete-replay oracles";
    std::ofstream file(original.output/"retained-opaque-catalog-partial-owned-lifetime.json");file<<witness.dump(2)<<'\n';
    if(!file)throw std::runtime_error("Cannot retain partial catalogue lifetime witness");
}
}
