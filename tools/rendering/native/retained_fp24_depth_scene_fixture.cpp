#include "retained_fp24_depth_scene_fixture.h"
#include "renderer/native/retained_fp24_depth.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/live_draw.h"
#include "renderer/native/pipeline_prewarm.h"
#include "plume_render_interface.h"
#include <algorithm>
#include <bit>
#include <fstream>
#include <stdexcept>
namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;constexpr uint32_t kBytes=2048*5120,kDepthBytes=720*5120;
void Require(bool pass,const char* why){if(!pass)throw std::runtime_error(why);}
uint32_t Word(std::span<const uint8_t> b,size_t at){return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
void Set(std::vector<uint8_t>& b,size_t at,uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=uint8_t(v>>(24-i*8));}
std::array<const TypedLiveDraw*,2> Donors(std::span<const LiveOperation* const> operations){
    Require(operations.size()<=65536,"Depth Scene fixture operation bound");std::array<const TypedLiveDraw*,2> out{};
    for(const auto* operation:operations){const auto* d=operation?operation->Draw():nullptr;
        if(!d||!d->vertex_shader||d->pixel_shader||d->function!=DrawFunction::DrawIndexedPrimitive||d->args[1]!=4||!d->textures.empty()||!d->surfaces[4])continue;
        const auto i=NativeRetainedFp24DepthSourceIndex(d->vertex_shader->hash.View());if(!i||out[*i])continue;
        const auto& z=*d->surfaces[4];const auto& owner=d->lazy_device_state?d->lazy_device_state->Owner():d->device_state.owner;
        if(z.width!=1280||z.height!=720||z.edram_base||z.format!=1||!z.depth||!owner||owner->size()!=LazyDeviceState::kBytes||
           Word(*owner,0x2934)!=0x700766||Word(*owner,0x28dc)||Word(*owner,0x2918)!=std::bit_cast<uint32_t>(-1.f)||Word(*owner,0x291c)!=std::bit_cast<uint32_t>(1.f))continue;
        const auto cpu=[](const auto& g){return !g.gpu_memory&&bool(g.payload.owner);};
        if(!std::all_of(d->geometry.begin(),d->geometry.end(),cpu)||!std::all_of(d->up_geometry.begin(),d->up_geometry.end(),cpu))continue;
        out[*i]=d;
    }Require(out[0]&&out[1],"Depth Scene fixture lacks both exact CPU-owned donor programs");return out;
}
NativeFsiPipelineRecipe Recipe(unsigned source,uint32_t front){return {std::string(kNativeRetainedFp24DepthSources.at(source).source),std::nullopt,{3,front,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};}
void Prewarm(SceneExecutor& scene,const std::array<std::span<const uint32_t>,2>& vertex,std::span<const uint32_t> builtin,unsigned limit,bool optional,bool hard_refusal){
    for(unsigned i=0;i<2;++i)for(uint32_t front:{1u,2u})scene.PrewarmFsiPipeline(Recipe(i,front),vertex[i],builtin);
    const auto before=scene.report();unsigned attempted=0;
    if(optional&&limit==4&&!hard_refusal) {
        auto bad_recipe=Recipe(0,1);bad_recipe.vertex=std::string(64,'a');
        Require(!scene.PrewarmRetainedFp24DepthPipeline(bad_recipe,vertex[0],builtin),"Unpinned source admitted a depth PSO");
        auto corrupted=std::vector<uint32_t>(vertex[0].begin(),vertex[0].end());corrupted.back()^=1;
        Require(!scene.PrewarmRetainedFp24DepthPipeline(Recipe(0,1),corrupted,builtin),"Wrong compiled VS admitted a depth PSO");
        auto wrong_builtin=std::vector<uint32_t>(builtin.begin(),builtin.end());wrong_builtin.back()^=1;
        Require(!scene.PrewarmRetainedFp24DepthPipeline(Recipe(0,1),vertex[0],wrong_builtin),"Wrong canonical builtin admitted a depth PSO");
        const auto refused=scene.report();Require(refused.at("shader_cache").at("creations")==before.at("shader_cache").at("creations")&&
            refused.at("pipeline_cache").at("creations")==before.at("pipeline_cache").at("creations")&&
            refused.at("retained_fp24_depth_writer").at("prepared")==0,"Malformed optional startup mutated original cache owners");
    }
    if(optional)for(unsigned i=0;i<2;++i)for(uint32_t front:{1u,2u}){
        if(attempted++>=limit)continue;Require(scene.PrewarmRetainedFp24DepthPipeline(Recipe(i,front),vertex[i],builtin)!=hard_refusal,"Depth optional startup result disagrees with budget");}
    for(unsigned i=0;i<2;++i)for(uint32_t front:{1u,2u})scene.PrewarmFsiPipeline(Recipe(i,front),vertex[i],builtin);
    const auto after=scene.report();
    Require(after.at("shader_cache").at("evictions")==before.at("shader_cache").at("evictions")&&after.at("pipeline_cache").at("evictions")==before.at("pipeline_cache").at("evictions"),"Depth optional startup evicted an original owner");
    if(!optional||!limit||hard_refusal)Require(after.at("shader_cache").at("creations")==before.at("shader_cache").at("creations")&&after.at("pipeline_cache").at("creations")==before.at("pipeline_cache").at("creations"),"Refused optional depth admission changed generic ownership");
}
TypedLiveDraw Clone(const TypedLiveDraw& donor,uint64_t event,std::string_view mode){auto d=donor;d.event=event;d.frame=2677;
    if(mode=="normal"||mode=="missing"||mode=="partial"||mode=="budget"||mode=="foreign"||mode=="owned")return d;
    const auto& owner=d.lazy_device_state?d.lazy_device_state->Owner():d.device_state.owner;auto bytes=*owner;std::vector<std::pair<size_t,uint32_t>> old;
    const auto change=[&](size_t at,uint32_t value){old.push_back({at,Word(bytes,at)});Set(bytes,at,value);};
    if(mode=="bias"){change(0x2948,Word(bytes,0x2948)|(1u<<11)|(1u<<12));change(0x2a54,std::bit_cast<uint32_t>(.125f));change(0x2a5c,std::bit_cast<uint32_t>(.125f));}
    else if(mode=="readonly")change(0x2934,0x700762);
    else if(mode=="forward"){change(0x2918,std::bit_cast<uint32_t>(1.f));change(0x291c,0);}
    else if(mode=="format"){change(0x2888,Word(bytes,0x2888)&~(1u<<16));d.surfaces[4]->format=0;d.surfaces[4]->format_info&=~(1u<<16);}
    else if(mode=="mask")change(0x28dc,1);
    else throw std::runtime_error("Unknown depth Scene fixture state");
    auto restored=bytes;for(auto [at,value]:old)Set(restored,at,value);Require(restored==*owner&&bytes!=*owner,"Depth refusal mutation altered unrelated state bytes");
    const auto hash=ContentHash(bytes);d.lazy_device_state.reset();d.device_state={CanonicalDigest(hash),std::make_shared<const std::vector<uint8_t>>(std::move(bytes))};return d;
}
Json Query(uint64_t generation,uint32_t address,uint64_t event,bool end){return {{"type",end?"query_end":"query_begin"},{"query_generation",generation},{"query_handle",0x8877},{"report_address",address},{"frame",2677},{"event",event}};}
Json Results(const std::vector<CompletedNativeQuery>& completed){Json out=Json::array();for(const auto& q:completed)out.push_back({q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});return out;}
struct AcceptedWork {bool pending=false;~AcceptedWork(){if(pending)std::terminate();}};
struct Outcome {std::vector<uint8_t> memory;Json queries,report;};
Outcome Run(const SceneExecutorConfig& base,const std::array<const TypedLiveDraw*,2>& donors,
    const std::array<std::span<const uint32_t>,2>& vertex,std::span<const uint32_t> builtin,bool enabled,std::string_view mode){
    auto config=base;config.present={};config.retained_fp24_depth_writer=enabled;config.native_readonly_depth_attachment=false;
    config.defer_native_queries=true;config.max_draw_batch_size=16;config.max_deferred_queries=config.async_submission_capacity;
    config.output=base.output/(std::string("depth-")+(enabled?"on-":"off-")+std::string(mode));
    if(mode=="budget")config.max_pipeline_cache_entries=4; // Exactly four original PSOs; no optional slot.
    Outcome out;uint64_t event=900000,generation=9000;std::vector<CompletedNativeQuery> completed;Json expected=Json::array();
    {
        SceneExecutor scene(std::move(config));AcceptedWork accepted;Require(scene.report().at("device").at("software")==false,"Depth Scene fixture requires real hardware");
        const unsigned limit=mode=="missing"?0:mode=="partial"?2:4;Prewarm(scene,vertex,builtin,limit,enabled,mode=="budget");
        std::vector<uint8_t> seed(kBytes);uint32_t random=0x17349;
        for(auto& b:seed){random^=random<<13;random^=random>>17;random^=random<<5;b=uint8_t(random);}
        // Depth zero + variable untouched stencil permits all rendered GE fragments.
        // Color and all other physical words keep a nontrivial independent seed.
        for(uint32_t at=0;at<kDepthBytes;at+=4){seed[at+1]=seed[at+2]=seed[at+3]=0;}
        scene.InitializeEdram(seed);
        const auto initial=scene.report();
        if(mode=="owned"){
            for(unsigned i:{0u,1u,0u}){auto draw=Clone(*donors[i],event++,mode);scene.Execute(draw);accepted.pending=true;}
            out.report=scene.report();const auto& stats=out.report.at("retained_fp24_depth_writer");
            Require(enabled&&stats.at("selected")==3&&stats.at("reuses")==2&&stats.at("shader_switches")==2&&stats.at("restores")==1&&stats.at("materializations")==0&&scene.HasPendingGpuWork(),"Depth owned tail lacks accepted dirty source-switch generation");
            out.report["pending_before_owned_shutdown"]={{"pending_gpu_work",scene.HasPendingGpuWork()},{"gpu_work_queue",out.report.at("gpu_work_queue")},{"current_run",stats.at("current_run")},{"restores",stats.at("restores")},{"materializations",stats.at("materializations")},{"dirty_authority_inferred_from_accepted_unexported_run",true}};accepted.pending=false; // The Scene destructor owns the deliberate final drain/export.
        }else{
            const unsigned groups=base.async_submission_capacity+8; // Real logical capacity and actual original pressure.
            for(unsigned group=0;group<groups;++group){
                if(scene.DeferredQueryCount()>=base.async_submission_capacity){scene.FlushOldestQuery();auto ready=scene.TakeCompletedQueries();completed.insert(completed.end(),ready.begin(),ready.end());}
                const uint32_t address=0x1000000+group*128;const uint64_t id=generation++;
                scene.Execute(Query(id,address+32,event++,false));
                const unsigned draw_count=group==0?base.async_submission_capacity+8:3;
                for(unsigned i=0;i<draw_count;++i){const unsigned source=i%2;auto draw=Clone(*donors[source],event++,mode);scene.Execute(draw);accepted.pending=true;}
                const auto end_event=event++;scene.Execute(Query(id,address,end_event,true));
                expected.push_back({id,2677,end_event,0x8877,address+32,address,draw_count});
                if(mode=="foreign"&&group==0){auto foreign=scene.ReadFullEdram();accepted.pending=false;foreign[100]^=0x5a;seed[100]^=0x5a;scene.InitializeEdram(foreign);}
            }
            if(scene.DeferredQueryCount()>=base.async_submission_capacity){scene.FlushOldestQuery();auto ready=scene.TakeCompletedQueries();completed.insert(completed.end(),ready.begin(),ready.end());}
            const uint32_t empty_address=0x1800000;scene.Execute(Query(generation,empty_address+32,event++,false));const auto empty_end=event++;scene.Execute(Query(generation,empty_address,empty_end,true));
            expected.push_back({generation,2677,empty_end,0x8877,empty_address+32,empty_address,0});
            scene.FlushGpuWork();accepted.pending=false;auto ready=scene.TakeCompletedQueries();completed.insert(completed.end(),ready.begin(),ready.end());
            Require(completed.size()==groups+1&&std::any_of(completed.begin(),completed.end(),[](const auto& q){return q.result.samples>0;})&&completed.back().result.samples==0&&completed.back().result.draws==0,"Depth actual positive/empty query identities missing");
            Require(completed.size()==expected.size(),"Depth query identity cardinality changed");
            for(size_t i=0;i<completed.size();++i){const auto& q=completed[i];Require(Json({q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws})==expected.at(i),"Depth exact expected query generation/frame/end/handle/addresses/draws changed");}
            out.memory=scene.ReadFullEdram();out.queries=Results(completed);out.report=scene.report();out.report["expected_query_identities"]=expected;
            for(uint32_t at=0;at<kDepthBytes;at+=4)Require(out.memory.at(at)==seed.at(at),"Depth writer altered preserved stencil bytes");
            if(mode!="mask")Require(std::equal(out.memory.begin()+kDepthBytes,out.memory.end(),seed.begin()+kDepthBytes),"Depth-only writer altered color or untouched physical words");
            const auto& stats=out.report.at("retained_fp24_depth_writer");
            const bool state_refusal=mode=="bias"||mode=="readonly"||mode=="forward"||mode=="format"||mode=="mask";
            if(enabled&&!state_refusal&&mode!="missing"&&mode!="budget")Require(stats.at("selected").get<uint64_t>()>0&&stats.at("selected")==stats.at("queried_selected")&&stats.at("restores")==stats.at("materializations")&&stats.at("source_refusals")==0&&stats.at("resource_refusals")==0,"Depth actual selected/conversion authority conservation failed");
            if(enabled&&(state_refusal||mode=="missing"||mode=="budget"))Require(stats.at("selected")==0&&stats.at("restores")==0,"Unproved/missing depth path selected hardware");
            if(enabled&&(mode=="missing"||mode=="partial"||mode=="budget"))Require(stats.at("residency_refusals").get<uint64_t>()>0,"Depth missing residency did not exercise ordinary Native refusal");
            if(enabled&&mode=="normal")Require(stats.at("prepared")==4&&stats.at("modules_created")==1&&stats.at("shader_switches").get<uint64_t>()>=2&&stats.at("reuses").get<uint64_t>()>0&&stats.at("maximum_run").get<uint64_t>()>=base.async_submission_capacity+8,"Depth retained source-switch/real ring-pressure continuity is vacuous");
            // Generic refusal may create ordinary PSOs for modified state. Stable
            // normal startup must create nothing while executing either mode.
            if(mode=="normal")Require(out.report.at("shader_cache").at("creations")==initial.at("shader_cache").at("creations")&&out.report.at("pipeline_cache").at("creations")==initial.at("pipeline_cache").at("creations"),"Depth normal gameplay compiled a new module/PSO");
        }
    }
    out.report["owned_destructor_returned"]=true;return out;
}
}
void RetainedFp24DepthSceneFixture(const SceneExecutorConfig& config,std::span<const LiveOperation* const> operations,
    const std::array<std::span<const uint32_t>,2>& vertex,std::span<const uint32_t> builtin){
    Require(config.retained_fp24_depth_writer&&config.native_fsi&&config.async_gpu_submissions&&config.unified_gpu_queue&&config.batch_transfers,"Depth Scene fixture requires isolated asynchronous Native startup");
    const auto donors=Donors(operations);Json rows=Json::array();
    for(const auto* mode:{"normal","missing","partial","budget","bias","readonly","forward","format","mask","foreign"}){
        const auto off=Run(config,donors,vertex,builtin,false,mode),on=Run(config,donors,vertex,builtin,true,mode);
        Require(off.memory==on.memory&&off.memory.size()==kBytes&&off.queries==on.queries,"Actual paired depth writer full10MiB/query identities or samples differ");
        rows.push_back({{"mode",mode},{"full_edram_sha256",ContentHash(on.memory)},{"full_edram_bytes",on.memory.size()},{"queries",on.queries},{"baseline",off.report},{"candidate",on.report},{"exact",true}});
    }
    auto owned=Run(config,donors,vertex,builtin,true,"owned");rows.push_back({{"mode","owned"},{"lifetime_only",true},{"candidate",owned.report}});
    Json witness={{"capacity",config.async_submission_capacity},{"paired_modes",10},{"owned_tail_returned",true},{"whole_capture_replay_executed",false},{"rows",std::move(rows)},
        {"scope","Identical OFF/ON CPU-owned actual shader/geometry/state donor clones with synthetic nontrivial full10MiB seed, real query identities/samples, original logical pressure, two-program retained continuity, Native-only negative admission and owned pending shutdown. Natural captured images/history parity remains separate."}};
    std::ofstream file(config.output/"retained-fp24-depth-scene-fixture.json");file<<witness.dump(2)<<'\n';Require(bool(file),"Cannot preserve depth Scene fixture witness");
}
}
