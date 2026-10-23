#include "retained_queued_draw_export_fixture.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/live_draw.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/pipeline_prewarm.h"
#include "plume_render_interface.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct FixtureInput {
    std::vector<const LiveOperation*> operations;
    std::shared_ptr<const LiveOperation> changed;
    const TypedLiveDraw* donor=nullptr;
    uint32_t through=0;
    Json metadata;
};
FixtureInput MakeInput(std::span<const LiveOperation* const> operations) {
    Require(operations.size()<=65536,"Queued export operation bound");
    FixtureInput out;out.operations.assign(operations.begin(),operations.end());
    size_t consecutive=0;const NativeRetainedOpaqueCatalogPair* previous=nullptr;
    for(size_t i=0;i<operations.size();++i) {
        const auto* op=operations[i];const auto* draw=op?op->Draw():nullptr;
        const auto* pair=draw&&draw->vertex_shader&&draw->pixel_shader?
            RetainedOpaqueCatalogPair(draw->vertex_shader->hash.View(),draw->pixel_shader->hash.View()):nullptr;
        const bool same=pair&&previous==pair&&i&&operations[i-1]->Frame()==op->Frame();
        consecutive=pair?(same?consecutive+1:1):0;previous=pair;
        if(consecutive!=4)continue;
        Require(op->Event()<UINT32_MAX,"Queued export event bound");
        const auto& owner=draw->lazy_device_state?draw->lazy_device_state->Owner():draw->device_state.owner;
        Require(owner&&owner->size()==LazyDeviceState::kBytes,"Queued export requires full immutable draw state");
        auto bytes=std::vector<uint8_t>(*owner);
        const uint32_t mask=uint32_t(bytes.at(0x28dc))<<24|uint32_t(bytes.at(0x28dd))<<16|
            uint32_t(bytes.at(0x28de))<<8|bytes.at(0x28df);
        Require((mask&15)==15,"Queued export donor must use full color mask");
        bytes[0x28df]=uint8_t((mask&~15u)|7u);
        auto restored=bytes;restored[0x28df]=(*owner)[0x28df];
        Require(restored==*owner&&bytes!=*owner,"Queued export fixture changed unrelated bytes");
        const auto digest=ContentHash(bytes);auto clone=*draw;clone.lazy_device_state.reset();
        clone.device_state={CanonicalDigest(digest),std::make_shared<const std::vector<uint8_t>>(std::move(bytes))};
        out.changed=std::make_shared<const LiveOperation>(std::move(clone));out.operations[i]=out.changed.get();
        out.donor=operations[i-1]->Draw();out.through=uint32_t(operations[i-1]->Event());
        out.metadata={{"synthetic_state_fixture",true},{"event",op->Event()},{"frame",op->Frame()},
            {"offset",0x28dc},{"old_word",mask},{"new_word",(mask&~15u)|7u},
            {"original_state_sha256",ContentHash(*owner)},{"modified_state_sha256",digest},
            {"scope","Only fourth consecutive actual material draw color mask15->7 changes. Prior draws, all geometry/textures/shaders and event/order remain identical in OFF/ON; authenticated captured history is imported."}};
        return out;
    }
    throw std::runtime_error("No four consecutive actual catalogue draws for queued export fixture");
}
void Prewarm(SceneExecutor& scene,std::span<const RetainedOpaqueCatalogFixtureSource> sources,bool candidate) {
    for(bool optional:{false,true}) {
        if(optional&&!candidate)continue;
        for(const auto& source:sources)for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
            const NativeFsiPipelineRecipe recipe{std::string(source.pair.vertex),std::string(source.pair.pixel),
                {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            if(optional)Require(scene.PrewarmRetainedOpaquePipeline(recipe,source.vertex,source.pixel),"Queued export optional startup refused");
            else scene.PrewarmFsiPipeline(recipe,source.vertex,source.pixel);
        }
    }
}
Json Query(uint64_t generation,uint32_t report,uint32_t frame,uint64_t event,bool end) {
    return {{"type",end?"query_end":"query_begin"},{"generation",generation},{"query_generation",generation},
        {"query_handle",0x7733},{"report_address",report},{"frame",frame},{"event",event}};
}
Json Queries(const std::vector<CompletedNativeQuery>& results) {
    Json out=Json::array();for(const auto& q:results)out.push_back({{"generation",q.generation},{"frame",q.frame},
        {"event",q.end_event},{"handle",q.result.handle},{"begin_address",q.result.begin_address},
        {"end_address",q.result.end_address},{"draws",q.result.draws},{"samples",q.result.samples}});return out;
}
struct Result {std::vector<uint8_t> edram,physical;Json queries,witness;};
Result Run(const SceneExecutorConfig& original,const FixtureInput& input,
    std::span<const RetainedOpaqueCatalogFixtureSource> sources,bool queue,bool cpu_read,bool foreign,bool missing,bool owned_tail) {
    auto config=original;config.present={};config.queue_retained_exports_for_draws=queue;
    config.defer_native_queries=true;config.retire_completed_query_prefixes=true;
    config.output=original.output/(std::string("queued-export-")+(queue?"on":"off")+
        (owned_tail?"-owned":foreign?"-foreign":cpu_read?"-cpu":missing?"-missing":"-queries"));
    Result out;const auto frame=input.donor->frame;const uint64_t event=input.changed->Event()+1ull;
    {
        SceneExecutor scene(std::move(config));Prewarm(scene,sources,!missing);
        if(!owned_tail)scene.Execute(Query(7001,0x1fffffa0,frame,0,false));
        scene.RunLiveCapturePartialForRetainedFixture(input.operations,input.through);
        const Json before=scene.report();const auto& before_stats=before.at("retained_opaque_catalog");
        if(!missing)Require(before_stats.at("current_run").get<uint64_t>()>=3&&
            before.at("partial_owned_retained_fixture").at("dirty_retained_authority").get<bool>()&&
            before_stats.at("retained_image_bytes").get<uint64_t>()>0,
            "Controlled donor prefix did not retain an accepted dirty generation");
        else Require(before_stats.at("selected")==0&&before_stats.at("candidates").get<uint64_t>()>0&&
            before_stats.at("residency_refusals").get<uint64_t>()>0,
            "Missing startup fixture did not exercise an actual resident-PSO refusal");
        const auto queued_before=before_stats.at("queued_draw_materializations").get<uint64_t>();
        const auto pending_before=before_stats.at("queued_draw_pending").get<uint64_t>();
        const auto materialized_before=before_stats.at("materializations").get<uint64_t>();
        scene.Execute(*input.changed->Draw());
        const Json after=scene.report();const auto& after_stats=after.at("retained_opaque_catalog");
        Require(after_stats.at("queued_draw_materializations").get<uint64_t>()==queued_before+uint64_t(queue&&!missing),
            "Controlled ordinary draw queued-export delta is not exact");
        Require(after_stats.at("queued_draw_pending").get<uint64_t>()==pending_before+uint64_t(queue&&!missing),
            "Controlled ordinary draw did not cross the exact uncompleted export frontier");
        if(!missing)Require(after_stats.at("materializations").get<uint64_t>()==materialized_before+1&&
            after.at("gpu_work_queue").at("accepted_work").get<uint64_t>()>
            before.at("gpu_work_queue").at("accepted_work").get<uint64_t>(),
            "Controlled ordinary draw did not publish its accepted canonical export");
        out.witness={{"queue",queue},{"cpu_read",cpu_read},{"foreign",foreign},{"missing",missing},
            {"owned_tail",owned_tail},{"donor_prefix",before},{"controlled_draw",after},{"before_completion",after},
            {"pending_before_completion",scene.HasPendingGpuWork()},{"controlled_input",input.metadata}};
        const auto& stats=after_stats;
        if(queue&&!missing)Require(stats.at("queued_draw_materializations").get<uint64_t>()>0&&
            stats.at("queued_draw_pending").get<uint64_t>()>0,"Queued export positive accepted pending coverage missing");
        if(!queue||missing)Require(stats.at("queued_draw_materializations")==0,"Refused/OFF fixture queued an export");
        if(owned_tail)Require(scene.HasPendingGpuWork(),"Owned queued tail lost its pending packet");
        else {
            if(cpu_read) {
                std::ifstream file(original.capture/"gpu-history.json");Json history;file>>history;
                Require(bool(file)&&!history.at("ranges").empty(),"Strong CPU read needs actual imported physical range");
                const auto address=history.at("ranges").front().at("address").get<uint32_t>();
                out.physical=scene.ReadPhysical(address,4);
                Require(!scene.HasPendingGpuWork(),"Public CPU read did not drain queued exports");
                out.witness["after_strong_cpu_read"]=scene.report();
            }
            scene.Execute(Query(7001,0x1fffff80,frame,event,true));
            scene.Execute(Query(7002,0x1fffff60,frame,event+1,false));
            scene.Execute(Query(7002,0x1fffff40,frame,event+2,true));
            if(!cpu_read&&!missing&&queue)Require(scene.TakeCompletedQueries().empty(),
                "Active/empty queries passed an uncompleted queued export frontier");
            out.witness["query_end_frontier"]=scene.report();
            scene.FlushOldestQuery();auto results=scene.TakeCompletedQueries();
            Require(results.size()==2&&results[0].generation==7001&&results[1].generation==7002&&
                results[0].result.draws>0&&results[0].result.samples>0&&results[1].result.draws==0&&results[1].result.samples==0,
                "Queued export exact active/empty hardware query coverage missing");
            out.queries=Queries(results);out.edram=scene.ReadFullEdram();
            Require(out.edram.size()==10u*1024*1024&&!scene.HasPendingGpuWork(),"Full CPU EDRAM read did not fence queued export");
            if(foreign) {
                const auto restores=scene.report().at("retained_opaque_catalog").at("restores").get<uint64_t>();
                scene.InitializeEdram(out.edram);auto follow=*input.donor;follow.event=event+3;scene.Execute(follow);
                Require(scene.report().at("retained_opaque_catalog").at("restores").get<uint64_t>()>restores,
                    "Foreign EDRAM write reused old retained authority");
                out.edram=scene.ReadFullEdram();Require(!scene.HasPendingGpuWork(),"Foreign follow-up did not complete");
            }
            out.witness["completed_queries"]=out.queries;out.witness["final_edram_sha256"]=ContentHash(out.edram);
            out.witness["after_strong_completion"]=scene.report();
        }
        // No explicit drain on the owned-tail case. Scene's actual destructor
        // must retire the queued transfer packet before its own VkDevice dies.
    }
    out.witness["owned_destructor_returned"]=true;return out;
}
}
void RetainedQueuedDrawExportFixture(const SceneExecutorConfig& original,
    std::span<const LiveOperation* const> operations,std::span<const RetainedOpaqueCatalogFixtureSource> sources) {
    Require(original.retained_opaque_catalog&&original.queue_retained_exports_for_draws&&
        original.native_fsi&&original.unified_gpu_queue&&original.async_gpu_submissions&&sources.size()==3,
        "Queued export fixture requires exclusive V2 unified Native setup");
    const auto input=MakeInput(operations);Json cases=Json::array();
    std::ifstream history_file(original.capture/"gpu-history.json");Json history;history_file>>history;
    Require(bool(history_file)&&!history.at("edram").is_null(),"Queued fixture requires authentic captured full EDRAM history");
    const auto& edram_history=history.at("edram");
    Require(edram_history.at("bytes")==10u*1024*1024,"Queued fixture history is not full10MiB");
    for(unsigned mode=0;mode<4;++mode) {
        const bool cpu=mode==1,foreign=mode==2,missing=mode==3;
        auto off=Run(original,input,sources,false,cpu,foreign,missing,false);
        auto on=Run(original,input,sources,true,cpu,foreign,missing,false);
        Require(off.edram==on.edram&&off.physical==on.physical&&off.queries==on.queries,
            "Queued export OFF/ON full10MiB/CPU bytes/real query identity or samples differs");
        cases.push_back({{"mode",mode},{"off",std::move(off.witness)},{"on",std::move(on.witness)},
            {"full10MiB_query_cpu_parity",true}});
    }
    auto tail=Run(original,input,sources,true,false,false,false,true);cases.push_back(std::move(tail.witness));
    Json witness={{"capacity",original.async_submission_capacity},{"cases",std::move(cases)},
        {"captured_edram_history",{{"bytes",edram_history.at("bytes")},{"sha256",edram_history.at("sha256")},
            {"provenance",history.at("provenance")},{"frame",history.at("frame")}}},
        {"scope","Real owned Scene OFF/ON queued export to ordinary masked FSI draw, active+empty queries, strong CPU/foreign memory closure, missing-resident refusal and actual pending owned shutdown. Initial EDRAM is authenticated captured history, not synthetic zero."}};
    std::ofstream file(original.output/"retained-queued-draw-export-fixture.json");file<<witness.dump(2)<<'\n';
    Require(bool(file),"Cannot retain queued export fixture witness");
}
}
