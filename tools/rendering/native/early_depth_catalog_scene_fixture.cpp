#include "early_depth_catalog_scene_fixture.h"
#include "renderer/native/native_early_depth_catalog.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/shader_compiler.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/content_hash.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
constexpr uint32_t kBytes=2048u*5120,kDepthBytes=720u*5120;
constexpr uint32_t kThrough=1395;
constexpr std::array<uint64_t,3> kDonorEvents{1215,883,745};
void Require(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
uint32_t Word(std::span<const uint8_t> b,size_t at) {
    Require(at+4<=b.size(),"Catalogue fixture state word extent");
    return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
uint64_t Count(const Json& report,std::string_view name,std::string_view field) {
    return report.at(std::string(name)).at(std::string(field)).get<uint64_t>();
}
uint64_t Difference(const Json& before,const Json& after,std::string_view name,std::string_view field) {
    const auto a=Count(before,name,field),b=Count(after,name,field);
    Require(b>=a,"Catalogue fixture counter went backwards");return b-a;
}
uint64_t SourceCount(const Json& report,unsigned index) {
    const auto& row=report.at("native_early_depth_catalog").at("sources").at(index);
    Require(row.at("pixel")==kNativeEarlyDepthCatalogPixelSources.at(index),"Catalogue fixture source counter identity");
    return row.at("selected").get<uint64_t>();
}
std::vector<uint8_t> Read(const std::filesystem::path& p,size_t maximum) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);Require(bool(f),"Catalogue fixture artifact open");
    const auto size=f.tellg();Require(size>=0&&uint64_t(size)<=maximum,"Catalogue fixture artifact extent");
    std::vector<uint8_t> result(static_cast<size_t>(size));f.seekg(0);
    if(!result.empty())f.read(reinterpret_cast<char*>(result.data()),std::streamsize(result.size()));
    Require(bool(f),"Catalogue fixture artifact read");return result;
}
void Write(const std::filesystem::path& p,std::span<const uint8_t> bytes) {
    std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    Require(bool(f),"Catalogue fixture artifact write");
}
struct Donors {
    std::array<TypedLiveDraw,3> draws;
    CanonicalResourceRef query_vertex;
    std::vector<uint32_t> query_vertex_spirv,query_builtin_spirv;
    Json resolve,metadata,history;
};
Donors Inputs(const SceneExecutorConfig& config,std::span<const LiveOperation* const> operations) {
    Require(operations.size()<=65536,"Catalogue fixture operation bound");Donors out;
    std::array<bool,3> found{};bool resolve=false;
    for(const auto* operation:operations) {
        Require(operation,"Catalogue fixture null operation");
        if(const auto* d=operation->Draw())if(d->vertex_shader&&d->vertex_shader->hash.View()==kReadonlyQueryVertexSource&&d->vertex_shader->owner)
            out.query_vertex=*d->vertex_shader;
        if(const auto* d=operation->Draw())for(unsigned i=0;i<3;++i)if(d->event==kDonorEvents[i]) {
            Require(!found[i],"Catalogue fixture duplicate donor event");found[i]=true;out.draws[i]=*d;
        }
        if(!resolve&&operation->Kind()=="resolve") {
            const auto& c=operation->Control();const auto slot=c.at("source_slot").get<unsigned>();
            const auto& surface=c.at("surfaces").at(std::to_string(6+slot));
            if(slot==0&&surface.at("edram_base")==720&&surface.at("width")==1280&&surface.at("height")==720&&
               c.at("source_rectangle")==Json({0,0,1280,720})&&c.at("destination_rectangle")==Json({0,0,1280,720})) {
                out.resolve=c;resolve=true;
            }
        }
    }
    Require(std::all_of(found.begin(),found.end(),[](bool b){return b;})&&resolve,"Catalogue fixture lacks fixed authentic donor/resolve inputs");
    out.metadata=Json::array();
    const auto frame=out.draws[0].frame;
    for(unsigned i=0;i<3;++i) {
        const auto& d=out.draws[i];Require(d.frame==frame&&d.event<=kThrough&&d.vertex_shader&&d.pixel_shader&&
            d.vertex_shader->owner&&d.pixel_shader->owner&&!d.rectangle_list&&
            d.function==DrawFunction::DrawIndexedPrimitive&&d.args[1]==4,"Catalogue fixture donor draw/source contract");
        const bool pair=i==0?(d.vertex_shader->hash.View()==kNativeEarlyDepthVertexSource&&d.pixel_shader->hash.View()==kNativeEarlyDepthPixelSource):
            (d.vertex_shader->hash.View()==kNativeEarlyDepthCatalogVertexSource&&d.pixel_shader->hash.View()==kNativeEarlyDepthCatalogPixelSources[i-1]);
        Require(pair&&ContentHash(*d.vertex_shader->owner)==d.vertex_shader->hash.View()&&
            ContentHash(*d.pixel_shader->owner)==d.pixel_shader->hash.View(),"Catalogue fixture canonical donor shader payload identity");
        const auto& owner=d.lazy_device_state?d.lazy_device_state->Owner():d.device_state.owner;
        Require(owner&&owner->size()==LazyDeviceState::kBytes&&Word(*owner,0x2934)==0x700766&&
            !(Word(*owner,0x2944)&(1u<<16))&&Word(*owner,0x2918)==std::bit_cast<uint32_t>(-1.f)&&
            Word(*owner,0x291c)==std::bit_cast<uint32_t>(1.f),"Catalogue fixture exact GE/clip/unit viewport contract");
        for(unsigned face=0;face<2;++face)if(Word(*owner,0x2948)&(1u<<(11+face)))for(unsigned component=0;component<2;++component)
            Require(std::bit_cast<float>(Word(*owner,0x2a50+face*8+component*4))==0.f,"Catalogue fixture effective nonzero depth bias");
        Require(d.surfaces[4]&&d.surfaces[0],"Catalogue fixture missing primary surfaces");
        const auto& depth=*d.surfaces[4];const auto& color=*d.surfaces[0];
        Require(depth.depth&&depth.width==1280&&depth.height==720&&(depth.surface_info&16383)==1280&&
            !(depth.surface_info&0x30000)&&depth.edram_base==0&&depth.format==1&&
            !color.depth&&color.width==1280&&color.height==720&&(color.surface_info&16383)==1280&&
            !(color.surface_info&0x30000)&&color.edram_base==720&&color.format==3,"Catalogue fixture exact primary footprints");
        for(unsigned slot=1;slot<4;++slot)Require(!d.surfaces[slot],"Catalogue fixture secondary color target");
        Json geometry=Json::array(),textures=Json::array();
        const auto owned=[&](const auto& g) {
            Require(!g.gpu_memory&&g.payload.owner&&g.bytes==g.payload.owner->size()&&
                ContentHash(*g.payload.owner)==g.payload.hash.View(),"Catalogue fixture CPU geometry ownership/hash");
            geometry.push_back({{"kind",g.kind},{"bytes",g.bytes},{"source",g.payload.hash.View()}});
        };
        for(const auto& g:d.geometry)owned(g);
        for(const auto& g:d.up_geometry)owned(g);
        Require(!geometry.empty(),"Catalogue fixture empty captured geometry");
        for(const auto& t:d.textures) {
            if(t.source==TextureSource::CpuSnapshot)Require(t.base_payload&&t.base_payload->owner&&
                ContentHash(*t.base_payload->owner)==t.base_payload->hash.View()&&
                (!t.mip_payload||(t.mip_payload->owner&&ContentHash(*t.mip_payload->owner)==t.mip_payload->hash.View())),
                "Catalogue fixture captured CPU texture ownership/hash");
            textures.push_back({{"slot",t.slot},{"source",uint32_t(t.source)},{"fetch",t.fetch}});
        }
        out.metadata.push_back({{"event",d.event},{"frame",d.frame},{"vertex",d.vertex_shader->hash.View()},
            {"pixel",d.pixel_shader->hash.View()},{"device_state_sha256",ContentHash(*owner)},
            {"geometry",std::move(geometry)},{"textures",std::move(textures)}});
    }
    std::ifstream file(config.capture/"gpu-history.json");file>>out.history;
    Require(bool(file)&&out.history.at("provenance")=="reference_gpu_frame_boundary"&&
        !out.history.at("edram").is_null()&&out.history.at("edram").at("bytes")==kBytes,
        "Catalogue fixture requires authenticated full captured EDRAM history");
    Require(out.query_vertex.owner&&ContentHash(*out.query_vertex.owner)==kReadonlyQueryVertexSource,
        "Catalogue fixture lacks authentic readonly query vertex source owner");
    // Explicit fixture preparation uses the real captured source and the same
    // canonical builtin compiler. It never supplies a fabricated shader ID.
    NativeShaderCompiler compiler;
    Require(compiler.CacheVersion()==kNativeEarlyDepthCanonicalVersion,"Catalogue query canonical compiler version changed");
    out.query_vertex_spirv=compiler.Get(*out.query_vertex.owner,NativeShaderMode::Raster)->spirv;
    out.query_builtin_spirv=compiler.GetDepthOnlyFragmentInterlock()->spirv;
    Require(AuditedReadonlyQueryVertex(kReadonlyQueryVertexSource,
        {reinterpret_cast<const uint8_t*>(out.query_vertex_spirv.data()),out.query_vertex_spirv.size()*sizeof(uint32_t)})&&
        ContentHash({reinterpret_cast<const uint8_t*>(out.query_builtin_spirv.data()),out.query_builtin_spirv.size()*sizeof(uint32_t)})==kReadonlyQueryBuiltinSpirv,
        "Catalogue positive query canonical module certificate refused");
    return out;
}
void Store(std::vector<uint8_t>& b,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=uint8_t(value>>(24-8*i));
}
CanonicalResourceRef Resource(std::vector<uint8_t> b) {
    const auto hash=ContentHash(b);return {CanonicalDigest(hash),std::make_shared<const std::vector<uint8_t>>(std::move(b))};
}
CanonicalResourceRef Floats(std::initializer_list<float> values) {
    std::vector<uint8_t> b(values.size()*sizeof(float));std::memcpy(b.data(),values.begin(),b.size());return Resource(std::move(b));
}
TypedLiveDraw PositiveQueryProbe(const Donors& input,uint64_t event) {
    // The already-proved Scene query fixture's oversized full-screen triangle,
    // identity transform and original pinned VS/builtin. Reversed unit viewport
    // maps clipz0 to exact depth1, which GE-passes every finite [0,1] primary word.
    TypedLiveDraw d;d.frame=input.draws[0].frame;d.event=event;d.function=DrawFunction::DrawPrimitive;
    d.args={0,4,0,3,0,0,0,0};d.vertex_shader=input.query_vertex;
    std::vector<uint8_t> state(LazyDeviceState::kBytes);
    const auto number=[&](size_t at,float value){Store(state,at,std::bit_cast<uint32_t>(value));};
    for(unsigned i=0;i<4;++i)number(0x780+16*i+4*i,1.f);
    Store(state,0x2880,1280);Store(state,0x2888,1u<<16);Store(state,0x28bc,1280u|(720u<<16));
    Store(state,0x28c4,0x80000000u);Store(state,0x28c8,1280u|(720u<<16));Store(state,0x28dc,0);
    Store(state,0x2944,1u<<19);Store(state,0x294c,0x43f);Store(state,0x2948,(1u<<11)|(1u<<12)|2u);Store(state,0x29c0,1);
    number(0x2908,640);number(0x290c,640);number(0x2910,-360);number(0x2914,360);
    number(0x2918,-1);number(0x291c,1);Store(state,0x2934,0x700762);
    d.device_state=Resource(std::move(state));d.surfaces[4]=TypedSurface{0,1280,720,1280,1u<<16,0,1,true};
    const std::array<CanonicalResourceRef,3> resources{Resource({0,1,2,0}),Floats({0,0,0,1,1,1}),Floats({-1,-1,0,3,-1,0,-1,3,0})};
    for(unsigned i=0;i<3;++i) {
        TypedGeometry g;g.kind=14;g.address=0x7000;g.bytes=uint32_t(resources[i].owner->size());g.payload=resources[i];
        g.fetch_slot=93+i;g.endian=0;d.geometry.push_back(std::move(g));
    }
    d.vertex_fetch_slots={93,94,95};return d;
}
void Prewarm(SceneExecutor& scene,std::span<const EarlyDepthCatalogSceneInput> inputs,bool catalog,std::string_view mode) {
    Require(!inputs.empty()&&inputs.size()<=8,"Catalogue fixture observed recipe count");
    std::array<bool,3> generic{},optional{};
    for(const auto& p:inputs) {
        Require(p.recipe.pixel&&p.program&&!p.vertex.empty()&&!p.pixel.empty(),"Catalogue fixture incomplete actual compiler input");
        unsigned i=3;
        if(p.recipe.vertex==kNativeEarlyDepthVertexSource&&*p.recipe.pixel==kNativeEarlyDepthPixelSource)i=0;
        for(unsigned j=0;j<2;++j)if(NativeEarlyDepthCatalogPair(p.recipe.vertex,*p.recipe.pixel)&&*p.recipe.pixel==kNativeEarlyDepthCatalogPixelSources[j])i=j+1;
        Require(i<3,"Catalogue fixture unpinned observed recipe");
        scene.PrewarmFsiPipeline(p.recipe,p.vertex,p.pixel);generic[i]=true;
    }
    Require(std::all_of(generic.begin(),generic.end(),[](bool v){return v;}),"Catalogue fixture missing original pair recipes");
    for(const auto& p:inputs) {
        const auto& pixel=*p.recipe.pixel;
        if(pixel==kNativeEarlyDepthPixelSource) {
            Require(scene.PrewarmNativeEarlyDepthPipeline(p.recipe,p.vertex,p.pixel,p.program),"Catalogue fixture V1 startup refused");optional[0]=true;
        } else if(catalog&&mode!="missing"&&
            (mode!="partial-a3"||pixel==kNativeEarlyDepthCatalogPixelSources[0])&&
            (mode!="partial-55"||pixel==kNativeEarlyDepthCatalogPixelSources[1])) {
            Require(scene.PrewarmNativeEarlyDepthCatalogPipeline(p.recipe,p.vertex,p.pixel,p.program),"Catalogue fixture V2 startup refused");
            optional[pixel==kNativeEarlyDepthCatalogPixelSources[0]?1:2]=true;
        }
    }
    Require(optional[0],"Catalogue fixture V1 was not prepared");
    if(catalog) {
        const auto& r=scene.report().at("native_early_depth_catalog");
        const unsigned expected=mode=="missing"?0u:(mode=="partial-a3"||mode=="partial-55")?1u:2u;
        Require(r.at("modules")==expected&&r.at("total_prepared").get<uint64_t>()<=8&&
            r.at("module_bytes").get<uint64_t>()<=262144&&Count(scene.report(),"native_early_depth","module_bytes")<=131072,
            "Catalogue fixture optional startup bounds/source count");
    }
}
Json Query(uint64_t generation,uint32_t frame,uint32_t address,uint64_t event,bool end) {
    return {{"type",end?"query_end":"query_begin"},{"query_generation",generation},{"query_handle",0x9966},
        {"report_address",address},{"frame",frame},{"event",event}};
}
Json Identities(const std::vector<CompletedNativeQuery>& values) {
    auto result=Json::array();for(const auto& q:values)result.push_back({q.generation,q.frame,q.end_event,q.result.handle,
        q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});return result;
}
struct Outcome {std::vector<uint8_t> memory,image;Json queries,report;std::filesystem::path output;};
Outcome Run(const SceneExecutorConfig& base,std::span<const LiveOperation* const> operations,
        std::span<const EarlyDepthCatalogSceneInput> inputs,const Donors& donors,bool catalog,std::string_view mode) {
    auto config=base;config.native_early_depth=true;config.native_early_depth_catalog=catalog;config.present={};
    config.max_draw_batch_size=16;config.max_deferred_queries=base.async_submission_capacity;
    config.multi_draw_command_buffers=false;config.retire_completed_query_prefixes=mode=="query";
    config.debug_raw_images=false;config.output=base.output/(std::string("early-catalog-")+(catalog?"on-":"off-")+std::string(mode));
    std::filesystem::create_directories(config.output);Outcome out;out.output=config.output;
    const auto frame=donors.draws[0].frame;uint64_t event=1000000,generation=20000;
    std::vector<CompletedNativeQuery> completed;std::vector<uint8_t> replacement_seed;Json expected=Json::array();
    {
        SceneExecutor scene(std::move(config));const auto device=scene.report().at("device");
        Require(device.at("software")==false,"Catalogue fixture requires actual hardware");out.report["device"]=device;
        Prewarm(scene,inputs,catalog,mode);
        if(mode=="query")scene.PrewarmFsiPipeline({std::string(kReadonlyQueryVertexSource),std::nullopt,
            {3,2,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1},donors.query_vertex_spirv,donors.query_builtin_spirv);
        scene.RunLiveCapture(operations,kThrough);
        Require(scene.report().at("initial_gpu_history").at("frame")==donors.history.at("frame"),"Catalogue fixture captured history was not imported");
        // No query is active at the fixed prefix boundary. The public strong
        // close also starts the controlled tail with a fresh anchor in BOTH arms.
        scene.FlushCommittedState();scene.FlushPendingDraws();
        Require(!scene.ActiveQueryMetadata(),"Catalogue fixture prefix has an active query");
        out.report["prefix"]=scene.report();
        const auto collect=[&] {auto values=scene.TakeCompletedQueries();completed.insert(completed.end(),values.begin(),values.end());};
        collect();Require(completed.empty(),"Catalogue fixture prefix unexpectedly contained query tickets");
        const auto draw=[&](unsigned index) {auto d=donors.draws.at(index);d.event=event++;scene.Execute(d);};
        const auto mixed=[&] {draw(0);draw(1);draw(2);draw(0);};
        const auto check_mixed=[&](const Json& before,const Json& after,bool fresh) {
            Require(Difference(before,after,"native_early_depth","selected")==2,"Catalogue fixture V1 controlled selection is not two");
            if(!catalog)return;
            const bool full=mode!="missing"&&mode!="partial-a3"&&mode!="partial-55";
            const unsigned selected=mode=="missing"?0u:full?2u:1u;
            Require(Difference(before,after,"native_early_depth_catalog","candidates")==2&&
                Difference(before,after,"native_early_depth_catalog","selected")==selected&&
                Difference(before,after,"native_early_depth_catalog","residency_refusals")==2-selected,
                "Catalogue fixture actual selected/missing residency delta");
            for(unsigned i=0;i<2;++i)Require(SourceCount(after,i)-SourceCount(before,i)==
                unsigned(full||(mode=="partial-a3"&&i==0)||(mode=="partial-55"&&i==1)),"Catalogue fixture per-source selection delta");
            if(full)Require(Difference(before,after,"native_early_depth","conversions")==unsigned(fresh)&&
                Difference(before,after,"native_early_depth","cache_hits")+Difference(before,after,"native_early_depth_catalog","cache_hits")==4-unsigned(fresh)&&
                Difference(before,after,"native_early_depth_catalog","shader_switches")==3&&
                Difference(before,after,"native_early_depth_catalog","invalidations")==0,
                "Catalogue fixture did not reuse one actual immutable snapshot across three program switches");
        };
        const auto before=scene.report();mixed();const auto initial=scene.report();check_mixed(before,initial,true);
        out.report["controlled_mixed"]={{"before",before},{"after",initial}};
        if(mode=="fenced") {
            scene.FlushPendingDraws();Require(!scene.HasPendingGpuWork(),"Catalogue fixture full fence did not retire accepted work");
            const auto fenced=scene.report();mixed();check_mixed(fenced,scene.report(),false);
            out.report["fenced_continuation"]={{"before",fenced},{"after",scene.report()}};
        } else if(mode=="strong"||mode=="reinit") {
            const auto snapshot=scene.ReadFullEdram();Require(snapshot.size()==kBytes,"Catalogue fixture strong read extent");
            out.report["strong_snapshot_sha256"]=ContentHash(snapshot);
            const auto invalidated=scene.report();
            if(catalog)Require(Difference(initial,invalidated,"native_early_depth_catalog","invalidations")==1,"Catalogue fixture strong read retained stale snapshot");
            if(mode=="reinit") {
                auto replacement=snapshot;
                for(uint32_t at=0;at<kDepthBytes;at+=4)replacement[at+1]=replacement[at+2]=replacement[at+3]=0;
                replacement.back()^=0x5a;scene.InitializeEdram(replacement);
                out.report["replacement_sha256"]=ContentHash(replacement);
                replacement_seed=std::move(replacement);
            }
            const auto fresh=scene.report();mixed();check_mixed(fresh,scene.report(),true);
            out.report["fresh_after_strong_mutation"]={{"before",fresh},{"after",scene.report()}};
        } else if(mode=="query") {
            // A deliberate full EDRAM mutation makes positive geometry a real
            // query witness rather than relying on a naturally occluded room.
            auto replacement=scene.ReadFullEdram();
            for(uint32_t at=0;at<kDepthBytes;at+=4)replacement[at+1]=replacement[at+2]=replacement[at+3]=0;
            scene.InitializeEdram(replacement);out.report["query_seed_sha256"]=ContentHash(replacement);
            replacement_seed=std::move(replacement);
            const auto query_before=scene.report();uint32_t maximum_deferred=0,pressure_flushes=0;Json pressure=Json::array();
            const auto query=[&](bool empty,bool material) {
                const auto id=++generation;const uint32_t address=0x1e00000u+uint32_t(id-20000)*128;
                scene.Execute(Query(id,frame,address+32,event++,false));
                if(material)mixed();
                if(!empty)scene.Execute(PositiveQueryProbe(donors,event++));
                const auto end=event++;scene.Execute(Query(id,frame,address,end,true));
                expected.push_back({id,frame,end,0x9966,address+32,address,empty?0u:material?5u:1u});
                maximum_deferred=std::max(maximum_deferred,scene.DeferredQueryCount());
            };
            query(true,false);query(false,true);
            Require(scene.HasPendingGpuWork(),"Catalogue fixture empty/nonempty tail lacks pending accepted work");
            auto ready=scene.TakeCompletedQueries();
            Require(std::none_of(ready.begin(),ready.end(),[&](const auto& q){return q.generation==generation;}),"Catalogue pending query became ready before its all-work frontier");
            completed.insert(completed.end(),ready.begin(),ready.end());out.report["pending_query_frontier"]=scene.report();
            for(uint32_t i=0;i<base.async_submission_capacity+8;++i) {
                if(scene.DeferredQueryCount()>=base.async_submission_capacity) {
                    const auto deferred=scene.DeferredQueryCount();const auto identities=completed.size();const auto prior=scene.report();
                    scene.FlushOldestQuery();collect();++pressure_flushes;
                    Require(scene.DeferredQueryCount()<deferred&&completed.size()>identities,"Catalogue actual query pressure did not retire identities");
                    pressure.push_back({{"before",prior},{"after",scene.report()},{"deferred_before",deferred},
                        {"deferred_after",scene.DeferredQueryCount()},{"completed_before",identities},{"completed_after",completed.size()}});
                }
                query(false,false);
            }
            Require(maximum_deferred==base.async_submission_capacity&&pressure_flushes>0,"Catalogue query capacity pressure was vacuous");
            const auto queried=scene.report();
            Require(Difference(query_before,queried,"native_early_depth","selected")==0,"Active queries selected V1 early rejection");
            if(catalog)Require(Difference(query_before,queried,"native_early_depth_catalog","selected")==0&&
                queried.at("native_early_depth_catalog").at("queried_selected")==0,"Active queries selected catalogue early rejection");
            scene.FlushGpuWork();collect();
            Require(completed.size()==expected.size()&&scene.DeferredQueryCount()==0,"Catalogue query identities were lost at full completion");
            uint64_t positive=0;
            for(size_t i=0;i<completed.size();++i) {
                const auto& q=completed[i];Require(Json({q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws})==expected.at(i),"Catalogue exact query publication identity changed");
                if(q.result.draws) {
                    // Genuine material coverage may legitimately be zero.
                    // The independent fullscreen probe remains strictly positive:
                    // exactly921600 alone; at least921600 with four material draws.
                    Require(q.result.draws==5?q.result.samples>=1280u*720u:q.result.samples==1280u*720u,
                        "Catalogue independent fullscreen positive query sample oracle failed");positive+=q.result.samples;
                }
                else Require(q.result.samples==0,"Catalogue empty query samples not zero");
            }
            out.report["capacity_pressure"]={{"capacity",base.async_submission_capacity},{"maximum_deferred",maximum_deferred},
                {"flushes",pressure_flushes},{"positive_samples",positive},{"witnesses",std::move(pressure)}};
            out.report["positive_query_witness"]={{"scope","Synthetic identity fullscreen geometry using real captured pinned VS and original canonical builtin; authentic four-material query coverage is separately allowed to be zero"},
                {"vertex_source",kReadonlyQueryVertexSource},{"vertex_spirv_sha256",kReadonlyQueryVertexSpirv},{"builtin_spirv_sha256",kReadonlyQueryBuiltinSpirv},
                {"fullscreen_samples",1280u*720u},{"material_and_probe_draws",5},{"standalone_probe_draws",1}};
            const auto after_query=scene.report();mixed();check_mixed(after_query,scene.report(),true);
            out.report["fresh_after_queries"]={{"before",after_query},{"after",scene.report()}};
        }
        if(mode=="owned") {
            Require(catalog&&scene.HasPendingGpuWork()&&Count(scene.report(),"native_early_depth_catalog","retained_image_bytes")>0,
                "Catalogue owned tail lacks an actual pending immutable generation");
            out.report["pending_owned_tail"]=scene.report();out.report["explicit_tail_drain_performed"]=false;
            // Scene's real destructor performs the completion. No fixture-owned
            // GPU handle or generation lease is retained across its return.
        } else {
            out.report["before_strong_final_read"]=scene.report();out.memory=scene.ReadFullEdram();collect();
            Require(out.memory.size()==kBytes&&!scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==0,"Catalogue final full-memory/query completion");
            if(!replacement_seed.empty()) {
                uint64_t changed=0;
                for(uint32_t at=0;at<kDepthBytes;at+=4) {
                    Require(out.memory[at]==replacement_seed[at],"Catalogue stencil-off material changed canonical stencil");
                    changed+=out.memory[at+1]!=0||out.memory[at+2]!=0||out.memory[at+3]!=0;
                }
                Require(changed>0&&out.memory.back()==replacement_seed.back(),"Catalogue stale-anchor mutation witness is vacuous or changed untouched tail");
                out.report["monotonic_changed_depth_words"]=changed;
            }
            // Reuse the captured full-size resolve envelope but the authentic
            // donor surface format. This is a genuine native image conversion,
            // not a CPU image synthesized from the compared packed bytes.
            auto resolve=donors.resolve;resolve["event"]=1009999u;resolve["frame"]=frame;
            const auto& c=*donors.draws[0].surfaces[0];
            std::ostringstream address;address<<std::hex<<c.address;
            resolve["surfaces"]["6"]={{"address",address.str()},{"depth",c.depth},{"width",c.width},{"height",c.height},
                {"surface_info",c.surface_info},{"format_info",c.format_info},{"edram_base",c.edram_base},{"format",c.format}};
            scene.Execute(resolve);out.image=Read(out.output/"resolve-1009999.ppm",16u*1024*1024);
            Require(!out.image.empty()&&scene.ReadFullEdram()==out.memory,"Catalogue image resolve changed packed EDRAM");
            out.queries=Identities(completed);out.report["final"]=scene.report();
            Write(out.output/"full-edram.bin",out.memory);
        }
    }
    out.report["owned_destructor_returned"]=true;return out;
}
}
EarlyDepthFixtureContext PrepareNativeEarlyDepthFixtureContext(const SceneExecutorConfig& config,
        std::span<const LiveOperation* const> operations) {
    const auto input=Inputs(config,operations);
    return {input.draws,PositiveQueryProbe(input,0),input.query_vertex_spirv,input.query_builtin_spirv,
        input.resolve,input.metadata,input.history};
}
void PrewarmNativeEarlyDepthFixture(SceneExecutor& scene,
        std::span<const EarlyDepthCatalogSceneInput> inputs,bool catalog) {
    Prewarm(scene,inputs,catalog,"normal");
}
void RunNativeEarlyDepthCatalogSceneFixtures(const SceneExecutorConfig& config,
        std::span<const LiveOperation* const> operations,std::span<const EarlyDepthCatalogSceneInput> inputs) {
    Require(config.native_fsi&&config.native_early_depth&&config.native_early_depth_catalog&&config.defer_native_queries&&
        config.unified_gpu_queue&&config.async_gpu_submissions&&config.batch_transfers&&
        (config.async_submission_capacity==64||config.async_submission_capacity==128),"Catalogue Scene fixture needs explicit asynchronous V1+V2 Native capacity64/128");
    const auto donors=Inputs(config,operations);Json rows=Json::array();
    for(const auto* mode:{"normal","fenced","strong","reinit","query","missing","partial-a3","partial-55"}) {
        const auto off=Run(config,operations,inputs,donors,false,mode),on=Run(config,operations,inputs,donors,true,mode);
        Require(off.report.at("device")==on.report.at("device"),"Catalogue controls used different device identities");
        Require(off.memory.size()==kBytes&&off.memory==on.memory&&off.image==on.image&&off.queries==on.queries,
            "Catalogue paired full10MiB/native image/actual queries are not exact");
        rows.push_back({{"mode",mode},{"exact",true},{"full_edram_bytes",on.memory.size()},{"full_edram_sha256",ContentHash(on.memory)},
            {"native_image_bytes",on.image.size()},{"native_image_sha256",ContentHash(on.image)},{"queries",on.queries},
            {"baseline_output",off.output.string()},{"candidate_output",on.output.string()},{"baseline",off.report},{"candidate",on.report}});
    }
    const auto owned=Run(config,operations,inputs,donors,true,"owned");rows.push_back({{"mode","owned"},{"lifetime_only",true},{"candidate",owned.report}});
    Json summary={{"schema",1},{"capacity",config.async_submission_capacity},{"paired_modes",8},{"owned_modes",1},
        {"whole_capture_replay_executed",false},{"prefix_through",kThrough},{"donors",donors.metadata},{"captured_gpu_history",donors.history},
        {"source_order",Json({kNativeEarlyDepthPixelSource,kNativeEarlyDepthCatalogPixelSources[0],kNativeEarlyDepthCatalogPixelSources[1],kNativeEarlyDepthPixelSource})},
        {"rows",std::move(rows)},
        {"scope","Real captured complete CPU-geometry material donors and authenticated original prefix/history. The separate readonly query probe uses the real pinned captured VS/original canonical builtin with explicitly synthetic identity fullscreen geometry. Execution order/events and explicit reinitialization/query controls are synthetic. Shipping V1 baseline versus V1+V2. Exact complete packed10MiB, native primary-color preview, actual query identities/samples, missing/partial residency, mixed-source immutable reuse after fence/strong mutation and pending owned destruction. PPM preview is supplemented by exact packed color in full-memory comparison. No live FPS or natural source-switch claim."},
        {"lifetime_limit","Pending generation is witnessed by accepted queue work and positive retained image bytes before actual owned Scene destruction. Public API supplies no weak early-generation lease; destructor return is not a separate weak-control-block expiration proof."}};
    std::filesystem::create_directories(config.output);std::ofstream file(config.output/"early-depth-catalog-scene-fixture.json");
    file<<summary.dump(2)<<'\n';Require(bool(file),"Cannot preserve catalogue Scene fixture result");
}
}
