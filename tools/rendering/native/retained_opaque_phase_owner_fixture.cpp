#include "retained_opaque_phase_owner_fixture.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/retained_opaque_phase_catalog.h"
#include "renderer/native/retained_opaque_writer.h"
#include <algorithm>
#include <fstream>
#include <set>

namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
constexpr uint32_t kBytes=10u*1024*1024,kPrefix=289,kFirst=308,kSecond=292,kFrame=17672;
constexpr std::string_view kVertex="75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802";
constexpr std::string_view kPixel="55c79ab30e62247886a8c74dc440f04695ffd0f6f88842ffa1aea0b6829ee6c6";
// Actual C++ DecodeDrawState proof: mode0x18002, bit2 clear => CCW enum2.
constexpr std::array<uint32_t,6> kRaster{3,2,1,4,0,0};
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
uint64_t Count(const Json& row,const char* field) {return row.at("retained_opaque_phase").at(field).get<uint64_t>();}
void CheckPrefixRange(const Json& report,std::span<const LiveOperation* const> ops,uint32_t through) {
    const uint32_t expected=through==289?287:through==487?487:through==1535?1533:through==1736?1736:UINT32_MAX;
    Require(expected!=UINT32_MAX,"Opaque phase unknown authenticated prefix boundary");
    uint32_t last=UINT32_MAX;
    for(const auto* op:ops) {
        Require(op,"Opaque phase prefix null operation");
        if(op->Event()>through)break;
        Require(last==UINT32_MAX||op->Event()>last,"Opaque phase prefix operation order changed");
        last=op->Event();
    }
    // Requested cutoffs may fall between actual operations: 288/289 and
    // 1534/1535 have no captured GPU operation. Check both facts independently.
    Require(last==expected,"Opaque phase actual prefix operation boundary changed");
    Require(report.at("requested_range_complete").get<bool>()&&
        report.at("last_accepted_event")==expected,"Opaque phase prefix range incomplete or wrong last operation");
}
void Write(const std::filesystem::path& p,std::span<const uint8_t> bytes) {
    std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    Require(bool(f),"Opaque owner fixture cannot preserve full EDRAM");
}
Json Observe(const NativeRetainedOpaqueResourceWitness& w) {
    return {{"valid",w.valid},{"dirty",w.dirty},{"generation",w.ticket.generation},
        {"color_version",w.ticket.color},{"depth_version",w.ticket.depth},
        {"color",w.color},{"depth",w.depth},{"framebuffer",w.framebuffer},
        {"accepted_work",w.accepted_work},{"completed_work",w.completed_work}};
}
Json Observe(const SceneExecutor::RetainedOpaquePhaseOwnerWitness& w) {
    return {{"removed_pipeline",w.removed_pipeline},{"distinct_vulkan_modules",w.distinct_vulkan_modules},
        {"certified_bytes",w.certified_bytes},{"dirty",w.dirty},
        {"pipeline_certificate_unchanged",w.pipeline_certificate_unchanged},
        {"old_shader",w.old_shader},{"new_shader",w.new_shader},{"old_pipeline",w.old_pipeline},
        {"certified_vertex",w.certified_vertex},{"certified_pixel",w.certified_pixel},
        {"accepted_work",w.accepted_work},{"completed_work",w.completed_work},
        {"generation",w.generation},{"color_version",w.color_version},{"depth_version",w.depth_version}};
}
const TypedLiveDraw& Draw(std::span<const LiveOperation* const> ops,uint32_t event) {
    const TypedLiveDraw* result=nullptr;
    for(const auto* op:ops) {
        Require(op,"Opaque owner fixture null operation");
        Require(op->Kind()!="query_begin"&&op->Kind()!="query_end","Opaque owner fixture requires unqueried authenticated capture");
        if(op->Event()!=event)continue;
        Require(!result&&op->Draw(),"Opaque owner fixture event missing/duplicated");result=op->Draw();
    }
    Require(result&&result->frame==kFrame&&result->vertex_shader&&result->pixel_shader&&
        result->vertex_shader->hash.View()==kVertex&&result->pixel_shader->hash.View()==kPixel&&
        result->function==DrawFunction::DrawIndexedPrimitive&&result->args[1]==4&&!result->rectangle_list,
        "Opaque owner fixture exact actual draw identity changed");
    return *result;
}
void CheckSetup(const SceneExecutorConfig& original,std::span<const LiveOperation* const> operations,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool seed_full_edram) {
    Require(seed_full_edram&&!std::filesystem::exists(original.capture/"gpu-history.json"),
        "Opaque phase fixture requires explicit symmetric zero10MiB seed and absent captured history");
    Require(original.retained_opaque_phase&&original.retained_opaque_phase_programs&&original.native_fsi&&
        original.unified_gpu_queue&&original.async_gpu_submissions&&!original.retained_opaque_writer&&
        !original.retained_opaque_catalog&&!original.native_early_depth_material_retention&&
        operations.size()<=65536&&sources.size()==65,"Opaque phase fixture explicit Native prerequisites");
    (void)Draw(operations,kFirst);(void)Draw(operations,kSecond);
    std::set<NativeFsiPipelineRecipe> unique;
    for(const auto& source:sources)Require(source.recipe.pixel&&
        NativeRetainedOpaquePhaseCatalogRecipe(source.recipe.vertex,*source.recipe.pixel,source.recipe.raster)&&
        source.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&source.recipe.depth_format==-1&&
        unique.insert(source.recipe).second&&!source.vertex.empty()&&!source.pixel.empty(),
        "Opaque phase fixture exact65 startup recipes");
    Require(unique.contains(NativeFsiPipelineRecipe{std::string(kVertex),std::string(kPixel),kRaster,
        int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1}),"Opaque phase first exact recipe missing");
}
Json PrewarmAndSeed(SceneExecutor& scene,const SceneExecutorConfig& original,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool phase) {
    for(const auto& source:sources)scene.PrewarmFsiPipeline(source.recipe,source.vertex,source.pixel);
    if(phase) {
        size_t admitted=0;
        for(const auto& source:sources) {
            if(!NativeRetainedOpaquePhaseRecipe(source.recipe.vertex,*source.recipe.pixel,source.recipe.raster))continue;
            Require(scene.PrewarmRetainedOpaquePhasePipeline(source.recipe,source.vertex,source.pixel),
                "Opaque phase fixture startup residency refused");
            ++admitted;
        }
        Require(admitted==64,"Opaque phase fixture eligible startup count changed");
    }
    Require(!std::filesystem::exists(original.capture/"gpu-history.json"),
        "Opaque phase fixture cannot combine zero seed with captured GPU history");
    // Same full replay zero seed, after all unexecuted-Scene startup admissions.
    const std::vector<uint8_t> seed(kBytes,0);scene.InitializeEdram(seed);
    return {{"kind","explicit_zero_seed_without_captured_edram"},
        {"bytes",seed.size()},{"initial_byte",0},{"sha256",ContentHash(seed)}};
}
struct Result {std::vector<uint8_t> edram;Json witness;};
enum class Arm {Before,First,Second,Pair,Positive,Replace,Missing};
const char* Name(Arm a) {
    switch(a) {case Arm::Before:return "ordinary-before";case Arm::First:return "ordinary-first";
        case Arm::Second:return "ordinary-second";case Arm::Pair:return "ordinary-pair";case Arm::Positive:return "phase-positive";
        case Arm::Replace:return "phase-replaced-owner";case Arm::Missing:return "phase-missing-pso";}
    throw std::logic_error("Opaque owner fixture arm");
}
Result Run(const SceneExecutorConfig& original,std::span<const LiveOperation* const> ops,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,const NativeFsiPipelineRecipe& recipe,Arm arm) {
    const bool phase=arm==Arm::Positive||arm==Arm::Replace||arm==Arm::Missing;
    auto config=original;config.present={};config.retained_opaque_phase=phase;
    if(!phase) {config.share_retained_opaque_phase_scratch=false;config.queue_retained_exports_for_draws=false;config.poll_retained_opaque_phase_pressure=false;config.share_retained_opaque_phase_attachments=false;config.retained_opaque_phase_render_pass_groups=false;}
    config.test_retained_resource_pool_diagnostics=true;
    // This separate fixture uses actual unqueried draws. The full pair retains
    // its independent query qualification; synthetic wrappers cannot span our hook.
    config.diagnostic_readonly_query_groups=false;
    config.output=original.output/Name(arm);std::filesystem::create_directories(config.output);
    const auto output=config.output;Result out;SceneExecutor scene(std::move(config));
    out.witness["initial_edram"]=PrewarmAndSeed(scene,original,sources,phase);
    // A controlled donor sequence, deliberately not the continuous capture:
    // preserve the actual prefix289, then execute immutable308 followed by292.
    // The first donor changes depth and color; the second is visible and uses
    // the same actual PSO. Every paired arm omits/reorders the same operations.
    scene.RunLiveCapture(ops,kPrefix);
    CheckPrefixRange(scene.report(),ops,kPrefix);
    if(arm!=Arm::Before&&arm!=Arm::Second)scene.Execute(Draw(ops,kFirst));
    if(phase) {
        scene.FlushPendingDraws(); // Completes actual draw; never materializes its dirty attachment authority.
        const auto dirty=scene.RetainedResourcePoolWitnessForFixture();
        const Json first=scene.report();
        Require(dirty.valid&&dirty.dirty&&dirty.ticket.generation&&dirty.accepted_work&&
            dirty.accepted_work==dirty.completed_work&&Count(first,"selected")==1&&
            Count(first,"restores")==1&&Count(first,"materializations")==0&&Count(first,"current_run")==1,
            "Opaque owner fixture first actual draw did not remain dirty and completed");
        out.witness["first_dirty"]=Observe(dirty);out.witness["first_report"]=first;
        if(arm!=Arm::Positive) {
            const auto before=scene.ObserveAcceptedWork();
            const auto changed=scene.MutateRetainedOpaquePhaseOwnerForFixture(recipe,arm==Arm::Missing);
            const auto after=scene.ObserveAcceptedWork();const auto held=scene.RetainedResourcePoolWitnessForFixture();
            Require(before.available&&after.available&&before.generation==after.generation&&
                before.accepted==after.accepted&&before.completed==after.completed&&before.draws==after.draws&&
                before.transfers==after.transfers&&held.valid&&held.dirty&&held.ticket.generation==dirty.ticket.generation&&held.ticket.color==dirty.ticket.color&&held.ticket.depth==dirty.ticket.depth&&
                held.color==dirty.color&&held.depth==dirty.depth&&held.framebuffer==dirty.framebuffer,
                "Opaque owner fixture changed GPU progress, attachment owners or physical authority");
            Require(changed.certified_bytes&&changed.dirty&&changed.old_shader&&changed.old_pipeline&&
                changed.certified_vertex&&changed.certified_pixel==changed.old_shader&&
                changed.accepted_work==dirty.accepted_work&&changed.completed_work==dirty.completed_work&&
                changed.generation==dirty.ticket.generation&&changed.color_version==dirty.ticket.color&&
                changed.depth_version==dirty.ticket.depth,"Opaque owner fixture actual owner witness incomplete");
            if(arm==Arm::Replace)Require(!changed.removed_pipeline&&changed.distinct_vulkan_modules&&
                changed.old_shader!=changed.new_shader&&changed.pipeline_certificate_unchanged,
                "Opaque owner replacement did not preserve a real stale PSO creation identity");
            else Require(changed.removed_pipeline&&changed.old_shader==changed.new_shader,
                "Opaque owner missing-PSO control changed shader identity");
            out.witness["owner_mutation"]=Observe(changed);out.witness["dirty_after_mutation"]=Observe(held);
        }
    }
    if(arm!=Arm::Before&&arm!=Arm::First) {
        // Execute the existing immutable second draw directly. Calling Run again
        // would reinitialize reference history and invalidate this ownership test.
        scene.Execute(Draw(ops,kSecond));
        const Json second=scene.report();out.witness["after_second_report"]=second;
        if(phase) {
            const auto w=scene.RetainedResourcePoolWitnessForFixture();out.witness["after_second_authority"]=Observe(w);
            if(arm==Arm::Positive)Require(Count(second,"selected")==2&&Count(second,"reuses")==1&&
                Count(second,"restores")==1&&Count(second,"materializations")==0&&w.valid&&w.dirty,
                "Opaque owner unchanged positive did not reuse exact dirty authority");
            else Require(Count(second,"selected")==1&&Count(second,"reuses")==0&&
                Count(second,"restores")==1&&Count(second,"materializations")==1&&
                Count(second,"residency_refusals")==1&&!w.valid,
                "Opaque owner control did not export first draw and refuse stale/missing phase PSO");
        }
    }
    out.edram=scene.ReadFullEdram();Require(out.edram.size()==kBytes&&!scene.HasPendingGpuWork(),
        "Opaque owner strong read extent/completion mismatch");
    const Json final=scene.report();out.witness["final_report"]=final;
    if(phase)Require(Count(final,"materializations")==1&&Count(final,"restores")==1&&
        Count(final,"queried_selected")==0&&Count(final,"prewarm_refusals")==0,
        "Opaque owner final phase conservation mismatch");
    out.witness["edram_sha256"]=ContentHash(out.edram);out.witness["full_edram_bytes"]=out.edram.size();
    Write(output/"owner-fixture-edram.bin",out.edram);
    return out;
}
uint64_t DifferentWords(std::span<const uint8_t> a,std::span<const uint8_t> b,size_t begin,size_t end) {
    Require(a.size()==kBytes&&b.size()==kBytes&&begin<end&&end<=kBytes&&begin%4==0&&end%4==0,
        "Opaque owner active bank bounds");uint64_t changed=0;
    for(size_t i=begin;i<end;i+=4)if(!std::equal(a.begin()+i,a.begin()+i+4,b.begin()+i))++changed;
    return changed;
}
std::pair<uint64_t,uint64_t> DepthChanges(std::span<const uint8_t> a,std::span<const uint8_t> b) {
    Require(a.size()==kBytes&&b.size()==kBytes,"Opaque phase depth witness extent");
    const auto word=[](std::span<const uint8_t> bytes,size_t i) {
        return uint32_t(bytes[i])|uint32_t(bytes[i+1])<<8|uint32_t(bytes[i+2])<<16|uint32_t(bytes[i+3])<<24;
    };
    uint64_t increased=0,decreased=0;
    for(size_t i=0;i<720u*5120;i+=4) {
        const auto before=word(a,i)>>8,after=word(b,i)>>8;
        increased+=after>before;decreased+=after<before;
    }
    return {increased,decreased};
}
Result Prefix(const SceneExecutorConfig& original,std::span<const LiveOperation* const> ops,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool phase,uint32_t through) {
    const uint64_t selected=through==289?0:through==487?197:through==1535?203:through==1736?403:UINT64_MAX;
    const uint64_t generations=through==289?0:through==487?2:through==1535?5:7;
    Require(selected!=UINT64_MAX,"Opaque phase prefix endpoint is not one of the four authenticated boundaries");
    auto config=original;config.present={};config.retained_opaque_phase=phase;
    if(!phase) {config.share_retained_opaque_phase_scratch=false;config.queue_retained_exports_for_draws=false;config.poll_retained_opaque_phase_pressure=false;config.share_retained_opaque_phase_attachments=false;config.retained_opaque_phase_render_pass_groups=false;}
    config.test_retained_resource_pool_diagnostics=true;config.diagnostic_readonly_query_groups=false;
    config.output=original.output/("prefix-"+std::to_string(through)+(phase?"-on":"-off"));
    const auto output=config.output;std::filesystem::create_directories(output);
    SceneExecutor scene(std::move(config));Result out;
    out.witness["initial_edram"]=PrewarmAndSeed(scene,original,sources,phase);
    scene.RunLiveCapture(ops,through); // One authentic prefix; no history reinitialization.
    const Json before_read=scene.report();out.witness["before_strong_read"]=before_read;
    CheckPrefixRange(before_read,ops,through);
    if(phase) {
        Require(Count(before_read,"candidates")==selected&&Count(before_read,"selected")==selected&&
            Count(before_read,"restores")==generations&&Count(before_read,"queried_selected")==0&&
            Count(before_read,"residency_refusals")==0&&Count(before_read,"prewarm_refusals")==0&&
            Count(before_read,"resource_refusals")==0,"Opaque phase prefix incomplete actual coverage");
        const auto dirty=scene.RetainedResourcePoolWitnessForFixture();
        const bool should_be_dirty=through==487||through==1736;
        Require(dirty.valid==should_be_dirty&&dirty.dirty==should_be_dirty,
            "Opaque phase endpoint lost actual dirty authority before the strong oracle read");
        out.witness["before_read_authority"]=Observe(dirty);
    }
    out.edram=scene.ReadFullEdram();Require(out.edram.size()==kBytes&&!scene.HasPendingGpuWork(),
        "Opaque phase prefix strong read extent/completion mismatch");
    const Json after=scene.report();out.witness["final_report"]=after;
    if(phase)Require(Count(after,"materializations")==generations&&Count(after,"current_run")==0&&
        Count(after,"reuses")==selected-generations,"Opaque phase prefix generation/export conservation");
    out.witness["full_edram_bytes"]=out.edram.size();out.witness["edram_sha256"]=ContentHash(out.edram);
    Write(output/"prefix-edram.bin",out.edram);return out;
}
}
void RetainedOpaquePhaseOwnerFixture(const SceneExecutorConfig& original,
        std::span<const LiveOperation* const> operations,std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool seed_full_edram) {
    CheckSetup(original,operations,sources,seed_full_edram);
    const NativeFsiPipelineRecipe recipe{std::string(kVertex),std::string(kPixel),kRaster,
        int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
    auto before=Run(original,operations,sources,recipe,Arm::Before);
    auto first=Run(original,operations,sources,recipe,Arm::First);
    // The fixed primary FP24 bank occupies tiles0..719; packed color starts tile720.
    // Nonzero separate color+depth changes avoid an all-zero/vacuous draw witness.
    const auto [depth,depth_decreased]=DepthChanges(before.edram,first.edram);
    const auto color=DifferentWords(before.edram,first.edram,720u*5120,1440u*5120);
    Require(depth&&color&&!depth_decreased,"Opaque owner first draw did not nonvacuously advance depth and alter color");
    auto second_only=Run(original,operations,sources,recipe,Arm::Second);
    auto baseline=Run(original,operations,sources,recipe,Arm::Pair);
    const auto [pair_depth,pair_depth_decreased]=DepthChanges(second_only.edram,baseline.edram);
    const auto pair_color=DifferentWords(second_only.edram,baseline.edram,720u*5120,1440u*5120);
    Require(pair_depth&&pair_color&&!pair_depth_decreased,
        "Opaque owner second draw masks the first donor's depth or color writes");
    const auto second_color=DifferentWords(first.edram,baseline.edram,720u*5120,1440u*5120);
    Require(second_color,"Opaque owner second donor does not visibly alter color");
    uint64_t preserved_first_words=0;
    for(size_t i=0;i<kBytes;i+=4)if(!std::equal(before.edram.begin()+i,before.edram.begin()+i+4,first.edram.begin()+i)) {
        Require(std::equal(first.edram.begin()+i,first.edram.begin()+i+4,baseline.edram.begin()+i),
            "Opaque owner second donor overwrites the first write witness");
        ++preserved_first_words;
    }
    Require(preserved_first_words,"Opaque owner first donor has no preserved writes");
    Json result={{"schema",1},{"events",Json::array({kFirst,kSecond})},{"frame",kFrame},
        {"controlled_sequence",{{"prefix_through",kPrefix},{"donor_events",Json::array({kFirst,kSecond})},
            {"reordered",true},{"continuous_capture",false},{"skipped_before_first",Json::array({290,307})},
            {"second_replayed_from_skipped_range",kSecond}}},
        {"first_draw_changed_depth_words",depth},{"first_draw_changed_color_words",color},
        {"first_draw_words_preserved_after_second",preserved_first_words},{"second_draw_changed_color_words",second_color},
        {"pair_vs_second_only_depth_words_increased",pair_depth},{"pair_vs_second_only_depth_words_decreased",pair_depth_decreased},
        {"pair_vs_second_only_color_words_changed",pair_color},
        {"ordinary_before",before.witness},{"ordinary_first",first.witness},{"ordinary_pair",baseline.witness},
        {"ordinary_second",second_only.witness},
        {"scope","Controlled actual prefix289 then immutable donor308 followed by292, deliberately reordered with intervening operations omitted identically in each arm. Whole ordered capture and continuous prefix parity are separate proofs. Real module/PSO mutations; no runtime configuration or performance claim"}};
    for(const auto arm:{Arm::Positive,Arm::Replace,Arm::Missing}) {
        auto candidate=Run(original,operations,sources,recipe,arm);
        Require(candidate.edram==baseline.edram,"Opaque owner full10MiB differs from same two-draw ordinary Native");
        candidate.witness["exact_full_edram"]=true;result[Name(arm)]=std::move(candidate.witness);
    }
    result["passed"]=true;std::ofstream f(original.output/"retained-opaque-phase-owner-fixture.json");
    f<<result.dump(2)<<'\n';Require(bool(f),"Opaque owner fixture summary write failed");
}
void RetainedOpaquePhasePrefixFixture(const SceneExecutorConfig& original,
        std::span<const LiveOperation* const> operations,std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool seed_full_edram) {
    CheckSetup(original,operations,sources,seed_full_edram);
    Json result={{"schema",1},{"scope","Actual OFF/ON full10MiB phase prefixes before later clears; no ordinary main post-run coverage assertions or performance claim"}};
    std::array<std::array<Result,2>,4> endpoints;
    constexpr std::array<uint32_t,4> events{289,487,1535,1736};
    for(size_t i=0;i<events.size();++i) {
        for(unsigned arm=0;arm<2;++arm)endpoints[i][arm]=Prefix(original,operations,sources,bool(arm),events[i]);
        Require(endpoints[i][0].edram==endpoints[i][1].edram,
            "Opaque phase endpoint full10MiB differs from ordinary Native before later clear");
        result["endpoints"].push_back({{"event",events[i]},{"exact",true},
            {"ordinary",endpoints[i][0].witness},{"phase",endpoints[i][1].witness}});
    }
    for(size_t phase=0;phase<2;++phase)for(size_t arm=0;arm<2;++arm) {
        const auto& before=endpoints[phase*2][arm].edram;const auto& after=endpoints[phase*2+1][arm].edram;
        const auto [increased,decreased]=DepthChanges(before,after);
        const auto colors=DifferentWords(before,after,720u*5120,1440u*5120);
        Require(increased&&!decreased&&colors,"Opaque phase before-clear witness is vacuous or decreases depth");
        result["nonvacuous_phase_witnesses"].push_back({{"phase",phase+1},{"candidate",bool(arm)},
            {"before_event",events[phase*2]},{"after_event",events[phase*2+1]},
            {"depth_words_increased",increased},{"depth_words_decreased",decreased},{"color_words_changed",colors}});
    }
    result["passed"]=true;std::ofstream file(original.output/"retained-opaque-phase-prefix-fixture.json");
    file<<result.dump(2)<<'\n';Require(bool(file),"Opaque phase prefix summary write failed");
}
void RetainedOpaquePhaseQueuedExportFixture(const SceneExecutorConfig& original,
        std::span<const LiveOperation* const> operations,std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool seed_full_edram) {
    CheckSetup(original,operations,sources,seed_full_edram);
    Require(original.queue_retained_exports_for_draws&&original.defer_native_queries&&
        !original.retire_completed_query_prefixes&&!original.reuse_retained_opaque_resources,
        "Queued phase fixture requires explicit queued exports and all-work query retirement without resource pooling");
    const TypedLiveDraw* consumer=nullptr;
    for(const auto* op:operations)if(op->Event()==310) {
        Require(!consumer&&op->Draw(),"Queued phase consumer missing or duplicated");consumer=op->Draw();
    }
    Require(consumer&&consumer->frame==kFrame&&consumer->vertex_shader&&consumer->pixel_shader&&
        consumer->vertex_shader->hash.View()==kVertex&&
        consumer->pixel_shader->hash.View()==kRetainedOpaquePhaseExcludedPixel,
        "Queued phase fixture requires the exact actual excluded consumer310");
    const auto ordinary=[&](uint32_t through) {
        auto config=original;config.present={};config.retained_opaque_phase=false;
        config.share_retained_opaque_phase_scratch=false;
        config.poll_retained_opaque_phase_pressure=false;config.share_retained_opaque_phase_attachments=false;config.retained_opaque_phase_render_pass_groups=false;
        config.queue_retained_exports_for_draws=false;config.diagnostic_readonly_query_groups=false;
        config.output=original.output/("queued-phase-ordinary-"+std::to_string(through));
        const auto output=config.output;std::filesystem::create_directories(output);
        SceneExecutor scene(std::move(config));Result out;
        out.witness["initial_edram"]=PrewarmAndSeed(scene,original,sources,false);
        scene.RunLiveCapture(operations,through);
        const auto last=through==289?287:through;
        Require(scene.report().at("requested_range_complete").get<bool>()&&scene.report().at("last_accepted_event")==last,
            "Queued phase ordinary prefix incomplete");
        out.edram=scene.ReadFullEdram();Require(out.edram.size()==kBytes&&!scene.HasPendingGpuWork(),
            "Queued phase ordinary strong read incomplete");
        out.witness["final_report"]=scene.report();out.witness["full_edram_bytes"]=out.edram.size();
        out.witness["edram_sha256"]=ContentHash(out.edram);Write(output/"queued-phase-edram.bin",out.edram);return out;
    };
    const auto before=ordinary(289),producer=ordinary(309),reference=ordinary(310);
    const auto [depth,decreased]=DepthChanges(before.edram,producer.edram);
    const auto color=DifferentWords(before.edram,producer.edram,720u*5120,1440u*5120);
    Require(depth&&color&&!decreased,"Queued phase actual producer prefix has no positive depth/color witness");
    Json result={{"schema",1},{"producer_prefix",309},{"consumer_event",310},
        {"producer_depth_words_increased",depth},{"producer_depth_words_decreased",decreased},
        {"producer_color_words_changed",color},{"ordinary_before",before.witness},
        {"ordinary_producer",producer.witness},{"ordinary_reference",reference.witness},
        {"scope","Actual continuous prefix309 then immutable excluded draw310; symmetric zero10MiB seed, queued versus strong phase export, real empty query followed by strong CPU EDRAM read. No timing claim."}};
    for(bool queue:{false,true}) {
        auto config=original;config.present={};config.queue_retained_exports_for_draws=queue;
        config.diagnostic_readonly_query_groups=false;config.test_retained_resource_pool_diagnostics=true;
        config.output=original.output/(queue?"queued-phase-on":"queued-phase-off");
        const auto output=config.output;std::filesystem::create_directories(output);
        SceneExecutor scene(std::move(config));Json witness;
        witness["initial_edram"]=PrewarmAndSeed(scene,original,sources,true);
        scene.RunLiveCapturePartialForRetainedFixture(operations,309);
        const Json prefix=scene.report();const auto dirty=scene.RetainedResourcePoolWitnessForFixture();
        Require(prefix.at("requested_range_complete").get<bool>()&&prefix.at("last_accepted_event")==309&&
            Count(prefix,"selected")==20&&Count(prefix,"restores")==1&&Count(prefix,"materializations")==0&&
            Count(prefix,"current_run")==20&&dirty.valid&&dirty.dirty,
            "Queued phase producer lost its exact dirty generation");
        witness["producer_report"]=prefix;witness["producer_authority"]=Observe(dirty);
        scene.Execute(*consumer);
        const Json after=scene.report();witness["after_consumer_report"]=after;
        Require(Count(after,"selected")==20&&Count(after,"materializations")==1&&
            Count(after,"queued_draw_materializations")==uint64_t(queue)&&
            Count(after,"queued_draw_pending")==uint64_t(queue)&&Count(after,"resource_refusals")==0&&
            Count(after,"residency_refusals")==0&&!scene.RetainedResourcePoolWitnessForFixture().valid,
            "Queued phase consumer lost exact export/frontier coverage");
        const auto query=[&](bool end) {
            return Json{{"type",end?"query_end":"query_begin"},{"frame",kFrame},{"event",end?312:311},
                {"generation",6101},{"query_generation",6101},{"query_handle",0x7734},
                {"report_address",end?0x1fffff80:0x1fffffa0}};
        };
        scene.Execute(query(false));scene.Execute(query(true));
        Require(scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==1,
            "Queued phase query did not retain its outstanding original work frontier");
        bool premature_refused=false;
        try {(void)scene.TakeCompletedQueries();}
        catch(const std::runtime_error& error) {
            if(std::string_view(error.what())!="Native query consumption before all GPU work completed")throw;
            premature_refused=true;
        }
        Require(premature_refused&&scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==1,
            "Queued phase premature query consumption was not refused with its ticket retained");
        witness["premature_query_consumption_refused"]=premature_refused;
        witness["deferred_queries_after_refusal"]=scene.DeferredQueryCount();
        witness["pending_query_report"]=scene.report();
        const auto full=scene.ReadFullEdram();
        Require(full.size()==kBytes&&full==reference.edram&&!scene.HasPendingGpuWork(),
            "Queued phase strong CPU read differs from ordinary full10MiB or left pending work");
        const auto queries=scene.TakeCompletedQueries();
        Require(queries.size()==1&&queries[0].generation==6101&&queries[0].frame==kFrame&&
            queries[0].end_event==312&&queries[0].result.handle==0x7734&&
            queries[0].result.begin_address==0x1fffffa0&&queries[0].result.end_address==0x1fffff80&&
            queries[0].result.draws==0&&queries[0].result.samples==0,
            "Queued phase strong CPU read lost exact empty-query completion");
        witness["completed_query"]={{"generation",6101},{"frame",kFrame},{"event",312},
            {"handle",0x7734},{"begin_address",0x1fffffa0},{"end_address",0x1fffff80},{"draws",0},{"samples",0}};
        witness["final_report"]=scene.report();witness["exact_full_edram"]=true;
        witness["full_edram_bytes"]=full.size();witness["edram_sha256"]=ContentHash(full);
        Write(output/"queued-phase-edram.bin",full);result[queue?"queued":"strong"]=std::move(witness);
    }
    result["passed"]=true;std::ofstream file(original.output/"retained-opaque-phase-queued-export-fixture.json");
    file<<result.dump(2)<<'\n';Require(bool(file),"Queued phase fixture summary write failed");
}
}
