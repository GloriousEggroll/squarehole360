#include "retained_shared_depth_scene_fixture.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/retained_opaque_writer.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/live_draw.h"
#include "renderer/native/pipeline_prewarm.h"
#include "plume_render_interface.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
constexpr uint32_t kWidth=1280,kHeight=720,kPlaneBytes=720*5120,kBytes=2048*5120;
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void Word(std::vector<uint8_t>& bytes,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(24-8*i));}
void Number(std::vector<uint8_t>& bytes,size_t at,float value){Word(bytes,at,std::bit_cast<uint32_t>(value));}
CanonicalResourceRef Resource(std::vector<uint8_t> bytes){const auto hash=ContentHash(bytes);return {CanonicalDigest(hash),std::make_shared<const std::vector<uint8_t>>(std::move(bytes))};}
CanonicalResourceRef Floats(std::initializer_list<float> values){std::vector<uint8_t> bytes(values.size()*4);std::memcpy(bytes.data(),values.begin(),bytes.size());return Resource(std::move(bytes));}
struct Input {const TypedLiveDraw* writer=nullptr;const TypedLiveDraw* independent_writer=nullptr;CanonicalResourceRef query_vertex;uint32_t through=0;};
Input MakeInput(std::span<const LiveOperation* const> operations){
    Require(operations.size()<=65536,"Shared-depth fixture operation bound");Input out;unsigned count=0;const NativeRetainedOpaqueCatalogPair* previous=nullptr;
    for(const auto* operation:operations){const auto* d=operation?operation->Draw():nullptr;
        if(d&&d->vertex_shader&&d->vertex_shader->hash.View()==kReadonlyQueryVertexSource&&d->vertex_shader->owner)out.query_vertex=*d->vertex_shader;
        const auto* pair=d&&d->vertex_shader&&d->pixel_shader?RetainedOpaqueCatalogPair(d->vertex_shader->hash.View(),d->pixel_shader->hash.View()):nullptr;
        if(pair&&!out.independent_writer&&d->function==DrawFunction::DrawIndexedPrimitive&&d->args[1]==4&&
           std::all_of(d->geometry.begin(),d->geometry.end(),[](const auto& g){return !g.gpu_memory&&bool(g.payload.owner);})&&
           std::all_of(d->up_geometry.begin(),d->up_geometry.end(),[](const auto& g){return !g.gpu_memory&&bool(g.payload.owner);})&&
           std::all_of(d->textures.begin(),d->textures.end(),[](const auto& t){return t.source==TextureSource::CpuSnapshot&&t.base_payload&&bool(t.base_payload->owner)&&(!t.mip_payload||bool(t.mip_payload->owner));})) {
            const auto& bytes=d->lazy_device_state?d->lazy_device_state->Owner():d->device_state.owner;
            if(bytes&&bytes->size()==LazyDeviceState::kBytes) {
                const auto raw=[&](size_t at){return uint32_t(bytes->at(at))<<24|uint32_t(bytes->at(at+1))<<16|uint32_t(bytes->at(at+2))<<8|bytes->at(at+3);};
                if(raw(0x2934)==0x700766&&raw(0x28dc)==15&&raw(0x2918)==std::bit_cast<uint32_t>(-1.f)&&raw(0x291c)==std::bit_cast<uint32_t>(1.f))out.independent_writer=d;
            }
        }
        count=pair?(pair==previous?count+1:1):0;previous=pair;
        if(!out.writer&&count==3){Require(operation->Event()<UINT32_MAX,"Shared-depth fixture event bound");out.writer=d;out.through=uint32_t(operation->Event());}
    }
    Require(out.writer&&out.independent_writer&&out.query_vertex.owner,"Shared-depth fixture needs an actual material prefix, CPU-owned donor, and authenticated query VS owner");return out;
}
NativeFsiPipelineRecipe QueryRecipe(){return {std::string(kReadonlyQueryVertexSource),std::nullopt,{3,2,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};}
void Prewarm(SceneExecutor& scene,std::span<const RetainedOpaqueCatalogFixtureSource> sources,
    std::span<const uint32_t> vertex,std::span<const uint32_t> builtin,bool query_startup,bool budget_refusal){
    for(const auto& source:sources)for(uint32_t front:{1u,2u})scene.PrewarmFsiPipeline(
        {std::string(source.pair.vertex),std::string(source.pair.pixel),{3,front,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1},source.vertex,source.pixel);
    scene.PrewarmFsiPipeline(QueryRecipe(),vertex,builtin);
    for(const auto& source:sources)for(uint32_t front:{1u,2u})Require(scene.PrewarmRetainedOpaquePipeline(
        {std::string(source.pair.vertex),std::string(source.pair.pixel),{3,front,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1},source.vertex,source.pixel),"Shared-depth material startup refused");
    const auto before=scene.report();
    if(query_startup)Require(scene.PrewarmRetainedSharedDepthQueryPipeline(QueryRecipe(),vertex,builtin)!=budget_refusal,"Shared query PSO budget admission differs from exact fixture limit");
    scene.PrewarmFsiPipeline(QueryRecipe(),vertex,builtin);
    for(const auto& source:sources)for(uint32_t front:{1u,2u})scene.PrewarmFsiPipeline(
        {std::string(source.pair.vertex),std::string(source.pair.pixel),{3,front,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1},source.vertex,source.pixel);
    const auto after=scene.report();
    Require(after.at("shader_cache").at("evictions")==before.at("shader_cache").at("evictions")&&
        after.at("pipeline_cache").at("evictions")==before.at("pipeline_cache").at("evictions"),"Shared optional startup evicted an original owner");
    if(!query_startup||budget_refusal)Require(after.at("shader_cache").at("creations")==before.at("shader_cache").at("creations")&&
        after.at("pipeline_cache").at("creations")==before.at("pipeline_cache").at("creations"),"Rejected shared startup altered resident original owners");
}
TypedLiveDraw QueryDraw(const Input& input,uint64_t event,bool reverse,bool positive,bool unsupported){
    TypedLiveDraw draw;draw.frame=input.writer->frame;draw.event=event;draw.function=DrawFunction::DrawPrimitive;draw.args={0,4,0,3,0,0,0,0};
    draw.vertex_shader=input.query_vertex;
    std::vector<uint8_t> state(LazyDeviceState::kBytes);
    for(unsigned i=0;i<4;++i)Number(state,0x780+16*i+4*i,1.f);
    Word(state,0x2880,kWidth);Word(state,0x28bc,kWidth|(kHeight<<16));Word(state,0x28c4,0x80000000);Word(state,0x28c8,kWidth|(kHeight<<16));
    Word(state,0x28dc,0);Word(state,0x2944,1<<19);Word(state,0x294c,0x43f);Word(state,0x2948,(1<<11)|(1<<12)|2u);Word(state,0x29c0,1);
    Number(state,0x2908,640);Number(state,0x290c,640);Number(state,0x2910,-360);Number(state,0x2914,360);
    Number(state,0x2918,reverse?-1.f:1.f);Number(state,0x291c,reverse?1.f:0.f);Word(state,0x2934,unsupported?0x700766:0x700762);
    draw.device_state=Resource(std::move(state));
    draw.surfaces[4]=TypedSurface{0,kWidth,kHeight,kWidth,0,0,1,true};
    const float clip=reverse?(positive?0.f:1.f):(positive?1.f:0.f);
    const std::array<CanonicalResourceRef,3> resources{Resource({0,1,2,0}),Floats({0,0,0,1,1,1}),Floats({-1,-1,clip,3,-1,clip,-1,3,clip})};
    for(unsigned i=0;i<3;++i){TypedGeometry geometry;geometry.kind=14;geometry.address=0x7000;geometry.bytes=uint32_t(resources[i].owner->size());geometry.payload=resources[i];geometry.fetch_slot=93+i;geometry.endian=0;draw.geometry.push_back(std::move(geometry));}
    draw.vertex_fetch_slots={93,94,95};return draw;
}
Json Query(uint64_t generation,uint32_t address,uint32_t frame,uint64_t event,bool end){return {{"type",end?"query_end":"query_begin"},{"query_generation",generation},{"query_handle",0x7733},{"report_address",address},{"frame",frame},{"event",event}};}
Json Results(const std::vector<CompletedNativeQuery>& queries){Json rows=Json::array();for(const auto& q:queries)rows.push_back({q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});return rows;}
void RequireUnavailable(SceneExecutor& scene,bool prefix,uint64_t target_generation,
        std::vector<CompletedNativeQuery>& completed,const char* why) {
    Require(scene.HasPendingGpuWork(),why);
    const auto count=scene.DeferredQueryCount();
    const auto before=scene.report().at("gpu_work_queue");
    size_t consumed=0;
    if(prefix) {
        auto ready=scene.TakeCompletedQueries();
        Require(std::none_of(ready.begin(),ready.end(),[&](const auto& q){return q.generation==target_generation;}),why);
        consumed=ready.size();completed.insert(completed.end(),ready.begin(),ready.end());
    }else {
        bool refused=false;
        try {(void)scene.TakeCompletedQueries();}
        catch(const std::runtime_error& error) {
            if(std::string_view(error.what())!="Native query consumption before all GPU work completed")throw;
            refused=true;
        }
        Require(refused,why);
    }
    Require(count>=consumed&&scene.DeferredQueryCount()==count-consumed,"Readiness probe lost deferred identities");
    const auto report=scene.report();const auto& after=report.at("gpu_work_queue");
    Require(before.at("accepted_work")==after.at("accepted_work")&&before.at("completed_work")==after.at("completed_work"),
        "Readiness probe changed actual accepted/completed GPU frontiers");
    const auto& shared=report.at("retained_shared_depth_queries");
    Require(shared.at("last_end_required_work").get<uint64_t>()>after.at("completed_work").get<uint64_t>(),
        "Targeted shared query lacks a genuinely pending all-work frontier");
}
struct AcceptedWork {bool pending=false;~AcceptedWork(){if(pending)std::terminate();}};
Json Witness(const Json& report){Json out;for(const auto* key:{"device","retained_opaque_catalog","retained_shared_depth_queries","gpu_work_queue","shader_cache","pipeline_cache"})if(report.contains(key))out[key]=report.at(key);return out;}
struct Outcome {std::vector<uint8_t> memory;Json queries,witness;};
Outcome Run(const SceneExecutorConfig& original,std::span<const LiveOperation* const> operations,const Input& input,
    std::span<const RetainedOpaqueCatalogFixtureSource> sources,std::span<const uint32_t> vertex,std::span<const uint32_t> builtin,
    bool shared,bool reverse,bool prefix,bool missing,bool budget,bool framebuffer_failure,bool unsupported,bool foreign,bool fenced_dirty,bool owned_tail){
    auto config=original;config.retained_shared_depth_queries=shared;config.queue_retained_exports_for_draws=false;
    config.defer_native_queries=true;config.retire_completed_query_prefixes=prefix;config.present={};
    config.max_draw_batch_size=16; // Targeted first/empty tails remain below the submit threshold; later loops exceed real ring capacity.
    const uint32_t logical_capacity=original.async_submission_capacity;
    config.max_deferred_queries=logical_capacity; // Exercise matching actual64/128 logical and physical capacities.
    if(budget)config.max_pipeline_cache_entries=13; // Seven generic plus six material PSOs; query alternative has no hard slot.
    config.test_retained_shared_depth_before_framebuffer={};
    unsigned framebuffer_attempts=0;bool framebuffer_failure_armed=false;if(shared&&framebuffer_failure)config.test_retained_shared_depth_before_framebuffer=[&]{if(framebuffer_failure_armed&&++framebuffer_attempts==1)throw std::runtime_error("Injected preaccept shared framebuffer allocation refusal");};
    config.output=original.output/(std::string("shared-depth-")+(shared?"on":"off")+(reverse?"-reverse":"-forward")+(prefix?"-prefix":"-allwork")+
        (owned_tail?"-owned":fenced_dirty?"-fenced-dirty":foreign?"-foreign":unsupported?"-writer-refusal":framebuffer_failure?"-framebuffer-refusal":budget?"-budget-refusal":missing?"-missing":"-normal"));
    Outcome out;std::vector<CompletedNativeQuery> completed;Json expected=Json::array();uint64_t event=input.through+1ull,generation=8000;
    const auto* writer_input=budget?input.independent_writer:input.writer;
    {
        SceneExecutor scene(std::move(config));AcceptedWork accepted;
        Require(scene.report().at("device").at("software")==false,"Shared depth fixture must use actual hardware");
        Prewarm(scene,sources,vertex,builtin,shared&&!missing,budget&&shared);
        // Authentic physical memory/history is imported by this prefix. Then
        // a separate explicitly synthetic depth plane is installed through the
        // strong API, preserving imported color/unused words and captured physical ranges.
        std::vector<uint8_t> seed(kBytes,0);
        if(!budget) {
            scene.RunLiveCapturePartialForRetainedFixture(operations,input.through);accepted.pending=scene.HasPendingGpuWork();
            seed=scene.ReadFullEdram();accepted.pending=false;Require(seed.size()==kBytes,"Shared fixture canonical history size");
        }
        // The hard13-PSO case uses a fresh Scene and a completely CPU-owned
        // captured material donor; unrelated capture PSOs cannot consume its
        // intentionally exhausted startup budget before the actual query test.
        for(uint32_t at=0;at<kPlaneBytes;at+=4){seed[at]=0x5a;seed[at+1]=0;seed[at+2]=0;seed[at+3]=0xe0;}
        scene.InitializeEdram(seed);framebuffer_failure_armed=true;
        const auto seeded_hash=ContentHash(seed);
        for(unsigned i=0;i<3;++i){auto writer=*writer_input;writer.frame=input.writer->frame;writer.event=event++;scene.Execute(writer);accepted.pending=true;}
        const auto before=scene.report();Require(before.at("retained_opaque_catalog").at("current_run").get<uint64_t>()==3&&
            before.at("retained_opaque_catalog").at("restores").get<uint64_t>()==before.at("retained_opaque_catalog").at("materializations").get<uint64_t>()+1,
            "Controlled shared fixture did not establish one dirty three-draw generation");
        const auto restores=before.at("retained_opaque_catalog").at("restores").get<uint64_t>(),exports=before.at("retained_opaque_catalog").at("materializations").get<uint64_t>();
        const auto collect=[&]{auto result=scene.TakeCompletedQueries();completed.insert(completed.end(),result.begin(),result.end());};
        // A zero-draw real query follows three UNQUERIED writers, before any
        // queried geometry. It must depend on the restore and graphics lists.
        const auto empty_initial=++generation;const uint32_t empty_address=0x14000;
        scene.Execute(Query(empty_initial,empty_address+32,input.writer->frame,event++,false));
        const auto empty_end=event++;scene.Execute(Query(empty_initial,empty_address,input.writer->frame,empty_end,true));
        expected.push_back({empty_initial,input.writer->frame,empty_end,0x7733,empty_address+32,empty_address,0,0});
        const auto empty_report=scene.report();
        if(shared){const auto& q=empty_report.at("retained_shared_depth_queries");
            Require(q.at("last_end_required_work").get<uint64_t>()>q.at("last_end_completed_work").get<uint64_t>()&&
                scene.HasPendingGpuWork(),"Initial empty query bypassed unqueried retained restore/writers");
            RequireUnavailable(scene,prefix,empty_initial,completed,"Initial empty query became consumable before prior work");
            Require(empty_report.at("retained_opaque_catalog").at("restores")==restores&&
                empty_report.at("retained_opaque_catalog").at("materializations")==exports,"Initial empty query materialized dirty authority");}
        Json fenced_witness=nullptr;
        if(fenced_dirty){scene.FlushOldestQuery();collect();accepted.pending=scene.HasPendingGpuWork();
            Require(!accepted.pending,"Fenced dirty witness still has pending GPU work");
            Require(Results(completed)==expected,"Fenced initial empty query changed its real identity or zero samples");
            const auto fenced_report=scene.report();
            if(shared){const auto required=empty_report.at("retained_shared_depth_queries").at("last_end_required_work").get<uint64_t>();
                Require(fenced_report.at("gpu_work_queue").at("accepted_work").get<uint64_t>()>=required&&
                    fenced_report.at("gpu_work_queue").at("completed_work").get<uint64_t>()>=required&&
                    fenced_report.at("retained_opaque_catalog").at("current_run")==3&&
                    fenced_report.at("retained_opaque_catalog").at("restores")==restores&&
                    fenced_report.at("retained_opaque_catalog").at("materializations")==exports,"Waiting real query frontier materialized canonical authority");}
            fenced_witness={{"has_pending_gpu_work",false},{"completed_queries",Results(completed)},{"snapshot",Witness(fenced_report)}};
        }
        const uint32_t count=owned_tail?2:original.async_submission_capacity+8;
        for(uint32_t i=0;i<count;++i){
            if(scene.DeferredQueryCount()>=logical_capacity){scene.FlushOldestQuery();collect();}
            const bool positive=i%3!=2;const uint32_t address=0x8000+64*(i%13);++generation;
            scene.Execute(Query(generation,address+32,input.writer->frame,event++,false));
            auto draw=QueryDraw(input,event++,reverse,positive,unsupported);scene.Execute(draw);accepted.pending=true;
            const auto end_event=event++;scene.Execute(Query(generation,address,input.writer->frame,end_event,true));
            expected.push_back({generation,input.writer->frame,end_event,0x7733,address+32,address,1,positive?uint64_t(kWidth)*kHeight:0});
            if(i==0&&shared&&!missing&&!budget&&!unsupported&&!framebuffer_failure&&!fenced_dirty)
                RequireUnavailable(scene,prefix,generation,completed,"First real shared query became ready before its original GPU list");
            // Query End must include all original accepted work, even when
            // prefix retirement is OFF. No fence/sample substitution is used.
            if(shared&&!missing&&!budget&&!unsupported&&!framebuffer_failure){const auto& s=scene.report().at("retained_shared_depth_queries");
                Require(s.at("last_end_required_work").get<uint64_t>()>0&&s.at("last_end_required_work").get<uint64_t>()>=s.at("last_end_completed_work").get<uint64_t>(),"Shared End lost actual all-work watermark");}
            if(i%4==3){auto writer=*writer_input;writer.frame=input.writer->frame;writer.event=event++;scene.Execute(writer);accepted.pending=true;}
            if(i%8==7){
                if(scene.DeferredQueryCount()>=logical_capacity){scene.FlushOldestQuery();collect();}
                const auto empty_generation=++generation;const uint32_t empty_address=0x10000+64*(i%13);
                scene.Execute(Query(empty_generation,empty_address+32,input.writer->frame,event++,false));
                const auto empty_end=event++;scene.Execute(Query(empty_generation,empty_address,input.writer->frame,empty_end,true));
                expected.push_back({empty_generation,input.writer->frame,empty_end,0x7733,empty_address+32,empty_address,0,0});
                if(i==7&&shared&&!missing&&!budget&&!unsupported&&!framebuffer_failure)
                    RequireUnavailable(scene,prefix,empty_generation,completed,"Empty query bypassed the earlier unqueried writer/all-work frontier");
            }
        }
        const auto prepared=scene.report();out.witness={{"shared",shared},{"reverse_query_viewport",reverse},{"prefix_retirement",prefix},{"missing_query_pso",missing},
            {"query_pso_budget_refusal",budget},{"framebuffer_failure",framebuffer_failure},{"unsupported_depth_writer",unsupported},{"foreign",foreign},{"fenced_dirty",fenced_dirty},{"owned_tail",owned_tail},
            {"device",prepared.at("device")},{"captured_prefix_imported",!budget},{"cpu_owned_material_donor",budget},
            {"synthetic_depth_plane_seed_sha256",seeded_hash},{"before_queries",Witness(before)},
            {"initial_empty_query_frontier",Witness(empty_report)},{"fenced_dirty_before_queries",fenced_witness},{"before_completion",Witness(prepared)}};
        if(shared){const auto& stats=prepared.at("retained_shared_depth_queries");
            const bool refused=missing||budget||unsupported;
            const auto& initial=before.at("retained_shared_depth_queries");
            Require(prepared.at("shader_cache").at("creations")==before.at("shader_cache").at("creations")&&
                prepared.at("pipeline_cache").at("creations")==before.at("pipeline_cache").at("creations"),"Shared controlled gameplay created a shader module or PSO");
            Require(stats.at("selected")==stats.at("queried_selected")&&stats.at("selected")==stats.at("promotions"),"Shared accepted selection/query/promotion counters diverge");
            if(refused)Require(stats.at("selected")==initial.at("selected")&&stats.at("promotions")==initial.at("promotions"),"Unsupported/missing controlled query selected hardware sharing");
            else Require(stats.at("selected").get<uint64_t>()>initial.at("selected").get<uint64_t>(),"Shared controlled real hardware query coverage is vacuous");
            if(missing||budget)Require(stats.at("residency_refusals").get<uint64_t>()>initial.at("residency_refusals").get<uint64_t>(),"Missing controlled query PSO lacks actual residency refusal");
            if(budget)Require(stats.at("prewarm_refusals")==1&&stats.at("module_bytes")==0,"Hard query PSO refusal altered module budget");
            if(framebuffer_failure)Require(stats.at("framebuffer_refusals").get<uint64_t>()==initial.at("framebuffer_refusals").get<uint64_t>()+1&&framebuffer_attempts>=2,"Preaccept framebuffer refusal did not resume on a fresh generation");
            if(!refused&&!framebuffer_failure)Require(prepared.at("retained_opaque_catalog").at("restores")==restores&&
                prepared.at("retained_opaque_catalog").at("materializations")==exports,"Shared readonly queries converted/exported the same dirty generation");
        }
        if(owned_tail){Require(scene.HasPendingGpuWork()&&scene.DeferredQueryCount()>0,"Shared owned teardown lacks pending real query owners");accepted.pending=false;}
        else{
            if(foreign){const auto old=scene.report().at("retained_opaque_catalog").at("restores").get<uint64_t>();
                scene.InitializeEdram(seed);accepted.pending=scene.HasPendingGpuWork();auto writer=*writer_input;writer.frame=input.writer->frame;writer.event=event++;scene.Execute(writer);accepted.pending=true;
                Require(scene.report().at("retained_opaque_catalog").at("restores").get<uint64_t>()==old+1,"Strong foreign write reused stale retained authority");}
            scene.FlushGpuWork();collect();accepted.pending=false;out.queries=Results(completed);
            Require(out.queries==expected,"Real shared query identity/zero/positive/empty samples differ from independent fullscreen oracle");
            Require(!scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==0,"Shared query/packet frontier did not drain");
            out.memory=scene.ReadFullEdram();Require(out.memory.size()==kBytes,"Shared full memory read extent");
            for(uint32_t at=0;at<kPlaneBytes;at+=4)Require(out.memory[at]==0x5a,"Readonly sharing changed packed stencil low8");
            out.witness["queries"]=out.queries;out.witness["final_edram_sha256"]=ContentHash(out.memory);out.witness["completed"]=Witness(scene.report());
        }
        // On the owned-tail path the real Scene destructor materializes/drains;
        // no explicit flush removes the original framebuffer/image/packet lease.
    }
    out.witness["owned_destructor_returned"]=true;return out;
}
}
void RetainedSharedDepthSceneFixture(const SceneExecutorConfig& original,std::span<const LiveOperation* const> operations,
    std::span<const RetainedOpaqueCatalogFixtureSource> sources,std::span<const uint32_t> vertex,std::span<const uint32_t> builtin){
    Require(original.retained_opaque_catalog&&original.retained_shared_depth_queries&&original.defer_native_queries&&
        original.async_gpu_submissions&&original.unified_gpu_queue&&sources.size()==3,"Shared fixture needs exclusive asynchronous Native V2 sharing setup");
    const auto input=MakeInput(operations);Json cases=Json::array();
    for(unsigned mode=0;mode<9;++mode){const bool reverse=mode==1,prefix=mode==2,missing=mode==3,budget=mode==4,framebuffer=mode==5,unsupported=mode==6,foreign=mode==7,fenced=mode==8;
        auto off=Run(original,operations,input,sources,vertex,builtin,false,reverse,prefix,missing,budget,framebuffer,unsupported,foreign,fenced,false);
        auto on=Run(original,operations,input,sources,vertex,builtin,true,reverse,prefix,missing,budget,framebuffer,unsupported,foreign,fenced,false);
        Require(off.memory==on.memory&&off.queries==on.queries,"Shared OFF/ON full10MiB or real queried samples/identity parity failed");
        cases.push_back({{"mode",mode},{"off",std::move(off.witness)},{"on",std::move(on.witness)},{"full10MiB_queries_exact",true}});
    }
    auto tail=Run(original,operations,input,sources,vertex,builtin,true,true,false,false,false,false,false,false,false,true);cases.push_back(std::move(tail.witness));
    Json proof{{"capacity",original.async_submission_capacity},{"logical_query_capacity",original.async_submission_capacity},{"cases",std::move(cases)},
        {"scope","Actual owned Scene with captured material/physical-memory prefix (hard-budget case instead uses a fresh Scene and CPU-owned captured donor), followed by explicit synthetic FP24.5/stencil5a depth-only plane seed and three captured writer draws. Known identity fullscreen queried geometry proves zero/positive real samples in both query viewport ranges, pressure/all-work/empty-query identity, startup hard/missing/framebuffer/unsupported refusal, foreign closure and pending owned destruction. Unchanged authentic full-capture image parity is separate."}};
    std::ofstream file(original.output/"retained-shared-depth-scene-fixture.json");file<<proof.dump(2)<<'\n';Require(bool(file),"Cannot retain shared depth actual fixture witness");
}
namespace {
Json PooledSharedWitness(const NativeRetainedOpaqueResourceWitness& w) {
    return {{"valid",w.valid},{"pooled",w.pooled},{"dirty",w.dirty},
        {"generation",w.ticket.generation},{"color_version",w.ticket.color},{"depth_version",w.ticket.depth},
        {"color",w.color},{"depth",w.depth},{"write_framebuffer",w.framebuffer},{"readonly_framebuffer",w.readonly_framebuffer},
        {"scratch",w.scratch},{"descriptors",w.descriptors},{"charged_bytes",w.charged_bytes},
        {"accepted_work",w.accepted_work},{"completed_work",w.completed_work},
        {"generation_expired",w.generation_lease.expired()},{"attachment_lease_expired",w.attachment_lease.expired()},
        {"scratch_lease_expired",w.scratch_lease.expired()},
        {"color_before_restore",uint32_t(w.color_before_restore)},{"depth_before_restore",uint32_t(w.depth_before_restore)}};
}
Outcome RunPooledShared(const SceneExecutorConfig& original,std::span<const LiveOperation* const> operations,
        const Input& input,std::span<const RetainedOpaqueCatalogFixtureSource> sources,
        std::span<const uint32_t> vertex,std::span<const uint32_t> builtin,bool pool,bool owned_tail) {
    auto config=original;config.present={};config.reuse_retained_opaque_resources=pool;
    config.retained_shared_depth_queries=true;config.queue_retained_exports_for_draws=true;
    config.defer_native_queries=true;config.retire_completed_query_prefixes=true;
    config.max_deferred_queries=original.async_submission_capacity;
    config.max_draw_batch_size=64;config.max_inflight_batches=4;
    config.test_retained_resource_pool_diagnostics=true;config.test_retained_shared_depth_before_framebuffer={};
    config.output=original.output/(std::string("pooled-shared-")+(pool?"pool-on":"pool-off")+(owned_tail?"-owned":"-paired"));
    Outcome out;std::vector<CompletedNativeQuery> completed;Json expected=Json::array();
    NativeRetainedOpaqueResourceWitness first,second,reused;uint64_t event=input.through+1ull;
    {
        SceneExecutor scene(std::move(config));AcceptedWork accepted;
        Require(!scene.report().at("device").at("software").get<bool>(),"Pooled shared fixture requires actual hardware");
        Prewarm(scene,sources,vertex,builtin,true,false);
        scene.RunLiveCapturePartialForRetainedFixture(operations,input.through);accepted.pending=scene.HasPendingGpuWork();
        auto seed=scene.ReadFullEdram();accepted.pending=false;Require(seed.size()==kBytes,"Pooled shared captured history extent");
        for(uint32_t at=0;at<kPlaneBytes;at+=4){seed[at]=0x5a;seed[at+1]=0;seed[at+2]=0;seed[at+3]=0xe0;}
        scene.InitializeEdram(seed);
        const auto writer=[&]{auto draw=*input.independent_writer;draw.frame=input.writer->frame;draw.event=event++;scene.Execute(draw);accepted.pending=true;};
        const auto masked=[&]{auto draw=*input.independent_writer;draw.frame=input.writer->frame;draw.event=event++;
            auto bytes=draw.lazy_device_state?*draw.lazy_device_state->Owner():*draw.device_state.owner;
            Word(bytes,0x28dc,7);draw.lazy_device_state.reset();draw.device_state=Resource(std::move(bytes));
            const auto before=scene.report().at("retained_opaque_catalog");scene.Execute(draw);accepted.pending=true;
            const auto after=scene.report().at("retained_opaque_catalog");
            Require(after.at("queued_draw_materializations").get<uint64_t>()==before.at("queued_draw_materializations").get<uint64_t>()+1&&
                after.at("queued_draw_pending").get<uint64_t>()==before.at("queued_draw_pending").get<uint64_t>()+1,
                "Pooled shared ordinary draw did not queue exactly one still-pending export");};
        const auto query=[&](uint64_t generation,uint32_t address,bool geometry,bool positive){
            scene.Execute(Query(generation,address+32,input.writer->frame,event++,false));
            if(geometry){scene.Execute(QueryDraw(input,event++,true,positive,false));accepted.pending=true;}
            const auto end_event=event++;scene.Execute(Query(generation,address,input.writer->frame,end_event,true));
            expected.push_back({generation,input.writer->frame,end_event,0x7733,address+32,address,geometry?1:0,
                geometry&&positive?uint64_t(kWidth)*kHeight:0});};
        for(unsigned i=0;i<3;++i)writer();query(8201,0x18000,true,true);
        first=scene.RetainedResourcePoolWitnessForFixture();
        Require(first.valid&&first.dirty&&first.readonly_framebuffer&&!first.generation_lease.expired(),
            "First queried generation lacks its actual generation-owned readonly framebuffer");
        const auto query_a=scene.report().at("retained_shared_depth_queries").at("last_end_required_work").get<uint64_t>();
        Require(query_a>first.completed_work,"First pooled shared real query frontier is not pending");
        masked();const auto first_export=scene.report().at("gpu_work_queue").at("accepted_work").get<uint64_t>();
        Require(!scene.RetainedResourcePoolWitnessForFixture().valid&&!first.generation_lease.expired(),
            "Queued export released a generation still owned by the accepted readonly DrawSlot");
        for(unsigned i=0;i<3;++i)writer();query(8202,0x18040,true,false);
        second=scene.RetainedResourcePoolWitnessForFixture();
        Require(second.valid&&second.dirty&&second.readonly_framebuffer&&!second.generation_lease.expired()&&
            !first.generation_lease.expired()&&second.ticket.generation>first.ticket.generation&&
            second.ticket.color>first.ticket.color&&second.ticket.depth>first.ticket.depth,
            "Second queried generation lost fresh authority or an unretired readonly generation");
        if(pool)Require(first.color!=second.color&&first.depth!=second.depth&&first.framebuffer!=second.framebuffer&&
            first.readonly_framebuffer!=second.readonly_framebuffer&&first.scratch!=second.scratch&&first.descriptors!=second.descriptors&&
            !first.attachment_lease.expired()&&!first.scratch_lease.expired()&&
            !second.attachment_lease.expired()&&!second.scratch_lease.expired()&&
            second.charged_bytes<=NativeRetainedOpaqueWriter::kMaximumRetainedImageBytes&&
            second.charged_bytes>=first.color_bytes+first.depth_bytes+first.scratch_bytes+second.color_bytes+second.depth_bytes+second.scratch_bytes,
            "Two queried pooled generations reused live owners or lost charged32MiB accounting");
        out.witness={{"pool",pool},{"owned_tail",owned_tail},{"synthetic_seed_sha256",ContentHash(seed)},
            {"two_pending_queried_generations",{{"first",PooledSharedWitness(first)},{"second",PooledSharedWitness(second)}}}};
        masked();const auto second_export=scene.report().at("gpu_work_queue").at("accepted_work").get<uint64_t>();
        const auto pressure_before=scene.report();writer();const auto pressure_after=scene.report();
        if(pool)Require(pressure_after.at("retained_opaque_catalog").at("resource_pool").at("budget_refusals").get<uint64_t>()==
            pressure_before.at("retained_opaque_catalog").at("resource_pool").at("budget_refusals").get<uint64_t>()+1&&
            pressure_after.at("retained_opaque_catalog").at("selected")==pressure_before.at("retained_opaque_catalog").at("selected")&&
            !scene.RetainedResourcePoolWitnessForFixture().valid&&!first.generation_lease.expired()&&!second.generation_lease.expired(),
            "Third pooled queried generation did not refuse before acceptance with both readonly generations still alive");
        out.witness["third_generation_pressure"]={{"before",Witness(pressure_before)},{"after",Witness(pressure_after)}};
        query(8203,0x18080,false,false);
        Require(scene.TakeCompletedQueries().empty()&&scene.HasPendingGpuWork(),"Pooled shared pending geometry/empty query became ready without a real fence");
        const auto before_fence=scene.report();
        Require(before_fence.at("retained_shared_depth_queries").at("last_end_required_work").get<uint64_t>()>=second_export&&
            before_fence.at("gpu_work_queue").at("completed_work").get<uint64_t>()<first_export,
            "Empty pooled shared query failed to include prior exports/ordinary graphics work");
        scene.FlushOldestQuery();completed=scene.TakeCompletedQueries();accepted.pending=scene.HasPendingGpuWork();
        Require(Results(completed)==expected&&!accepted.pending&&first.generation_lease.expired()&&second.generation_lease.expired(),
            "Actual covering fence did not retire readonly DrawSlots or preserve real positive/zero/empty query identities");
        if(pool)Require(first.attachment_lease.expired()&&first.scratch_lease.expired()&&
            second.attachment_lease.expired()&&second.scratch_lease.expired(),"Actual covering fence did not retire pooled export packet leases");
        out.witness["after_actual_covering_fence"]={{"snapshot",Witness(scene.report())},{"first",PooledSharedWitness(first)},{"second",PooledSharedWitness(second)}};
        // This controlled tail shares ONE real covering submission. It does not
        // claim the oldest query always fences later exports in ordinary runs.
        auto intermediate=scene.ReadFullEdram();accepted.pending=false;
        out.witness["intermediate_edram_sha256"]=ContentHash(intermediate);
        writer();query(8204,0x180c0,true,true);reused=scene.RetainedResourcePoolWitnessForFixture();
        Require(reused.valid&&reused.dirty&&reused.readonly_framebuffer&&!reused.generation_lease.expired()&&
            reused.ticket.generation>second.ticket.generation&&reused.ticket.color>second.ticket.color&&reused.ticket.depth>second.ticket.depth,
            "Retired queried resources carried stale generation or physical-page authority");
        if(pool){const bool first_bundle=reused.color==first.color&&reused.depth==first.depth&&reused.framebuffer==first.framebuffer;
            const bool second_bundle=reused.color==second.color&&reused.depth==second.depth&&reused.framebuffer==second.framebuffer;
            Require((first_bundle||second_bundle)&&
            (reused.scratch==first.scratch||reused.scratch==second.scratch)&&
            reused.color_before_restore==plume::RenderTextureLayout::COPY_SOURCE&&reused.depth_before_restore==plume::RenderTextureLayout::COPY_SOURCE,
            "Pooled query generation did not reuse truly retired images from known COPY_SOURCE layouts");}
        out.witness["reused_queried_generation"]=PooledSharedWitness(reused);
        if(owned_tail){Require(pool&&scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==1&&
            !reused.generation_lease.expired()&&!reused.attachment_lease.expired()&&!reused.scratch_lease.expired(),
            "Pooled shared owned tail has no real pending query/image/framebuffer/scratch ownership");
            out.witness["pending_before_owned_shutdown"]=Witness(scene.report());accepted.pending=false;
        }else{
            scene.FlushGpuWork();auto last=scene.TakeCompletedQueries();completed.insert(completed.end(),last.begin(),last.end());accepted.pending=false;
            out.queries=Results(completed);Require(out.queries==expected&&!scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==0,
                "Pooled shared final query identities or fullscreen samples differ");
            out.memory=scene.ReadFullEdram();Require(out.memory.size()==kBytes,"Pooled shared full10MiB extent");
            for(uint32_t at=0;at<kPlaneBytes;at+=4)Require(out.memory[at]==0x5a,"Pooled readonly query changed canonical stencil");
            out.witness["queries"]=out.queries;out.witness["final_edram_sha256"]=ContentHash(out.memory);out.witness["completed"]=Witness(scene.report());
        }
    }
    Require(first.generation_lease.expired()&&second.generation_lease.expired()&&reused.generation_lease.expired(),
        "Owned Scene returned with a queried retained generation still alive");
    if(pool)Require(reused.attachment_lease.expired()&&reused.scratch_lease.expired(),"Owned pooled shared Scene returned with live conversion/image owners");
    out.witness["owned_destructor_returned"]=true;out.witness["reused_after_owned_shutdown"]=PooledSharedWitness(reused);return out;
}
}
void RetainedPooledSharedDepthSceneFixture(const SceneExecutorConfig& original,std::span<const LiveOperation* const> operations,
        std::span<const RetainedOpaqueCatalogFixtureSource> sources,std::span<const uint32_t> vertex,std::span<const uint32_t> builtin) {
    Require(original.retained_opaque_catalog&&original.retained_shared_depth_queries&&original.reuse_retained_opaque_resources&&
        original.queue_retained_exports_for_draws&&original.defer_native_queries&&original.async_gpu_submissions&&original.unified_gpu_queue&&
        sources.size()==3&&(original.async_submission_capacity==64||original.async_submission_capacity==128),
        "Combined fixture requires explicit V2/shared/pool/queued Native64/128 setup");
    const auto input=MakeInput(operations);
    auto off=RunPooledShared(original,operations,input,sources,vertex,builtin,false,false);
    auto on=RunPooledShared(original,operations,input,sources,vertex,builtin,true,false);
    Require(off.memory==on.memory&&off.queries==on.queries&&off.witness.at("intermediate_edram_sha256")==on.witness.at("intermediate_edram_sha256"),
        "Combined pooled/shared OFF/ON full10MiB/real query/intermediate memory parity differs");
    auto pending=RunPooledShared(original,operations,input,sources,vertex,builtin,true,true);
    Json proof{{"capacity",original.async_submission_capacity},{"logical_query_capacity",original.async_submission_capacity},
        {"off",std::move(off.witness)},{"on",std::move(on.witness)},{"partial_owned",std::move(pending.witness)},
        {"full10MiB_queries_exact",true},{"whole_capture_replay_executed",false},
        {"scope","Controlled actual owned Scene, authenticated captured prefix followed by explicit synthetic FP24.5/stencil5a depth seed. Shared query DrawSlots keep generation-owned readonly framebuffers across queued exports and two overlapping pooled generations. Third admission refuses before recording, actual common covering fence retires old query and export owners, images restore from known COPY_SOURCE with fresh authority; positive/zero/empty real query identities and full10MiB pair exactly. Pending tail uses normal destructor. No general oldest-query covering guarantee, natural whole-capture proof or performance claim."}};
    std::ofstream file(original.output/"retained-pooled-shared-depth-scene-fixture.json");file<<proof.dump(2)<<'\n';Require(bool(file),"Cannot preserve combined pooled/shared witness");
}
}
