#include "early_depth_material_retention_scene_fixture.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/draw_state.h"
#include "renderer/native/fsi_state_specialization_compiler.h"
#include "renderer/native/fsi_state_specialization_recipe.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/native_monotonic_depth_module.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
constexpr uint32_t kBytes=2048u*5120,kThrough=1395;
constexpr std::string_view kVertex="20d0f93a552fb740d8f3e650b3ceed46b00284cd7c33017a5340c51506c79a49";
constexpr std::string_view kPixel="e413a5d1f8ecffb4f709964a31952aa5e377bb4a331dcc399dd1f28468401c87";
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
uint32_t Word(std::span<const uint8_t> b,size_t at) {
    Require(at+4<=b.size(),"Material retention state bounds");
    return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
void Store(std::vector<uint8_t>& b,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=uint8_t(value>>(24-8*i));
}
std::span<const uint8_t> Bytes(std::span<const uint32_t> v) {
    return {reinterpret_cast<const uint8_t*>(v.data()),v.size_bytes()};
}
const std::vector<uint8_t>& State(const TypedLiveDraw& d) {
    const auto& owner=d.lazy_device_state?d.lazy_device_state->Owner():d.device_state.owner;
    Require(owner&&owner->size()==LazyDeviceState::kBytes,"Material retention state owner");return *owner;
}
uint64_t Count(const Json& r,std::string_view group,std::string_view field) {
    return r.at(std::string(group)).at(std::string(field)).get<uint64_t>();
}
uint64_t Delta(const Json& a,const Json& b,std::string_view group,std::string_view field) {
    const auto x=Count(a,group,field),y=Count(b,group,field);
    Require(y>=x,"Material retention backwards counter");return y-x;
}
void Write(const std::filesystem::path& p,std::span<const uint8_t> data) {
    std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));
    Require(bool(f),"Material retention artifact write");
}
std::vector<uint8_t> Read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);const auto n=f.tellg();
    Require(bool(f)&&n>=0&&uint64_t(n)<=16u*1024*1024,"Material retention artifact extent");
    std::vector<uint8_t> b(static_cast<size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),n);
    Require(bool(f),"Material retention artifact read");return b;
}
struct Donor {
    TypedLiveDraw draw;
    NativeFsiStateRecipe recipe;
    std::vector<uint32_t> vertex,pixel,foreign;
    std::shared_ptr<const FsiStateSpecializationProgram> specialized;
    Json clear,metadata;
};
Donor Prepare(std::span<const LiveOperation* const> operations) {
    Donor out;bool found=false,clear=false;
    for(const auto* op:operations) {
        Require(op,"Material retention null operation");
        if(const auto* d=op->Draw();d&&d->event==1155) {
            Require(!found,"Material retention duplicate separator donor");out.draw=*d;found=true;
        }
        if(!clear&&op->Kind()=="clearrect") {
            const auto& c=op->Control();
            if(c.at("surfaces").contains("10")) {
                const auto& d=c.at("surfaces").at("10");
                if(d.at("width")==1280&&d.at("height")==720&&d.at("edram_base")==0&&
                   d.at("format")==1&&c.at("rectangle")==Json({0,0,1280,720})&&c.at("flags")==48&&c.at("depth")==0) {
                    out.clear=c;clear=true;
                }
            }
        }
    }
    Require(found&&clear,"Material retention fixed weapon separator/clear missing");
    const auto& d=out.draw;
    Require(d.vertex_shader&&d.pixel_shader&&d.vertex_shader->owner&&d.pixel_shader->owner&&
        d.vertex_shader->hash.View()==kVertex&&d.pixel_shader->hash.View()==kPixel&&
        ContentHash(*d.vertex_shader->owner)==kVertex&&ContentHash(*d.pixel_shader->owner)==kPixel&&
        d.function==DrawFunction::DrawIndexedPrimitive&&d.args[1]==4&&!d.rectangle_list,
        "Material retention separator identity");
    for(const auto& g:d.geometry)Require(!g.gpu_memory&&g.payload.owner&&g.bytes==g.payload.owner->size()&&
        ContentHash(*g.payload.owner)==g.payload.hash.View(),"Material retention separator geometry");
    Require(!d.geometry.empty()&&d.up_geometry.empty(),"Material retention separator geometry kind");
    for(const auto& t:d.textures)Require(t.source==TextureSource::CpuSnapshot&&t.base_payload&&t.base_payload->owner&&
        ContentHash(*t.base_payload->owner)==t.base_payload->hash.View()&&
        (!t.mip_payload||(t.mip_payload->owner&&ContentHash(*t.mip_payload->owner)==t.mip_payload->hash.View())),
        "Material retention separator texture owners");
    const auto& raw=State(d);Require(Word(raw,0x2934)==0x700766&&Word(raw,0x28dc)==15,"Material retention separator GE/mask");
    Require(d.surfaces[0]&&d.surfaces[4],"Material retention separator primary surfaces");
    for(unsigned i=1;i<4;++i)Require(!d.surfaces[i],"Material retention separator MRT");
    for(unsigned i:{0u,4u}) {const auto& s=*d.surfaces[i];Require(s.width==1280&&s.height==720&&
        (s.surface_info&16383)==1280&&!(s.surface_info&0x30000)&&s.edram_base==(i?0:720)&&
        s.format==(i?1:3)&&s.depth==bool(i),"Material retention separator surface layout");}
    std::array<uint32_t,kDrawStateWords> words;
    for(unsigned i=0;i<words.size();++i)words[i]=Word(raw,kDrawStateOffset+4*i);
    words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
    const auto state=DecodeDrawState(words,1280,720,4,true);const auto& p=state.pipeline;
    out.recipe.generic={std::string(kVertex),std::string(kPixel),
        {uint32_t(p.cullMode),uint32_t(p.frontFace),uint32_t(p.depthClipEnabled),uint32_t(p.primitiveTopology),state.specialization,0},
        int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
    NativeFsiConstants raster;raster.depth_control=Word(raw,0x2934);raster.depth_format=1;
    raster.color[0]={720,3,15,Word(raw,0x2938)};raster.blend_enabled=uint32_t(p.renderTargetBlend[0].blendEnabled);
    out.recipe.state=NativeFsiStateSpecialization::From(raster);
    NativeShaderCompiler compiler;Require(compiler.CacheVersion()==kNativeEarlyDepthCanonicalVersion,"Material retention compiler changed");
    out.vertex=compiler.Get(*d.vertex_shader->owner,NativeShaderMode::Raster)->spirv;
    out.pixel=compiler.Get(*d.pixel_shader->owner,NativeShaderMode::FragmentInterlock)->spirv;
    out.specialized=TryPrepareFsiStateSpecialization(compiler,*d.pixel_shader->owner);
    Require(out.specialized&&NativeMonotonicDepthModuleCertified(kVertex,false,NativeShaderVariant::Unmodified,Bytes(out.vertex))&&
        NativeMonotonicDepthModuleCertified(kPixel,true,NativeShaderVariant::Unmodified,Bytes(out.pixel))&&
        NativeMonotonicDepthModuleCertified(kPixel,true,NativeShaderVariant::PipelineStateEpilogueV1,Bytes(out.specialized->spirv)),
        "Material retention actual donor module certificates refused");
    out.foreign=out.pixel;out.foreign.at(2)^=1u; // SPIR-V generator metadata only; execution is unchanged.
    Require(!NativeMonotonicDepthModuleCertified(kPixel,true,NativeShaderVariant::Unmodified,Bytes(out.foreign)),
        "Material retention foreign module unexpectedly certified");
    out.metadata={{"event",d.event},{"frame",d.frame},{"vertex",kVertex},{"pixel",kPixel},
        {"state_sha256",ContentHash(raw)},{"vertex_spirv",ContentHash(Bytes(out.vertex))},
        {"pixel_spirv",ContentHash(Bytes(out.pixel))},{"specialized_spirv",ContentHash(Bytes(out.specialized->spirv))},
        {"foreign_spirv",ContentHash(Bytes(out.foreign))},{"foreign_change","Only SPIR-V generator metadata word2 XOR1"}};
    return out;
}
Json Query(uint64_t event,uint32_t frame,bool end) {
    return {{"type",end?"query_end":"query_begin"},{"event",event},{"frame",frame},
        {"query_generation",50001},{"query_handle",0x9977},{"report_address",end?0x1f00000:0x1f00020}};
}
struct Outcome {std::vector<uint8_t> memory,image,query_memory;Json report,queries;std::filesystem::path output;};
Outcome Run(const SceneExecutorConfig& base,std::span<const LiveOperation* const> operations,
        std::span<const EarlyDepthCatalogSceneInput> inputs,const EarlyDepthFixtureContext& context,
        const Donor& donor,bool enabled,std::string_view mode) {
    auto config=base;config.native_early_depth_material_retention=enabled;config.present={};
    config.reuse_early_depth_readonly_boundaries=false;config.multi_draw_command_buffers=false;
    config.native_readonly_depth_attachment=false;config.max_draw_batch_size=16;config.debug_raw_images=false;
    config.specialize_fsi_pipeline_state=mode=="specialized"||mode=="wrong-state";
    config.fsi_state_material_composition=config.specialize_fsi_pipeline_state;
    if(config.specialize_fsi_pipeline_state) {
        auto programs=std::make_shared<FsiStateSpecializationCatalog>();programs->programs.emplace(std::string(kPixel),donor.specialized);
        config.fsi_state_programs=std::move(programs);
    }
    config.output=base.output/(std::string("material-")+(enabled?"on-":"off-")+std::string(mode));
    std::filesystem::create_directories(config.output);Outcome out;out.output=config.output;
    SceneExecutor scene(std::move(config));Require(!scene.report().at("device").at("software").get<bool>(),"Material retention hardware required");
    out.report["device"]=scene.report().at("device");
    scene.PrewarmFsiPipeline(donor.recipe.generic,donor.vertex,mode=="foreign-module"?donor.foreign:donor.pixel);
    PrewarmNativeEarlyDepthFixture(scene,inputs,true);
    if(mode=="specialized"||mode=="wrong-state") {
        auto recipe=donor.recipe;if(mode=="wrong-state")recipe.state.words[0]=0x700716;
        Require(scene.PrewarmFsiStatePipeline(recipe,donor.vertex),"Material retention state startup refused");
    }
    scene.PrewarmFsiPipeline({std::string(kReadonlyQueryVertexSource),std::nullopt,
        {3,2,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1},context.query_vertex_spirv,context.query_builtin_spirv);
    scene.RunLiveCapture(operations,kThrough);
    Require(scene.report().at("initial_gpu_history").at("frame")==context.history.at("frame"),"Material retention history import");
    scene.FlushCommittedState();scene.FlushPendingDraws();
    Require(!scene.ActiveQueryMetadata()&&scene.TakeCompletedQueries().empty(),"Material retention prefix query boundary");
    const uint32_t frame=context.draws[0].frame;uint64_t event=2000000;
    const auto early=[&] {auto d=context.draws[0];d.event=event++;scene.Execute(d);};
    auto separator=donor.draw;
    if(mode=="compare") {auto raw=State(separator);Store(raw,0x2934,0x700716);const auto hash=ContentHash(raw);
        separator.device_state={CanonicalDigest(hash),std::make_shared<const std::vector<uint8_t>>(std::move(raw))};separator.lazy_device_state.reset();}
    if(mode=="alias") {
        separator.surfaces[0]->edram_base=0;separator.surfaces[0]->format_info&=~0xfffu;
        // The configured surfaces still overlap and MUST be refused. Limit
        // actual raster coverage to one40-pixel half-tile: aliased color uses
        // columns0..39, while depth's bank swap uses40..79. Otherwise ordinary
        // FSI has cross-pixel races which pixel interlock cannot serialize.
        auto raw=State(separator);Store(raw,0x28c4,0x80000000u|880u);Store(raw,0x28c8,920u|(720u<<16));
        std::array<uint32_t,kDrawStateWords> words;
        for(unsigned i=0;i<words.size();++i)words[i]=Word(raw,kDrawStateOffset+4*i);
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto decoded=DecodeDrawState(words,1280,720,4,true);
        Require(decoded.scissor.left==880&&decoded.scissor.top==0&&decoded.scissor.right==920&&decoded.scissor.bottom==720,
            "Material retention alias scissor was not the exact nonracing half-tile");
        const auto hash=ContentHash(raw);separator.device_state={CanonicalDigest(hash),std::make_shared<const std::vector<uint8_t>>(std::move(raw))};
        separator.lazy_device_state.reset();
        out.report["alias_control"]={{"configured_color_base",0},{"configured_depth_base",0},{"scissor",Json({880,0,920,720})},
            {"active_color_columns",Json({0,39})},{"active_depth_columns",Json({40,79})},{"active_address_intersection",0},
            {"state_sha256",hash},{"scope","Full configured layout remains rejected; only synthetic raster coverage excludes cross-pixel color/depth races."}};
    }
    // Give the authentic material real depth work to perform; the replayed
    // prefix already contained these donors and could otherwise make equal
    // depth rewrites a vacuous monotonic witness.
    auto seed=donor.clear;seed["event"]=event++;seed["frame"]=frame;scene.Execute(seed);
    const auto before=scene.report();early();const auto anchored=scene.report();
    Require(Delta(before,anchored,"native_early_depth","conversions")==1&&
        Delta(before,anchored,"native_early_depth","selected")==1,"Material retention initial anchor was not selected");
    std::vector<uint8_t> witness_before;
    if(!enabled&&(mode=="normal"||mode=="alias"))witness_before=scene.ReadFullEdram();
    if(mode=="clear") {auto c=donor.clear;c["event"]=event++;c["frame"]=frame;scene.Execute(c);}
    if(mode=="strong-read")Require(scene.ReadFullEdram().size()==kBytes,"Material retention strong read extent");
    if(mode=="query")scene.Execute(Query(event++,frame,false));
    separator.event=event++;scene.Execute(separator);
    if(!witness_before.empty()) {
        const auto witness_after=scene.ReadFullEdram();uint64_t increased=0;
        Require(witness_before.size()==kBytes&&witness_after.size()==kBytes,"Material retention monotonic witness extent");
        const auto depth=[](const std::vector<uint8_t>& b,size_t at) {
            // Packed words are little-endian in the native GPU buffer; the
            // first byte is stencil and the remaining24bits are Xbox FP24.
            return uint32_t(b[at+1])|uint32_t(b[at+2])<<8|uint32_t(b[at+3])<<16;
        };
        for(size_t at=0;at<720u*5120;at+=4) {
            if(mode=="alias") {
                const auto word=at/4,tile=word/1280,column=word%80;
                const auto x=(tile%16)*80+(column+40)%80;
                if(x<880||x>=920)continue; // Only the nonaliased active depth half.
            }
            const auto a=depth(witness_before,at),b=depth(witness_after,at);
            Require(b>=a&&witness_before[at]==witness_after[at],"Material retention separator decreased depth or changed stencil");
            increased+=b>a;
        }
        Require(increased>0,"Material retention separator depth-advance witness was vacuous");
        out.report[mode=="alias"?"alias_coverage_witness":"depth_advance_witness"]={{"increased_depth_words",increased},{"decreased_depth_words",0},
            {"before_sha256",ContentHash(witness_before)},{"after_sha256",ContentHash(witness_after)},
            {"scope",mode=="alias"?"OFF alias only: require real depth advance in the active880..919 half-tile, with unchanged active stencil. Color writes the disjoint bank-swapped half. Candidate sequence is uninterrupted; baseline already requires two conversions.":
                "OFF normal only: real full-buffer reads after first early draw and after separator. Candidate sequence remains uninterrupted; extra baseline reads do not change its already-required two conversions. No performance measurement."}};
        Write(out.output/"witness-before.bin",witness_before);Write(out.output/"witness-after.bin",witness_after);
    }
    if(mode=="query") {auto d=context.positive_query;d.event=event++;scene.Execute(d);scene.Execute(Query(event++,frame,true));}
    early();const auto after=scene.report();
    const bool positive=mode=="normal"||mode=="fenced"||mode=="specialized"||mode=="wrong-state";
    Require(Delta(before,after,"native_early_depth","selected")==2&&
        Delta(before,after,"native_early_depth","conversions")==uint64_t(enabled&&positive?1:2)&&
        Delta(before,after,"native_early_depth","cache_hits")==uint64_t(enabled&&positive),
        "Material retention actual conversion/reuse counts differ from the controlled sequence");
    if(enabled) {
        Require(Delta(before,after,"native_early_depth_material_retention","selected")==uint64_t(positive)&&
            Delta(before,after,"native_early_depth_material_retention","token_refusals")==0&&
            Delta(before,after,"native_early_depth_material_retention","late_refusals")==0,
            "Material retention accepted proof coverage/refusal mismatch");
        if(mode=="compare")Require(Delta(before,after,"native_early_depth_material_retention","state_refusals")==1,"Material retention compare not refused");
        if(mode=="alias")Require(Delta(before,after,"native_early_depth_material_retention","layout_refusals")==1,"Material retention alias not refused");
        if(mode=="foreign-module")Require(Delta(before,after,"native_early_depth_material_retention","module_refusals")==1,"Material retention foreign module/PSO not refused");
    }
    if(mode=="specialized")Require(Delta(anchored,after,"fsi_state_specialization","selected")==1,"Material retention variant8 not selected");
    if(mode=="wrong-state")Require(Delta(anchored,after,"fsi_state_specialization","selected")==0&&
        Delta(anchored,after,"fsi_state_specialization","residency_refusals")>0,"Material retention wrong state PSO used");
    out.report["controlled"]={{"before",before},{"anchored",anchored},{"after",after}};
    if(mode=="fenced") {scene.FlushPendingDraws();const auto a=scene.report();early();Require(
        Delta(a,scene.report(),"native_early_depth","cache_hits")==1&&Delta(a,scene.report(),"native_early_depth","conversions")==0,
        "Material retention fence lost current generation");out.report["fenced"]=scene.report();}
    // Preserve the entire controlled result BEFORE any independent positive
    // query seed can overwrite it (especially the deliberate color/depth alias).
    out.memory=scene.ReadFullEdram();Require(out.memory.size()==kBytes,"Material retention full EDRAM extent");
    auto resolve=context.resolve;resolve["event"]=2009999u;resolve["frame"]=frame;
    const auto& c=*context.draws[0].surfaces[0];std::ostringstream address;address<<std::hex<<c.address;
    resolve["surfaces"]["6"]={{"address",address.str()},{"depth",c.depth},{"width",c.width},{"height",c.height},
        {"surface_info",c.surface_info},{"format_info",c.format_info},{"edram_base",c.edram_base},{"format",c.format}};
    scene.Execute(resolve);out.image=Read(out.output/"resolve-2009999.ppm");
    Require(!out.image.empty()&&scene.ReadFullEdram()==out.memory,"Material retention resolve changed packed EDRAM");
    if(mode!="query") {
        auto clear=donor.clear;clear["event"]=event++;clear["frame"]=frame;scene.Execute(clear);
        scene.Execute(Query(event++,frame,false));auto d=context.positive_query;d.event=event++;scene.Execute(d);scene.Execute(Query(event++,frame,true));
    }
    scene.FlushGpuWork();const auto completed=scene.TakeCompletedQueries();
    Require(completed.size()==1&&completed[0].generation==50001&&completed[0].frame==frame&&
        completed[0].result.handle==0x9977&&completed[0].result.begin_address==0x1f00020&&completed[0].result.end_address==0x1f00000&&
        completed[0].result.draws==uint32_t(mode=="query"?2:1)&&completed[0].result.samples>=1280u*720u,
        "Material retention independent positive actual query oracle");
    const auto& q=completed[0];out.queries=Json::array({Json({q.generation,q.frame,q.end_event,q.result.handle,
        q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples})});
    out.query_memory=scene.ReadFullEdram();Require(out.query_memory.size()==kBytes,"Material retention post-query extent");
    out.report["final"]=scene.report();Write(out.output/"full-edram.bin",out.memory);
    Write(out.output/"post-query-edram.bin",out.query_memory);
    const auto control=out.output/"control-report.json";Require(!std::filesystem::exists(control),"Material retention control report already exists");
    std::ofstream report_file(control);report_file<<out.report.dump(2)<<'\n';Require(bool(report_file),"Material retention control report write");
    return out;
}
}
void RunNativeEarlyDepthMaterialRetentionSceneFixtures(const SceneExecutorConfig& config,
        std::span<const LiveOperation* const> operations,std::span<const EarlyDepthCatalogSceneInput> inputs) {
    Require(config.native_fsi&&config.native_early_depth&&config.native_early_depth_catalog&&config.defer_native_queries&&
        config.unified_gpu_queue&&config.async_gpu_submissions&&config.batch_transfers,
        "Material retention Scene fixture requires complete asynchronous native catalogue startup");
    const auto context=PrepareNativeEarlyDepthFixtureContext(config,operations);const auto donor=Prepare(operations);
    Json rows=Json::array();
    for(const auto* mode:{"normal","fenced","clear","strong-read","query","compare","alias","foreign-module","specialized","wrong-state"}) {
        const auto off=Run(config,operations,inputs,context,donor,false,mode),on=Run(config,operations,inputs,context,donor,true,mode);
        Require(off.report.at("device")==on.report.at("device")&&off.memory==on.memory&&off.image==on.image&&off.queries==on.queries&&off.query_memory==on.query_memory,
            "Material retention paired full EDRAM/image/query parity failed");
        rows.push_back({{"mode",mode},{"exact",true},{"full_edram_bytes",on.memory.size()},{"full_edram_sha256",ContentHash(on.memory)},
            {"native_image_sha256",ContentHash(on.image)},{"post_query_edram_sha256",ContentHash(on.query_memory)},
            {"queries",on.queries},{"baseline",off.report},{"candidate",on.report}});
    }
    Json summary={{"schema",2},{"paired_modes",rows.size()},{"prefix_through",kThrough},{"whole_capture_replay_executed",false},
        {"early_donors",context.metadata},{"separator",donor.metadata},{"captured_history",context.history},{"rows",std::move(rows)},
        {"scope","Actual captured CPU-geometry draws and authenticated history/prefix. Synthetic event/order/clear/query/state/alias controls; exact complete10MiB EDRAM, native image and real positive query identities/samples. Conversion misses/hits witness actual generation reuse. Specialized mode selects the certified actual variant8; wrong-state mode declines a nonmatching resident PSO and retains via the independently certified canonical path."},
        {"limit","Foreign-module mode creates and draws a valid metadata-modified SPIR-V and cached PSO under the same source key. It does not isolate an independently evicted shader module with an older surviving PSO; creation-time PSO provenance is additionally source-reviewed and CPU policy-tested. No FPS or natural eight-to-two conversion claim."}};
    std::ofstream f(config.output/"early-depth-material-retention-scene-fixture.json");f<<summary.dump(2)<<'\n';Require(bool(f),"Material retention summary write");
}
}
