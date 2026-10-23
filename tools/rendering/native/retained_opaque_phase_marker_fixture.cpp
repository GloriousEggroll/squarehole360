#include "retained_opaque_phase_marker_fixture.h"
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
constexpr uint32_t kBytes=10u*1024*1024,kFrame=17672,kPrefix=289,kFirst=308,kSecond=292;
constexpr std::string_view kVertex="75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802";
constexpr std::string_view kPixel="55c79ab30e62247886a8c74dc440f04695ffd0f6f88842ffa1aea0b6829ee6c6";
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
uint64_t Count(const Json& r,const char* k) {return r.at("retained_opaque_phase").at(k).get<uint64_t>();}
void Save(const std::filesystem::path& path,std::span<const uint8_t> bytes) {
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    Require(bool(f),"Phase marker fixture artifact write failed");
}
Json Observe(const NativeRetainedOpaqueResourceWitness& w) {
    return {{"valid",w.valid},{"dirty",w.dirty},{"generation",w.ticket.generation},
        {"color_version",w.ticket.color},{"depth_version",w.ticket.depth},
        {"color",w.color},{"depth",w.depth},{"framebuffer",w.framebuffer},
        {"accepted_work",w.accepted_work},{"completed_work",w.completed_work},
        {"shared_scratch",w.shared_scratch},{"scratch",w.scratch},{"descriptors",w.descriptors},
        {"canonical",w.canonical},{"scratch_bytes",w.scratch_bytes}};
}
Json Observe(const SceneExecutor::MarkerQueryReadiness& q) {
    return {{"available",q.available},{"active",q.active},{"closed",q.closed},
        {"ready",q.ready},{"unknown",q.unknown}};
}
bool ZeroQueries(const SceneExecutor::MarkerQueryReadiness& q) {
    return q.available&&!q.active&&!q.closed&&!q.unknown;
}
const TypedLiveDraw& Draw(std::span<const LiveOperation* const> ops,uint32_t event) {
    const TypedLiveDraw* result=nullptr;
    for(const auto* op:ops) {
        Require(op,"Phase marker null operation");
        Require(op->Kind()!="query_begin"&&op->Kind()!="query_end","Phase marker requires original unqueried capture");
        if(op->Event()!=event)continue;
        Require(!result&&op->Draw(),"Phase marker duplicated/missing donor");result=op->Draw();
    }
    Require(result&&result->frame==kFrame&&result->vertex_shader&&result->pixel_shader&&
        result->vertex_shader->hash.View()==kVertex&&result->pixel_shader->hash.View()==kPixel&&
        result->function==DrawFunction::DrawIndexedPrimitive&&result->args[1]==4&&!result->rectangle_list,
        "Phase marker exact immutable donor changed");
    return *result;
}
void Setup(SceneExecutor& scene,const SceneExecutorConfig& original,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool phase) {
    std::set<NativeFsiPipelineRecipe> unique;size_t selected=0;
    for(const auto& source:sources) {
        Require(source.recipe.pixel&&NativeRetainedOpaquePhaseCatalogRecipe(source.recipe.vertex,*source.recipe.pixel,source.recipe.raster)&&
            source.recipe.color_format==int32_t(plume::RenderFormat::R8G8B8A8_UNORM)&&source.recipe.depth_format==-1&&
            !source.vertex.empty()&&!source.pixel.empty()&&unique.insert(source.recipe).second,
            "Phase marker requires exact65 unique canonical recipes");
        scene.PrewarmFsiPipeline(source.recipe,source.vertex,source.pixel);
    }
    if(phase)for(const auto& source:sources) {
        if(!NativeRetainedOpaquePhaseRecipe(source.recipe.vertex,*source.recipe.pixel,source.recipe.raster))continue;
        Require(scene.PrewarmRetainedOpaquePhasePipeline(source.recipe,source.vertex,source.pixel),
            "Phase marker optional recipe did not prepare");++selected;
    }
    Require(unique.size()==65&&(!phase||selected==64)&&!std::filesystem::exists(original.capture/"gpu-history.json"),
        "Phase marker seed or original recipe cohort changed");
    scene.InitializeEdram(std::vector<uint8_t>(kBytes));
}
enum class Arm {Ordinary,NoProgress,Submit};
const char* Name(Arm a) {
    switch(a) {case Arm::Ordinary:return "ordinary-pair-query";
        case Arm::NoProgress:return "phase-no-progress";case Arm::Submit:return "phase-submit-progress";}
    throw std::logic_error("Phase marker unknown arm");
}
struct Result {std::vector<uint8_t> memory,pre_query,before,first;Json witness;};
Result Run(const SceneExecutorConfig& original,std::span<const LiveOperation* const> ops,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,Arm arm) {
    const bool phase=arm==Arm::NoProgress||arm==Arm::Submit;
    auto config=original;config.present={};config.retained_opaque_phase=phase;
    // Queue export has no owner in ordinary Native; it is unchanged in phase arms.
    if(!phase) {config.queue_retained_exports_for_draws=false;config.share_retained_opaque_phase_scratch=false;config.poll_retained_opaque_phase_pressure=false;config.share_retained_opaque_phase_attachments=false;config.retained_opaque_phase_render_pass_groups=false;}
    config.test_retained_resource_pool_diagnostics=true;config.diagnostic_readonly_query_groups=false;
    config.output=original.output/Name(arm);const auto output=config.output;
    std::filesystem::create_directories(output);SceneExecutor scene(std::move(config));Result out;
    Setup(scene,original,sources,phase);scene.RunLiveCapture(ops,kPrefix);
    const auto prefix=scene.report();
    Require(prefix.at("requested_range_complete").get<bool>()&&prefix.at("last_accepted_event")==287,
        "Phase marker original prefix incomplete");
    if(!phase)out.before=scene.ReadFullEdram();
    scene.Execute(Draw(ops,kFirst));
    if(!phase)out.first=scene.ReadFullEdram(); // Ordinary oracle only; candidate stays uninterrupted.
    if(phase) {
        const auto before=scene.RetainedResourcePoolWitnessForFixture();const auto stats=scene.report();
        Require(before.valid&&before.dirty&&before.ticket.generation&&before.accepted_work&&
            Count(stats,"selected")==1&&Count(stats,"restores")==1&&Count(stats,"materializations")==0,
            "Phase marker seam lacks real dirty first draw");
        out.witness["before_progress"]=Observe(before);
        if(arm==Arm::Submit)scene.SubmitGpuWork();
        const auto after=scene.RetainedResourcePoolWitnessForFixture();const auto work=scene.ObserveAcceptedWork();
        const auto ready=scene.ObserveMarkerQueryReadiness();const auto after_stats=scene.report();
        Require(after.valid&&after.dirty&&after.ticket.generation==before.ticket.generation&&
            after.ticket.color==before.ticket.color&&after.ticket.depth==before.ticket.depth&&
            after.color==before.color&&after.depth==before.depth&&after.framebuffer==before.framebuffer&&
            after.accepted_work==before.accepted_work&&after.completed_work>=before.completed_work&&
            Count(after_stats,"selected")==1&&Count(after_stats,"restores")==1&&Count(after_stats,"materializations")==0,
            "Phase marker progress changed retained ownership or packed representation");
        if(arm==Arm::Submit)Require(work.available&&ZeroQueries(ready)&&!scene.ActiveQueryMetadata()&&
            !scene.DeferredQueryCount(),"Phase marker submitted prefix lacks zero-query readiness");
        out.witness["after_progress"]=Observe(after);out.witness["readiness_after_progress"]=Observe(ready);
        out.witness["explicit_submit_calls"]=arm==Arm::Submit?1:0;
    }
    scene.Execute(Draw(ops,kSecond));
    if(phase) {
        const auto second=scene.RetainedResourcePoolWitnessForFixture();const auto stats=scene.report();
        Require(second.valid&&second.dirty&&second.ticket.generation==out.witness["before_progress"]["generation"].get<uint64_t>()&&
            second.color==out.witness["before_progress"]["color"].get<uintptr_t>()&&
            second.depth==out.witness["before_progress"]["depth"].get<uintptr_t>()&&
            Count(stats,"selected")==2&&Count(stats,"reuses")==1&&Count(stats,"restores")==1&&
            Count(stats,"materializations")==0,"Phase marker second donor failed same-generation continuation");
        out.witness["after_second"]=Observe(second);
    }
    // Preserve the completed pair before replaying any donor under a query.
    // The later query draw cannot hide a lost first draw in this full oracle.
    out.pre_query=scene.ReadFullEdram();
    Require(out.pre_query.size()==kBytes&&!scene.HasPendingGpuWork(),"Phase marker pre-query read incomplete");
    out.witness["pre_query_sha256"]=ContentHash(out.pre_query);
    Save(output/"marker-pre-query-edram.bin",out.pre_query);
    {
        // Subsequent query readiness is a separate guard/control check. The
        // pre-query strong read has already materialized any dirty authority.
        scene.Execute(Json{{"type","query_begin"},{"frame",kFrame},{"event",4000},
            {"query_generation",47001},{"query_handle",0x7100},{"report_address",0x1fffffe0}});
        const auto active=scene.ObserveMarkerQueryReadiness();
        Require(scene.ActiveQueryMetadata().has_value()&&(!active.available||active.active),
            "Phase marker active query not observable as a refusal");
        out.witness["active_query_readiness"]=Observe(active);
        scene.Execute(Draw(ops,kFirst)); // Equal-depth real geometry; ordinary Native under query.
        scene.Execute(Json{{"type","query_end"},{"frame",kFrame},{"event",4001},
            {"query_generation",47001},{"query_handle",0x7100},{"report_address",0x1fffffc0}});
        const auto closed=scene.ObserveMarkerQueryReadiness();
        Require(scene.DeferredQueryCount()==1&&(!closed.available||closed.closed==1),
            "Phase marker closed query disappeared before original publication");
        out.witness["closed_query_readiness"]=Observe(closed);
        scene.FlushGpuWork();auto queries=scene.TakeCompletedQueries();
        Require(queries.size()==1&&queries[0].generation==47001&&queries[0].frame==kFrame&&
            queries[0].result.handle==0x7100&&queries[0].result.begin_address==0x1fffffe0&&
            queries[0].result.end_address==0x1fffffc0&&queries[0].result.draws==1&&queries[0].result.samples>0,
            "Phase marker real query identity/positive samples changed");
        out.witness["query"]={{"generation",queries[0].generation},{"frame",queries[0].frame},
            {"handle",queries[0].result.handle},{"begin",queries[0].result.begin_address},
            {"end",queries[0].result.end_address},{"draws",queries[0].result.draws},{"samples",queries[0].result.samples}};
        Require(ZeroQueries(scene.ObserveMarkerQueryReadiness())&&!scene.DeferredQueryCount(),
            "Phase marker real completed query did not retire");
    }
    out.memory=scene.ReadFullEdram();Require(out.memory.size()==kBytes&&!scene.HasPendingGpuWork(),
        "Phase marker full-memory read incomplete");
    if(phase) {
        const auto final=scene.report();Require(Count(final,"selected")==2&&Count(final,"queried_selected")==0&&
            Count(final,"restores")==1&&Count(final,"materializations")==1&&Count(final,"prewarm_refusals")==0,
            "Phase marker final generation/query conservation failed");
        out.witness["phase_final"]=final.at("retained_opaque_phase");
    }
    out.witness["edram_sha256"]=ContentHash(out.memory);out.witness["full_edram_bytes"]=kBytes;
    Save(output/"marker-edram.bin",out.memory);return out;
}
uint32_t Word(const std::vector<uint8_t>& bytes,size_t i) {
    return uint32_t(bytes[i])|uint32_t(bytes[i+1])<<8|uint32_t(bytes[i+2])<<16|uint32_t(bytes[i+3])<<24;
}
}
void RetainedOpaquePhaseMarkerFixture(const SceneExecutorConfig& original,
        std::span<const LiveOperation* const> operations,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool seed_full_edram) {
    Require(seed_full_edram&&original.retained_opaque_phase&&original.retained_opaque_phase_programs&&
        original.native_fsi&&original.async_gpu_submissions&&original.unified_gpu_queue&&original.defer_native_queries&&
        !original.retained_opaque_writer&&!original.retained_opaque_catalog&&sources.size()==65&&operations.size()<=65536,
        "Phase marker prerequisites not explicit");
    (void)Draw(operations,kFirst);(void)Draw(operations,kSecond);
    auto baseline=Run(original,operations,sources,Arm::Ordinary);
    Require(baseline.before.size()==kBytes&&baseline.first.size()==kBytes,"Phase marker ordinary witness extent");
    uint64_t first_depth=0,first_color=0,second_color=0,preserved=0;
    for(size_t i=0;i<kBytes;i+=4) {
        const auto b=Word(baseline.before,i),f=Word(baseline.first,i),p=Word(baseline.pre_query,i);
        if(i<720u*5120) {
            Require((f>>8)>=(b>>8),"Phase marker witness decreases depth");
            first_depth+=(f>>8)>(b>>8);
        }else if(i<1440u*5120) {first_color+=f!=b;second_color+=p!=f;}
        if(f!=b){Require(p==f,"Phase marker second donor masks first witness");++preserved;}
    }
    Require(first_depth&&first_color&&second_color&&preserved,
        "Phase marker controlled donor sequence is vacuous");
    Json summary={{"schema",1},{"controlled_sequence",{{"prefix_through",kPrefix},{"last_prefix_event",287},
        {"donors",Json::array({kFirst,kSecond})},{"reordered",true},{"continuous_capture",false}}},
        {"first_depth_increased",first_depth},{"first_color_changed",first_color},
        {"second_color_changed",second_color},{"first_words_preserved",preserved},
        {"ordinary_before_sha256",ContentHash(baseline.before)},{"ordinary_first_sha256",ContentHash(baseline.first)},
        {"ordinary-pair-query",baseline.witness},
        {"scope","Real dirty retained Scene SubmitGpuWork seam and immutable reordered donors; full10MiB oracle before any query redraw. Later query readiness/samples are separate after a strong read; no dirty query-export claim. No guest marker stores or Runtime query/target guard bypass; SDK decoder/poison coverage is separate. No performance claim."}};
    Save(original.output/"ordinary-before.bin",baseline.before);
    Save(original.output/"ordinary-first.bin",baseline.first);
    for(const auto arm:{Arm::NoProgress,Arm::Submit}) {
        auto value=Run(original,operations,sources,arm);
        Require(value.pre_query==baseline.pre_query,
            "Phase marker pre-query full10MiB differs from ordinary Native");
        Require(value.memory==baseline.memory&&value.witness.at("query")==baseline.witness.at("query"),
            "Phase marker post-query full10MiB or real query differs from ordinary Native");
        value.witness["exact_full_edram_and_query"]=true;summary[Name(arm)]=std::move(value.witness);
    }
    summary["passed"]=true;std::ofstream f(original.output/"retained-opaque-phase-marker-fixture.json");
    f<<summary.dump(2)<<'\n';Require(bool(f),"Phase marker summary write failed");
}
}
