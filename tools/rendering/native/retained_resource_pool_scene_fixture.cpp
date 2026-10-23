#include "retained_resource_pool_scene_fixture.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/live_draw.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/pipeline_prewarm.h"
#include "renderer/native/retained_opaque_writer.h"
#include "plume_render_interface.h"
#include <fstream>
#include <stdexcept>

namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
using Witness=NativeRetainedOpaqueResourceWitness;
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct Input {
    std::span<const LiveOperation* const> operations;
    const TypedLiveDraw* donor=nullptr;
    TypedLiveDraw ordinary;
    uint32_t through=0;
    Json metadata;
};
Input MakeInput(std::span<const LiveOperation* const> operations) {
    Require(operations.size()<=65536,"Resource pool operation bound");
    size_t consecutive=0;const NativeRetainedOpaqueCatalogPair* previous=nullptr;
    for(size_t i=0;i<operations.size();++i) {
        const auto* op=operations[i];const auto* draw=op?op->Draw():nullptr;
        const auto* pair=draw&&draw->vertex_shader&&draw->pixel_shader?
            RetainedOpaqueCatalogPair(draw->vertex_shader->hash.View(),draw->pixel_shader->hash.View()):nullptr;
        const bool same=pair&&previous==pair&&i&&operations[i-1]->Frame()==op->Frame();
        consecutive=pair?(same?consecutive+1:1):0;previous=pair;
        if(consecutive!=3)continue;
        Require(op->Event()<UINT32_MAX-32u,"Resource pool synthetic event bound");
        const auto& owner=draw->lazy_device_state?draw->lazy_device_state->Owner():draw->device_state.owner;
        Require(owner&&owner->size()==LazyDeviceState::kBytes,"Resource pool donor requires immutable full draw state");
        auto bytes=std::vector<uint8_t>(*owner);
        const uint32_t mask=uint32_t(bytes.at(0x28dc))<<24|uint32_t(bytes.at(0x28dd))<<16|
            uint32_t(bytes.at(0x28de))<<8|bytes.at(0x28df);
        Require((mask&15)==15,"Resource pool donor must use full color mask");
        bytes[0x28df]=uint8_t((mask&~15u)|7u);
        auto restored=bytes;restored[0x28df]=(*owner)[0x28df];
        Require(restored==*owner&&bytes!=*owner,"Resource pool fixture altered unrelated state");
        const auto digest=ContentHash(bytes);auto ordinary=*draw;ordinary.lazy_device_state.reset();
        ordinary.device_state={CanonicalDigest(digest),std::make_shared<const std::vector<uint8_t>>(std::move(bytes))};
        return {operations,draw,std::move(ordinary),uint32_t(op->Event()),
            {{"synthetic_controlled_tail",true},{"donor_event",op->Event()},{"frame",op->Frame()},
             {"changed_offset",0x28dc},{"old_word",mask},{"new_word",(mask&~15u)|7u},
             {"original_state_sha256",ContentHash(*owner)},{"masked_state_sha256",digest},
             {"scope","Authenticated captured prefix through three same-family draws, then identical OFF/ON controlled donor repeats with only color mask15->7 changed on ordinary draws. Synthetic query controls/events are explicit; this is not unmodified gameplay."}}};
    }
    throw std::runtime_error("No three consecutive catalogue draws for resource pool fixture");
}
void Prewarm(SceneExecutor& scene,std::span<const RetainedOpaqueCatalogFixtureSource> sources) {
    for(bool optional:{false,true})for(const auto& source:sources)
        for(uint32_t front:{uint32_t(plume::RenderFrontFace::CLOCKWISE),uint32_t(plume::RenderFrontFace::COUNTER_CLOCKWISE)}) {
            const NativeFsiPipelineRecipe recipe{std::string(source.pair.vertex),std::string(source.pair.pixel),
                {uint32_t(plume::RenderCullMode::BACK),front,1,uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST),0,0},
                int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            if(optional)Require(scene.PrewarmRetainedOpaquePipeline(recipe,source.vertex,source.pixel),"Resource pool optional startup refused");
            else scene.PrewarmFsiPipeline(recipe,source.vertex,source.pixel);
        }
}
Json Query(uint64_t generation,uint32_t report,uint32_t frame,uint64_t event,bool end) {
    return {{"type",end?"query_end":"query_begin"},{"query_generation",generation},
        {"query_handle",0x8844},{"report_address",report},{"frame",frame},{"event",event}};
}
Json Queries(const std::vector<CompletedNativeQuery>& results) {
    Json out=Json::array();for(const auto& q:results)out.push_back({{"generation",q.generation},{"frame",q.frame},
        {"event",q.end_event},{"handle",q.result.handle},{"begin_address",q.result.begin_address},
        {"end_address",q.result.end_address},{"draws",q.result.draws},{"samples",q.result.samples}});return out;
}
uint64_t Value(const Json& report,const char* key) {return report.at("retained_opaque_catalog").at(key).get<uint64_t>();}
uint64_t Pool(const Json& report,const char* key) {return report.at("retained_opaque_catalog").at("resource_pool").at(key).get<uint64_t>();}
uint64_t Frontier(const Json& report,const char* key) {return report.at("gpu_work_queue").at(key).get<uint64_t>();}
Json Observe(const Witness& w) {
    return {{"valid",w.valid},{"pooled",w.pooled},{"dirty",w.dirty},
        {"generation",w.ticket.generation},{"color_version",w.ticket.color},{"depth_version",w.ticket.depth},
        {"color_identity",w.color},{"depth_identity",w.depth},{"framebuffer_identity",w.framebuffer},
        {"scratch_identity",w.scratch},{"descriptor_identity",w.descriptors},{"canonical_identity",w.canonical},
        {"color_bytes",w.color_bytes},{"depth_bytes",w.depth_bytes},{"scratch_bytes",w.scratch_bytes},
        {"charged_active_and_idle_bytes",w.charged_bytes},{"accepted_work",w.accepted_work},{"completed_work",w.completed_work},
        {"color_before_restore",uint32_t(w.color_before_restore)},{"depth_before_restore",uint32_t(w.depth_before_restore)},
        {"color_layout",uint32_t(w.color_layout)},{"depth_layout",uint32_t(w.depth_layout)},
        {"attachment_lease_expired",w.attachment_lease.expired()},{"scratch_lease_expired",w.scratch_lease.expired()}};
}
void Live(const Witness& w,const char* message) {
    Require(w.valid&&w.pooled&&w.dirty&&w.ticket.generation&&w.ticket.color&&w.ticket.depth&&
        w.color&&w.depth&&w.framebuffer&&w.scratch&&w.descriptors&&w.canonical&&
        w.color_bytes&&w.depth_bytes&&w.scratch_bytes&&!w.attachment_lease.expired()&&!w.scratch_lease.expired(),message);
}
struct Result {std::vector<uint8_t> edram,physical;Json queries,witness;};
Result Run(const SceneExecutorConfig& original,const Input& input,
        std::span<const RetainedOpaqueCatalogFixtureSource> sources,bool pool,bool owned_tail) {
    auto config=original;config.present={};config.reuse_retained_opaque_resources=pool;
    config.test_retained_resource_pool_diagnostics=true;config.queue_retained_exports_for_draws=true;
    config.defer_native_queries=true;config.retire_completed_query_prefixes=true;
    // Fixture setup drains the authentic prefix once. The bounded controlled
    // tail stays below existing staging/list/upload limits; feature code adds
    // no wait, drain, polling call or queue-pressure exception.
    config.max_draw_batch_size=64;config.max_inflight_batches=4;
    config.output=original.output/(std::string("resource-pool-")+(pool?"on":"off")+(owned_tail?"-owned":"-queries"));
    const uint32_t frame=input.donor->frame;uint64_t event=uint64_t(input.through)+1;
    Witness first,second,reused;uint64_t first_export=0,second_export=0;
    Result out;
    {
        SceneExecutor scene(std::move(config));Prewarm(scene,sources);
        if(!owned_tail)scene.Execute(Query(8101,0x1ffffe20,frame,0,false));
        scene.RunLiveCapturePartialForRetainedFixture(input.operations,input.through);
        const Json prefix=scene.report();
        Require(Value(prefix,"current_run")>=3&&Value(prefix,"selected")>=3&&
            prefix.at("partial_owned_retained_fixture").at("dirty_retained_authority").get<bool>(),
            "Resource pool authentic donor prefix did not admit a dirty run");
        scene.FlushPendingDraws();first=scene.RetainedResourcePoolWitnessForFixture();
        Require(first.valid&&first.dirty&&first.accepted_work==first.completed_work,
            "Resource pool fixture prefix is not an actually fenced dirty generation");
        if(pool)Live(first,"First generation has no generation-owned immutable conversion lease");
        out.witness={{"pool",pool},{"owned_tail",owned_tail},{"controlled_input",input.metadata},
            {"authentic_prefix",prefix},{"fenced_dirty_first_generation",Observe(first)}};
        const auto masked=[&]() {auto draw=input.ordinary;draw.event=event++;scene.Execute(draw);};
        const auto retained=[&]() {auto draw=*input.donor;draw.event=event++;scene.Execute(draw);};
        const auto queued=[&]() {
            const Json before=scene.report();masked();const Json after=scene.report();
            Require(Value(after,"queued_draw_materializations")==Value(before,"queued_draw_materializations")+1&&
                Value(after,"queued_draw_pending")==Value(before,"queued_draw_pending")+1&&
                Value(after,"materializations")==Value(before,"materializations")+1&&
                Frontier(after,"accepted_work")>Frontier(before,"accepted_work")&&
                Frontier(after,"completed_work")<Frontier(after,"accepted_work"),
                "Resource pool tail lost exact accepted/uncompleted queued export coverage");
            return after;
        };
        const Json first_queued=queued();first_export=Frontier(first_queued,"accepted_work");
        if(!owned_tail) {
            scene.Execute(Query(8101,0x1ffffe00,frame,event++,true));
            scene.Execute(Query(8102,0x1ffffde0,frame,event++,false));
        }
        retained();second=scene.RetainedResourcePoolWitnessForFixture();
        Require(second.valid&&second.dirty&&second.ticket.generation>first.ticket.generation&&
            second.ticket.color>first.ticket.color&&second.ticket.depth>first.ticket.depth,
            "Second retained generation reused stale authority or page tokens");
        if(pool) {
            Live(second,"Second generation has no immutable conversion lease");
            Require(!first.attachment_lease.expired()&&!first.scratch_lease.expired()&&
                first.color!=second.color&&first.depth!=second.depth&&first.framebuffer!=second.framebuffer&&
                first.scratch!=second.scratch&&first.descriptors!=second.descriptors&&
                first.canonical==second.canonical&&second.completed_work<first_export&&
                second.completed_work<second.accepted_work&&
                second.charged_bytes<=NativeRetainedOpaqueWriter::kMaximumRetainedImageBytes,
                "Two accepted generations overlapped or reused live leases, or exceeded charged32MiB");
            Require(second.charged_bytes>=first.color_bytes+first.depth_bytes+first.scratch_bytes+
                second.color_bytes+second.depth_bytes+second.scratch_bytes,
                "Overlapping generation charge excluded actual images or scratch");
        }
        out.witness["first_queued_export"]=first_queued;
        out.witness["two_accepted_generations"]={{"first",Observe(first)},{"second",Observe(second)}};
        const Json second_queued=queued();second_export=Frontier(second_queued,"accepted_work");
        if(!owned_tail) {
            scene.Execute(Query(8102,0x1ffffdc0,frame,event++,true));
            scene.Execute(Query(8103,0x1ffffda0,frame,event++,false));
        }
        const Json pressure_before=scene.report();retained();const Json pressure_after=scene.report();
        if(pool) {
            Require(!first.attachment_lease.expired()&&!first.scratch_lease.expired()&&
                !second.attachment_lease.expired()&&!second.scratch_lease.expired()&&
                Frontier(pressure_after,"completed_work")<first_export&&
                Frontier(pressure_after,"completed_work")<second_export,
                "Pressure case did not keep both earlier accepted generations unretired");
            Require(Pool(pressure_after,"budget_refusals")==Pool(pressure_before,"budget_refusals")+1&&
                Value(pressure_after,"resource_refusals")==Value(pressure_before,"resource_refusals")+1&&
                Value(pressure_after,"selected")==Value(pressure_before,"selected")&&
                !scene.RetainedResourcePoolWitnessForFixture().valid,
                "Third generation did not exercise a preaccept32MiB resource refusal and ordinary Native path");
        }
        out.witness["third_generation_pressure"]={{"before",pressure_before},{"after",pressure_after}};
        if(owned_tail) {
            Require(pool&&scene.HasPendingGpuWork()&&!first.attachment_lease.expired()&&!second.scratch_lease.expired(),
                "Owned pool tail lacks accepted uncompleted leases");
            out.witness["pending_before_owned_shutdown"]=scene.report();
            out.witness["destructor_is_only_final_drain"]=true;
        }else {
            scene.Execute(Query(8103,0x1ffffd80,frame,event++,true));
            scene.Execute(Query(8104,0x1ffffd60,frame,event++,false));
            scene.Execute(Query(8104,0x1ffffd40,frame,event++,true));
            if(pool)Require(scene.TakeCompletedQueries().empty(),
                "Hardware or empty query passed uncompleted retained conversion/draw frontier");
            out.witness["query_frontier_before_completion"]=scene.report();
            scene.FlushOldestQuery();auto completed=scene.TakeCompletedQueries();
            Require(completed.size()==4&&completed[0].generation==8101&&completed[1].generation==8102&&
                completed[2].generation==8103&&completed[3].generation==8104&&
                completed[0].result.draws>=4&&completed[1].result.draws==2&&completed[2].result.draws==1&&
                completed[0].result.samples>0&&completed[1].result.samples>0&&completed[2].result.samples>0&&
                completed[3].result.draws==0&&completed[3].result.samples==0,
                "Resource pool actual nonempty/empty query counts or identities differ");
            const Json fenced=scene.report();
            if(pool)Require(Frontier(fenced,"completed_work")>=first_export&&Frontier(fenced,"completed_work")>=second_export&&
                first.attachment_lease.expired()&&first.scratch_lease.expired()&&
                second.attachment_lease.expired()&&second.scratch_lease.expired(),
                "Pool returned cross-generation leases before all actual packet/draw fences retired");
            out.witness["after_actual_covering_fence"]={{"report",fenced},{"first",Observe(first)},{"second",Observe(second)}};
            const auto attachment_reuses=Pool(fenced,"attachment_reuses");
            const auto scratch_reuses=Pool(fenced,"scratch_reuses");
            const auto reused_layouts=Pool(fenced,"reused_restore_layouts");
            scene.Execute(Query(8105,0x1ffffd20,frame,event++,false));retained();
            reused=scene.RetainedResourcePoolWitnessForFixture();
            const Json reuse_report=scene.report();
            Require(reused.valid&&reused.dirty&&reused.ticket.generation>second.ticket.generation&&
                reused.ticket.color>second.ticket.color&&reused.ticket.depth>second.ticket.depth,
                "Retired resource reuse carried old generation or page authority");
            if(pool) {
                Live(reused,"Retired physical resources did not receive new lease owners");
                const bool first_bundle=reused.color==first.color&&reused.depth==first.depth&&reused.framebuffer==first.framebuffer;
                const bool second_bundle=reused.color==second.color&&reused.depth==second.depth&&reused.framebuffer==second.framebuffer;
                Require((first_bundle||second_bundle)&&
                    (reused.scratch==first.scratch||reused.scratch==second.scratch)&&
                    reused.color_before_restore==plume::RenderTextureLayout::COPY_SOURCE&&
                    reused.depth_before_restore==plume::RenderTextureLayout::COPY_SOURCE&&
                    Pool(reuse_report,"attachment_reuses")==attachment_reuses+1&&
                    Pool(reuse_report,"scratch_reuses")==scratch_reuses+1&&
                    Pool(reuse_report,"reused_restore_layouts")==reused_layouts+1,
                    "Actual fenced resources were not reused from known COPY_SOURCE layouts");
            }
            out.witness["actual_retired_reuse"]={{"generation",Observe(reused)},{"report",reuse_report}};
            scene.Execute(Query(8105,0x1ffffd00,frame,event++,true));
            scene.FlushOldestQuery();auto last=scene.TakeCompletedQueries();
            Require(last.size()==1&&last[0].generation==8105&&last[0].result.draws==1&&last[0].result.samples>0,
                "Reused generation actual query failed");
            completed.insert(completed.end(),last.begin(),last.end());out.queries=Queries(completed);
            out.edram=scene.ReadFullEdram();
            Require(out.edram.size()==10u*1024*1024&&!scene.HasPendingGpuWork(),
                "Strong full EDRAM read did not materialize and fence pooled authority");
            std::ifstream file(original.capture/"gpu-history.json");Json history;file>>history;
            Require(bool(file)&&!history.at("ranges").empty(),"Pool strong CPU read requires captured physical history");
            out.physical=scene.ReadPhysical(history.at("ranges").front().at("address").get<uint32_t>(),4);
            Require(!scene.HasPendingGpuWork(),"Strong CPU read left accepted pool work pending");
            out.witness["completed_queries"]=out.queries;out.witness["final_edram_sha256"]=ContentHash(out.edram);
            out.witness["after_strong_completion"]=scene.report();
        }
        // Witnesses hold weak references only. This scope's ordinary Scene
        // destructor is responsible for the owned-tail exports/fences/device.
    }
    out.witness["owned_destructor_returned"]=true;
    if(pool)Require(first.attachment_lease.expired()&&first.scratch_lease.expired()&&
        second.attachment_lease.expired()&&second.scratch_lease.expired(),
        "Owned Scene returned while captured generation leases still survive");
    out.witness["leases_after_owned_destructor"]={{"first",Observe(first)},{"second",Observe(second)},
        {"reused",Observe(reused)}};
    return out;
}
}
void RetainedResourcePoolSceneFixture(const SceneExecutorConfig& original,
        std::span<const LiveOperation* const> operations,std::span<const RetainedOpaqueCatalogFixtureSource> sources) {
    Require(original.retained_opaque_catalog&&original.reuse_retained_opaque_resources&&
        original.queue_retained_exports_for_draws&&original.native_fsi&&original.unified_gpu_queue&&
        original.async_gpu_submissions&&sources.size()==3&&
        (original.async_submission_capacity==64||original.async_submission_capacity==128),
        "Pool fixture requires explicit V2 queued asynchronous Native and64/128 capacity");
    std::ifstream history_file(original.capture/"gpu-history.json");Json history;history_file>>history;
    Require(bool(history_file)&&history.at("provenance")=="reference_gpu_frame_boundary"&&
        !history.at("edram").is_null()&&history.at("edram").at("bytes")==10u*1024*1024,
        "Pool fixture requires authenticated captured full10MiB history");
    const auto input=MakeInput(operations);
    auto off=Run(original,input,sources,false,false);
    auto on=Run(original,input,sources,true,false);
    Require(off.edram==on.edram&&off.physical==on.physical&&off.queries==on.queries,
        "Resource pool OFF/ON full10MiB, CPU bytes or real query identity/samples differs");
    auto pending=Run(original,input,sources,true,true);
    Json result={{"capacity",original.async_submission_capacity},{"off",std::move(off.witness)},
        {"on",std::move(on.witness)},{"partial_owned",std::move(pending.witness)},
        {"full10MiB_query_cpu_parity",true},
        {"captured_edram_history",{{"bytes",history.at("edram").at("bytes")},{"sha256",history.at("edram").at("sha256")},
            {"provenance",history.at("provenance")},{"frame",history.at("frame")}}},
        {"scope","Actual owned Scenes with identical controlled OFF/ON inputs; two unretired accepted generations, third preaccept pressure refusal, actual covering fence before physical reuse from COPY_SOURCE, fresh authority, hardware/empty query parity and normal pending destructor. Captured history is authenticated by the original Run path. Allocation attempts rejected before charge can temporarily exceed the charged32MiB budget. No performance claim."}};
    std::ofstream file(original.output/"retained-resource-pool-scene-fixture.json");file<<result.dump(2)<<'\n';
    Require(bool(file),"Cannot preserve resource pool Scene witness");
}
}
