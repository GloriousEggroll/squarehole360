#include "retained_disjoint_query_scene_fixture.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/live_draw.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/retained_fp24_depth.h"
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
constexpr uint32_t kBytes=2048*5120,kPrimaryDepthBytes=720*5120;
constexpr uint32_t kSecondaryFirst=1440*5120,kSecondaryLast=1624*5120;
void Require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
uint32_t Get(std::span<const uint8_t> b,size_t at){return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
void Set(std::vector<uint8_t>& b,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)b.at(at+i)=uint8_t(value>>(24-8*i));}
void Number(std::vector<uint8_t>& b,size_t at,float value){Set(b,at,std::bit_cast<uint32_t>(value));}
CanonicalResourceRef Resource(std::vector<uint8_t> bytes){const auto digest=ContentHash(bytes);return {CanonicalDigest(digest),std::make_shared<const std::vector<uint8_t>>(std::move(bytes))};}
CanonicalResourceRef Floats(std::initializer_list<float> values){std::vector<uint8_t> b(values.size()*sizeof(float));std::memcpy(b.data(),values.begin(),b.size());return Resource(std::move(b));}
struct Input {const TypedLiveDraw* writer=nullptr;CanonicalResourceRef query_vertex;};
Input Donor(std::span<const LiveOperation* const> operations,bool fp24){
    Require(operations.size()<=65536,"Disjoint fixture operation bound");Input out;
    for(const auto* operation:operations){const auto* d=operation?operation->Draw():nullptr;if(!d)continue;
        if(d->vertex_shader&&d->vertex_shader->hash.View()==kReadonlyQueryVertexSource&&d->vertex_shader->owner)out.query_vertex=*d->vertex_shader;
        if(out.writer||!d->vertex_shader||!d->surfaces[4]||d->function!=DrawFunction::DrawIndexedPrimitive||d->args[1]!=4)continue;
        const auto& owner=d->lazy_device_state?d->lazy_device_state->Owner():d->device_state.owner;
        if(!owner||owner->size()!=LazyDeviceState::kBytes||Get(*owner,0x2934)!=0x700766||
           Get(*owner,0x2918)!=std::bit_cast<uint32_t>(-1.f)||Get(*owner,0x291c)!=std::bit_cast<uint32_t>(1.f))continue;
        const auto& depth=*d->surfaces[4];if(depth.width!=1280||depth.height!=720||(depth.surface_info&16383)!=1280||depth.edram_base||depth.format!=1||!depth.depth)continue;
        const auto cpu=[](const auto& g){return !g.gpu_memory&&bool(g.payload.owner);};
        if(!std::all_of(d->geometry.begin(),d->geometry.end(),cpu)||!std::all_of(d->up_geometry.begin(),d->up_geometry.end(),cpu))continue;
        if(fp24){if(d->pixel_shader||!NativeRetainedFp24DepthSourceIndex(d->vertex_shader->hash.View())||!d->textures.empty()||Get(*owner,0x28dc))continue;}
        else {
            if(!d->pixel_shader||!RetainedOpaqueCatalogPair(d->vertex_shader->hash.View(),d->pixel_shader->hash.View())||!d->surfaces[0]||Get(*owner,0x28dc)!=15)continue;
            const auto& c=*d->surfaces[0];if(c.width!=1280||c.height!=720||(c.surface_info&16383)!=1280||c.edram_base!=720||c.format!=3||c.depth)continue;
            if(!std::all_of(d->textures.begin(),d->textures.end(),[](const auto& t){return t.source==TextureSource::CpuSnapshot&&t.base_payload&&t.base_payload->owner&&(!t.mip_payload||t.mip_payload->owner);}))continue;
        }
        bool zero_bias=true;const auto mode=Get(*owner,0x2948);
        for(unsigned face=0;face<2;++face)if(mode&(1u<<(11+face)))for(unsigned c=0;c<2;++c)
            zero_bias&=std::bit_cast<float>(Get(*owner,0x2a50+face*8+c*4))==0.f;
        if(zero_bias)out.writer=d;
    }
    Require(out.writer&&out.query_vertex.owner,"Disjoint fixture lacks CPU-owned exact writer/query source donors");return out;
}
NativeFsiPipelineRecipe QueryRecipe(){return {std::string(kReadonlyQueryVertexSource),std::nullopt,{3,2,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};}
void Prewarm(SceneExecutor& scene,bool fp24,std::span<const RetainedOpaqueCatalogFixtureSource> sources,
    const std::array<std::span<const uint32_t>,2>& depth_vertex,std::span<const uint32_t> query_vertex,
    std::span<const uint32_t> builtin,bool query_resident){
    if(fp24){
        for(unsigned i=0;i<2;++i)for(uint32_t front:{1u,2u}){
            const NativeFsiPipelineRecipe recipe{std::string(kNativeRetainedFp24DepthSources.at(i).source),std::nullopt,{3,front,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            scene.PrewarmFsiPipeline(recipe,depth_vertex[i],builtin);
            Require(scene.PrewarmRetainedFp24DepthPipeline(recipe,depth_vertex[i],builtin),"Disjoint fixture depth startup refusal");
        }
    }else {
        Require(sources.size()==3,"Disjoint fixture requires all three authenticated catalogue sources");
        for(const auto& source:sources)for(uint32_t front:{1u,2u}){
            const NativeFsiPipelineRecipe recipe{std::string(source.pair.vertex),std::string(source.pair.pixel),{3,front,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
            scene.PrewarmFsiPipeline(recipe,source.vertex,source.pixel);
            Require(scene.PrewarmRetainedOpaquePipeline(recipe,source.vertex,source.pixel),"Disjoint fixture catalogue startup refusal");
        }
    }
    scene.PrewarmFsiPipeline(QueryRecipe(),query_vertex,builtin);
    if(query_resident)Require(scene.PrewarmNativeReadonlyDepthPipeline(QueryRecipe(),query_vertex,builtin),"Disjoint fixture D32 startup refusal");
}
TypedLiveDraw Writer(const Input& input,uint64_t event){auto d=*input.writer;d.event=event;return d;}
TypedLiveDraw QueryDraw(const Input& input,uint64_t event,bool positive,bool overlap,bool biased){
    TypedLiveDraw d;d.frame=input.writer->frame;d.event=event;d.function=DrawFunction::DrawPrimitive;d.args={0,4,0,3,0,0,0,0};d.vertex_shader=input.query_vertex;
    std::vector<uint8_t> state(LazyDeviceState::kBytes);
    for(unsigned i=0;i<4;++i)Number(state,0x780+16*i+4*i,1.f);
    Set(state,0x2880,640);Set(state,0x2888,(1u<<16)|(overlap?0u:1440u));Set(state,0x28bc,640u|(360u<<16));
    Set(state,0x28c4,0x80000000u);Set(state,0x28c8,640u|(360u<<16));Set(state,0x28dc,0);
    Set(state,0x2944,1u<<19);Set(state,0x294c,0x43f);Set(state,0x2948,(1u<<11)|(1u<<12)|2u);Set(state,0x29c0,1);
    Number(state,0x2908,320);Number(state,0x290c,320);Number(state,0x2910,-180);Number(state,0x2914,180);
    Number(state,0x2918,-1);Number(state,0x291c,1);Set(state,0x2934,0x700762);
    if(biased){Number(state,0x2a54,.125f);Number(state,0x2a5c,.125f);}
    d.device_state=Resource(std::move(state));d.surfaces[4]=TypedSurface{0,640,360,640,(1u<<16)|(overlap?0u:1440u),overlap?0u:1440u,1,true};
    const float clip=positive?0.f:1.f;
    const std::array<CanonicalResourceRef,3> resources{Resource({0,1,2,0}),Floats({0,0,0,1,1,1}),Floats({-1,-1,clip,3,-1,clip,-1,3,clip})};
    for(unsigned i=0;i<3;++i){TypedGeometry g;g.kind=14;g.address=0x7000;g.bytes=uint32_t(resources[i].owner->size());g.payload=resources[i];g.fetch_slot=93+i;g.endian=0;d.geometry.push_back(std::move(g));}
    d.vertex_fetch_slots={93,94,95};return d;
}
Json Query(uint64_t generation,uint32_t frame,uint32_t address,uint64_t event,bool end){return {{"type",end?"query_end":"query_begin"},{"query_generation",generation},{"query_handle",0x9955},{"report_address",address},{"frame",frame},{"event",event}};}
Json Identities(const std::vector<CompletedNativeQuery>& values){Json rows=Json::array();for(const auto& q:values)rows.push_back({q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});return rows;}
const Json& WriterStats(const Json& report,bool fp24){return report.at(fp24?"retained_fp24_depth_writer":"retained_opaque_catalog");}
void RequireUnavailable(SceneExecutor& scene,bool prefix,uint64_t generation,std::vector<CompletedNativeQuery>& completed){
    Require(scene.HasPendingGpuWork(),"Disjoint query lacks pending accepted-work frontier");const auto before=scene.report();const auto count=scene.DeferredQueryCount();size_t taken=0;
    if(prefix){auto ready=scene.TakeCompletedQueries();Require(std::none_of(ready.begin(),ready.end(),[&](const auto& q){return q.generation==generation;}),"Disjoint pending target query became ready early");taken=ready.size();completed.insert(completed.end(),ready.begin(),ready.end());}
    else {bool refused=false;try{(void)scene.TakeCompletedQueries();}catch(const std::runtime_error& e){if(std::string_view(e.what())!="Native query consumption before all GPU work completed")throw;refused=true;}Require(refused,"Disjoint allwork collector accepted pending work");}
    const auto after=scene.report();Require(taken<=count&&scene.DeferredQueryCount()==count-taken,"Disjoint readiness probe lost ticket identities");
    Require(before.at("gpu_work_queue").at("accepted_work")==after.at("gpu_work_queue").at("accepted_work")&&before.at("gpu_work_queue").at("completed_work")==after.at("gpu_work_queue").at("completed_work"),"Disjoint readiness probe changed accepted/completed frontier");
    Require(after.at("retained_disjoint_queries").at("last_end_required_work").get<uint64_t>()>after.at("gpu_work_queue").at("completed_work").get<uint64_t>(),"Disjoint empty target did not include prior unqueried restore/draw work");
}
struct AcceptedWork {bool pending=false;~AcceptedWork(){if(pending)std::terminate();}};
struct Outcome {std::vector<uint8_t> memory;Json queries,report;};
Outcome Run(const SceneExecutorConfig& base,const Input& input,std::span<const RetainedOpaqueCatalogFixtureSource> sources,
    const std::array<std::span<const uint32_t>,2>& depth_vertex,std::span<const uint32_t> query_vertex,
    std::span<const uint32_t> builtin,bool preserve,std::string_view mode){
    const bool fp24=base.retained_fp24_depth_writer,prefix=mode=="prefix";
    const bool negative=mode=="overlap"||mode=="bias"||mode=="missing";
    auto config=base;config.present={};config.retained_d32_composition=true;config.retain_primary_across_disjoint_queries=preserve;
    config.retire_completed_query_prefixes=prefix;config.max_deferred_queries=base.async_submission_capacity;config.max_draw_batch_size=16;
    config.output=base.output/(std::string("disjoint-")+(fp24?"depth-":"catalog-")+(preserve?"on-":"off-")+std::string(mode));
    Outcome out;std::vector<CompletedNativeQuery> completed;Json expected=Json::array(),expected_samples=Json::array();uint64_t event=1000000,generation=10000;const uint32_t frame=input.writer->frame;
    std::vector<uint8_t> seed(kBytes);uint32_t random=0x5ad4173;
    for(auto& byte:seed){random^=random<<13;random^=random>>17;random^=random<<5;byte=uint8_t(random);}
    for(uint32_t at=0;at<kPrimaryDepthBytes;at+=4)seed[at+1]=seed[at+2]=seed[at+3]=0;
    for(uint32_t at=kSecondaryFirst;at<kSecondaryLast;at+=4){seed[at]=0x5a;seed[at+1]=seed[at+2]=0;seed[at+3]=0xe0;}
    {
        SceneExecutor scene(std::move(config));AcceptedWork accepted;Require(scene.report().at("device").at("software")==false,"Disjoint fixture requires actual hardware");
        Prewarm(scene,fp24,sources,depth_vertex,query_vertex,builtin,mode!="missing");scene.InitializeEdram(seed);
        for(unsigned i=0;i<3;++i){scene.Execute(Writer(input,event++));accepted.pending=true;}
        auto initial=scene.report();const auto initial_stats=WriterStats(initial,fp24);
        Require(initial_stats.at("selected")==3&&initial_stats.at("restores")==1&&initial_stats.at("materializations")==0&&initial_stats.at("current_run")==3,"Disjoint fixture lacks one dirty actual three-writer primary");
        out.report["initial_dirty_primary"]=initial;
        const auto collect=[&]{auto values=scene.TakeCompletedQueries();completed.insert(completed.end(),values.begin(),values.end());};
        const auto query=[&](unsigned draws,bool positive,bool overlap,bool bias){
            const uint64_t id=++generation;const uint32_t address=0x1800000+uint32_t(generation-10000)*128;
            scene.Execute(Query(id,frame,address+32,event++,false));
            for(unsigned i=0;i<draws;++i){scene.Execute(QueryDraw(input,event++,positive,overlap,bias));accepted.pending=true;}
            const auto ended=event++;scene.Execute(Query(id,frame,address,ended,true));
            expected.push_back({id,frame,ended,0x9955,address+32,address,draws});
            expected_samples.push_back(overlap||bias?Json(nullptr):Json(draws?(positive?640u*360u:0u):0u));return id;
        };
        // The first real EMPTY query follows an unqueried restore and three
        // writers. Pending readiness requires all preceding work, not only
        // physical slots allocated inside this zero-draw query.
        const auto empty=query(0,false,false,false);
        if(preserve)RequireUnavailable(scene,prefix,empty,completed);
        if(preserve)scene.CompleteGpuWorkForQueryCompletion();else scene.FlushGpuWork();accepted.pending=false;collect();
        const auto fenced=scene.report();out.report["empty_query_full_fence"]=fenced;
        Require(!scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==0&&completed.size()==1&&completed[0].result.draws==0&&completed[0].result.samples==0,"Disjoint empty query full-fence identity/readiness changed");
        if(preserve)Require(WriterStats(fenced,fp24).at("materializations")==0&&WriterStats(fenced,fp24).at("current_run")==3&&fenced.at("retained_disjoint_queries").at("dirty_query_completions")==1,"Fenced-but-dirty primary was exported or lost during query-only completion");
        // Reuse must work with NO pending work. Observing HasPendingGpuWork
        // false alone must not discard the representation's dirty authority.
        scene.Execute(Writer(input,event++));accepted.pending=true;
        const auto continued=scene.report();out.report["writer_after_query_fence"]=continued;
        if(preserve)Require(WriterStats(continued,fp24).at("restores")==1&&WriterStats(continued,fp24).at("reuses")==3,"Fenced dirty writer continuation unnecessarily restored canonical bytes");
        query(1,true,mode=="overlap",mode=="bias");
        if(!negative)query(1,false,false,false);
        const auto middle=scene.report();out.report["after_controlled_query_draws"]=middle;
        if(preserve&&!negative)Require(middle.at("retained_disjoint_queries").at("accepted_draws")==2&&WriterStats(middle,fp24).at("materializations")==0,"Disjoint actual separate D32 query did not retain primary");
        if(preserve&&negative)Require(middle.at("retained_disjoint_queries").at("accepted_draws")==0&&WriterStats(middle,fp24).at("materializations")==1,"Unproved/missing D32 query bypassed strong primary materialization");
        if(mode=="missing")Require(middle.at("native_readonly_depth_attachment").at("candidates").get<uint64_t>()>0&&
            middle.at("native_readonly_depth_attachment").at("residency_refusals").get<uint64_t>()>0&&middle.at("native_readonly_depth_attachment").at("selected")==0,
            "Disjoint missing startup mode did not reach an actual missing D32 residency refusal");
        if(mode=="strong"){
            const auto before=WriterStats(middle,fp24).at("materializations").get<uint64_t>();const auto canonical=scene.ReadFullEdram();accepted.pending=false;
            Require(canonical.size()==kBytes&&WriterStats(scene.report(),fp24).at("materializations").get<uint64_t>()==before+(preserve?1u:0u),"Public full EDRAM read did not commit dirty primary");collect();
            scene.Execute(Writer(input,event++));accepted.pending=true;out.report["after_strong_read_and_fresh_writer"]=scene.report();
            if(preserve)Require(WriterStats(out.report.at("after_strong_read_and_fresh_writer"),fp24).at("restores")==2,"Writer reused an exported generation after public CPU read");
        }
        if(mode=="owned"){
            scene.Execute(Writer(input,event++));accepted.pending=true;
            out.report["pending_owned_tail"]=scene.report();
            Require(preserve&&scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==2&&WriterStats(out.report.at("pending_owned_tail"),fp24).at("materializations")==0&&WriterStats(out.report.at("pending_owned_tail"),fp24).at("current_run")==5,"Disjoint owned tail lacks dirty writer/query work");
            out.report["pending_owned_query_count"]=scene.DeferredQueryCount();out.report["expected_query_identities"]=expected;out.report["expected_query_samples"]=expected_samples;
            accepted.pending=false; // Owned Scene destructor must export/fence all resources.
        }else {
            if(!negative&&mode!="strong") {
                uint32_t pressure_queries=0,pressure_flushes=0,maximum_deferred=scene.DeferredQueryCount();
                Json pressure_witnesses=Json::array();
                for(uint32_t i=0;i<base.async_submission_capacity+8;++i){
                    if(scene.DeferredQueryCount()>=base.async_submission_capacity){
                        const auto before=scene.report();const auto deferred_before=scene.DeferredQueryCount();const auto identities_before=completed.size();
                        scene.FlushOldestQuery();collect();++pressure_flushes;
                        Require(scene.DeferredQueryCount()<deferred_before&&completed.size()>identities_before,"Disjoint real capacity pressure did not retire actual query identities");
                        pressure_witnesses.push_back({{"before",before},{"after",scene.report()},{"deferred_before",deferred_before},
                            {"deferred_after",scene.DeferredQueryCount()},{"completed_before",identities_before},{"completed_after",completed.size()}});
                    }
                    query(1,(i&1)==0,false,false);++pressure_queries;maximum_deferred=std::max(maximum_deferred,scene.DeferredQueryCount());
                    scene.Execute(Writer(input,event++));accepted.pending=true;
                }
                Require(pressure_queries==base.async_submission_capacity+8&&pressure_flushes>0&&maximum_deferred==base.async_submission_capacity,
                    "Disjoint real logical/physical capacity pressure witness is vacuous");
                out.report["capacity_pressure"]={{"logical_capacity",base.async_submission_capacity},{"physical_capacity",base.async_submission_capacity},
                    {"query_draws",pressure_queries},{"flushes",pressure_flushes},{"maximum_deferred",maximum_deferred},{"witnesses",std::move(pressure_witnesses)},{"after_tail",scene.report()}};
                if(preserve)Require(WriterStats(scene.report(),fp24).at("restores")==1&&WriterStats(scene.report(),fp24).at("materializations")==0&&
                    WriterStats(scene.report(),fp24).at("current_run")==base.async_submission_capacity+12&&
                    scene.report().at("retained_disjoint_queries").at("accepted_draws")==base.async_submission_capacity+10,
                    "Disjoint capacity pressure lost the original dirty primary run or separate D32 query coverage");
            }
            if(preserve)scene.CompleteGpuWorkForQueryCompletion();else scene.FlushGpuWork();accepted.pending=false;collect();
            Require(scene.DeferredQueryCount()==0&&!scene.HasPendingGpuWork(),"Disjoint full query completion left pending accepted work/tickets");
            Require(completed.size()==expected.size(),"Disjoint fixture lost actual query identity count");
            for(size_t i=0;i<completed.size();++i){const auto& q=completed[i];
                Require(Json({q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws})==expected.at(i),"Disjoint query identity/frame/generation/address/draw count changed");
                if(!expected_samples.at(i).is_null())Require(q.result.samples==expected_samples.at(i).get<uint64_t>(),"Disjoint exact full-screen positive230400/negative0/empty0 sample oracle mismatch");
            }
            out.report["before_strong_final_read"]=scene.report();out.memory=scene.ReadFullEdram();out.queries=Identities(completed);out.report["final"]=scene.report();out.report["expected_query_identities"]=expected;out.report["expected_query_samples"]=expected_samples;
            Require(out.memory.size()==kBytes,"Disjoint full memory oracle extent");
            Require(std::equal(seed.begin()+kSecondaryFirst,seed.end(),out.memory.begin()+kSecondaryFirst),"Readonly query changed secondary or untouched packed words");
            for(uint32_t at=0;at<kPrimaryDepthBytes;at+=4)Require(out.memory[at]==seed[at],"Disjoint writer/query modified primary stencil");
            if(fp24)Require(std::equal(seed.begin()+kPrimaryDepthBytes,seed.begin()+kSecondaryFirst,out.memory.begin()+kPrimaryDepthBytes),"Depth-only retained path modified primary color words");
        }
    }
    out.report["owned_destructor_returned"]=true;out.report["seed_sha256"]=ContentHash(seed);out.report["seed_bytes"]=seed.size();return out;
}
}
void RetainedDisjointQuerySceneFixture(const SceneExecutorConfig& config,std::span<const LiveOperation* const> operations,
    std::span<const RetainedOpaqueCatalogFixtureSource> sources,const std::array<std::span<const uint32_t>,2>& depth_vertex,
    std::span<const uint32_t> query_vertex,std::span<const uint32_t> builtin){
    Require(config.retained_d32_composition&&config.retain_primary_across_disjoint_queries&&config.native_fsi&&config.native_readonly_depth_attachment&&config.defer_native_queries&&
        config.async_gpu_submissions&&config.unified_gpu_queue&&config.batch_transfers&&
        (config.retained_fp24_depth_writer!=config.retained_opaque_catalog)&&!config.retained_opaque_writer,"Disjoint fixture needs exactly catalogueV2 or FP24 writer and canonical D32");
    const auto input=Donor(operations,config.retained_fp24_depth_writer);Json rows=Json::array();
    const auto& state=input.writer->lazy_device_state?input.writer->lazy_device_state->Owner():input.writer->device_state.owner;
    Json donor={{"frame",input.writer->frame},{"event",input.writer->event},{"vertex_source",input.writer->vertex_shader->hash.View()},
        {"pixel_source",input.writer->pixel_shader?Json(input.writer->pixel_shader->hash.View()):Json(nullptr)},
        {"device_state_sha256",ContentHash(*state)},{"device_state_bytes",state->size()},
        {"query_vertex_source",input.query_vertex.hash.View()},{"query_vertex_payload_sha256",ContentHash(*input.query_vertex.owner)}};
    for(const auto* mode:{"normal","prefix","missing","overlap","bias","strong"}){
        const auto off=Run(config,input,sources,depth_vertex,query_vertex,builtin,false,mode);
        const auto on=Run(config,input,sources,depth_vertex,query_vertex,builtin,true,mode);
        Require(off.memory==on.memory&&off.memory.size()==kBytes&&off.queries==on.queries,"Disjoint paired full10MiB actual query samples/identities mismatch");
        rows.push_back({{"mode",mode},{"exact",true},{"full_edram_bytes",on.memory.size()},{"full_edram_sha256",ContentHash(on.memory)},{"queries",on.queries},{"baseline",off.report},{"candidate",on.report}});
    }
    const auto owned=Run(config,input,sources,depth_vertex,query_vertex,builtin,true,"owned");
    rows.push_back({{"mode","owned"},{"lifetime_only",true},{"candidate",owned.report}});
    Json report={{"schema",1},{"writer_mode",config.retained_fp24_depth_writer?"fp24":"catalog-v2"},{"capacity",config.async_submission_capacity},
        {"paired_modes",6},{"whole_capture_replay_executed",false},{"donor",std::move(donor)},{"rows",std::move(rows)},
        {"scope","Controlled typed CPU-owned captured writer/query-source donors, explicitly synthetic geometry/query controls and nontrivial full10MiB seed; exact OFF/ON strong-composition baseline, actual empty/positive/negative samples, logical/physical pressure, query-only full-fence dirty authority, overlap/refusal/public-read and pending owned lifetime. Natural overlap/live benefit remain separate."}};
    std::ofstream file(config.output/"retained-disjoint-query-scene-fixture.json");file<<report.dump(2)<<'\n';Require(bool(file),"Cannot preserve disjoint Scene fixture witness");
}
}
