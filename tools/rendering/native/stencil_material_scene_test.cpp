#include "renderer/native/stencil_material_cutoff_compiler.h"
#include "renderer/native/scene_executor.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/content_hash.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#if defined(GOW3_NATIVE_STENCIL_MATERIAL_REPLAY) || defined(GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY)
#error This fixture must exercise the production Scene without replay census code.
#endif
using namespace gow3::renderer;
using Json=nlohmann::json;
using Bytes=std::vector<uint8_t>;
namespace {
uint64_t checks=0;
void Check(bool good,const std::string& why){++checks;if(!good)throw std::runtime_error(why);}
Bytes Read(const std::filesystem::path& path,size_t maximum=64u*1024*1024) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);Check(bool(input),"Missing input: "+path.string());
    const auto size=input.tellg();Check(size>=0&&uint64_t(size)<=maximum,"Unbounded input");
    Bytes bytes(static_cast<size_t>(size));input.seekg(0);
    Check(bool(input.read(reinterpret_cast<char*>(bytes.data()),bytes.size())),"Input read");return bytes;
}
Bytes Module(std::span<const uint32_t> words){Bytes bytes(words.size_bytes());std::memcpy(bytes.data(),words.data(),bytes.size());return bytes;}
uint32_t Word(const Bytes& bytes,size_t offset){Check(offset+4<=bytes.size(),"State word extent");return uint32_t(bytes[offset])<<24|uint32_t(bytes[offset+1])<<16|uint32_t(bytes[offset+2])<<8|bytes[offset+3];}
void Word(Bytes& bytes,size_t offset,uint32_t value){Check(offset+4<=bytes.size(),"State mutation extent");for(unsigned i=0;i<4;++i)bytes[offset+i]=uint8_t(value>>(24-8*i));}
struct Inputs {
    std::filesystem::path capture;
    std::map<std::string,OwnedPayload::Snapshot> payloads;
    Json donor;
    OwnedPayload::Snapshot Get(const std::string& key) {
        if(const auto found=payloads.find(key);found!=payloads.end())return found->second;
        Check(key.size()==64&&key.find_first_not_of("0123456789abcdef")==key.npos,"Invalid payload identity");
        auto bytes=std::make_shared<const Bytes>(Read(capture/(key+".payload.bin")));
        Check(ContentHash(*bytes)==key,"Payload authentication");payloads.emplace(key,bytes);return bytes;
    }
    std::string Add(Bytes bytes){auto key=ContentHash(bytes);payloads[key]=std::make_shared<const Bytes>(std::move(bytes));return key;}
};
Json Queries(const std::vector<CompletedNativeQuery>& values) {
    Json out=Json::array();for(const auto& q:values)out.push_back({q.generation,q.frame,q.end_event,q.result.handle,
        q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});return out;
}
Json Queue(const Json& report) {
    const auto& q=report.at("gpu_work_queue");Json result;
    for(const char* key:{"accepted_work","completed_work","draw_lists","transfer_lists","submissions","capacity","async_batch_lists","inflight_batch_limit"})result[key]=q.at(key);
    Check(q.at("accepted_work")==q.at("completed_work"),"All accepted work must retire");return result;
}
Json Query(uint64_t event,bool end) {
    return {{"type",end?"query_end":"query_begin"},{"event",event},{"frame",1},{"query_generation",1},
        {"query_handle",0xabc0},{"report_address",end?0x2000:0x2020}};
}
Bytes Seed(const std::string& name) {
    Bytes bytes(2048u*5120);
    for(size_t i=0;i<bytes.size();i+=4) {
        uint32_t value=uint32_t((i/4)*1664525u+1013904223u);
        const uint8_t stencil=name=="fail"||name=="clear-before"?0:name=="mixed"?uint8_t((i/4)%2):1;
        value=(value&0xffffff00u)|stencil;std::memcpy(bytes.data()+i,&value,4);
        // Only the depth surface interprets this byte as stencil. Give the
        // dependency witness a nonzero destination R so a zero material output
        // still demonstrates that the pending stencil clear allowed the draw.
        if(name=="clear-before"&&i>=720u*5120&&i<1440u*5120)bytes[i]=0x7f;
    }
    return bytes;
}
struct Result {Bytes edram;Json queries,queue,mechanism;bool pending_transfer=false;};
Result Run(Inputs& input,const SceneExecutorConfig& base,const NativeFsiPipelineRecipe& recipe,
           std::span<const uint32_t> vertex,std::span<const uint32_t> pixel,plume::RenderDevice& device,
           plume::RenderCommandQueue& queue,bool enabled,const std::string& name) {
    auto config=base;config.native_stencil_material_cutoff=enabled;
    if(name=="missing-program")config.native_stencil_material_program.reset();
    SceneExecutor scene(config,&device,&queue);
    Check(scene.report().at("device").at("software")==false,"Hardware Vulkan device required");
    const bool prewarm=name!="no-prewarm";
    if(prewarm)scene.PrewarmFsiPipeline(recipe,vertex,pixel);
    if(enabled&&prewarm&&name!="missing-program") {
        auto malformed=std::vector<uint32_t>(pixel.begin(),pixel.end());malformed[0]^=1;
        Check(!scene.PrewarmStencilMaterialPipeline(recipe,vertex,malformed),"Wrong actual canonical PS bytes refused");
        auto wrong_recipe=recipe;wrong_recipe.raster[0]^=1;
        Check(!scene.PrewarmStencilMaterialPipeline(wrong_recipe,vertex,pixel),"Wrong immutable raster recipe refused");
        Check(scene.PrewarmStencilMaterialPipeline(recipe,vertex,pixel),"Exact candidate pipeline prepared");
        const auto creations=scene.report().at("pipelines_created");
        Check(scene.PrewarmStencilMaterialPipeline(recipe,vertex,pixel),"Duplicate startup prewarm stays resident");
        Check(scene.report().at("pipelines_created")==creations&&!scene.mutation_serial()&&!scene.HasPendingGpuWork(),"Duplicate prewarm does not create/mutate/submit");
    }
    auto draw=input.donor;draw["event"]=3;draw["frame"]=1;
    auto state=*input.Get(draw.at("device_state").get<std::string>());
    if(name=="control")Word(state,0x2934,Word(state,0x2934)^1u);
    if(name=="reference")for(size_t offset:{0x2900u,0x28fcu})Word(state,offset,Word(state,offset)|1u);
    if(name=="write-mask")for(size_t offset:{0x2900u,0x28fcu})Word(state,offset,Word(state,offset)&~0xff0000u);
    draw["device_state"]=input.Add(std::move(state));
    if(name=="index")for(auto& block:draw.at("up_geometry"))if(block.at("kind")==97) {
        auto indices=*input.Get(block.at("sha256").get<std::string>());
        // Swap two valid index values: it remains a valid bounded draw, but
        // fails the exact owned-index certificate without forging its digest.
        std::swap(indices.at(0),indices.at(2));std::swap(indices.at(1),indices.at(3));
        block["sha256"]=input.Add(std::move(indices));
    }
    auto seed=Seed(name);scene.InitializeEdram(seed);scene.InitializeMemory(0x2000,Bytes(128));
    if(enabled&&prewarm&&name!="missing-program") {
        bool refused=false;try{scene.PrewarmStencilMaterialPipeline(recipe,vertex,pixel);}catch(const std::invalid_argument&){refused=true;}
        Check(refused,"Prewarm after authoritative mutation must refuse");
    }
    Result result;
    if(name=="clear-before") {
        // Warm the exact texture/module/geometry resources before the dependency
        // witness. The following authoritative seed replaces this warmup draw.
        auto warm=draw;warm["event"]=0;scene.Execute(warm);scene.FlushGpuWork();scene.InitializeEdram(seed);
    }
    const uint64_t selected_before=enabled?scene.report().at("native_stencil_material_cutoff").at("selected").get<uint64_t>():0;
    const auto texture_creations=scene.report().at("texture_cache").at("creations");
    if(name=="clear-before") {
        scene.Execute(Json{{"type","clearrect"},{"event",1},{"frame",1},{"flags",0x20},
            {"rectangle",{0,0,1280,720}},{"color",nullptr},{"depth",1.0f},{"stencil",1},
            {"surfaces",{{"10",draw.at("surfaces").at("10")}}}});
        result.pending_transfer=scene.HasPendingGpuWork();
        Check(result.pending_transfer,"Transfer dependency witness must remain pending before draw");
    }
    const bool queried=name=="query";
    if(queried)scene.Execute(Query(2,false));
    scene.Execute(draw);
    if(name=="clear-before")Check(scene.report().at("texture_cache").at("creations")==texture_creations,"Dependency draw reused already uploaded textures");
    if(queried)scene.Execute(Query(4,true));
    scene.FlushGpuWork();result.queries=Queries(scene.TakeCompletedQueries());
    if(queried)Check(result.queries.size()==1&&result.queries[0][6]==1&&result.queries[0][7].get<uint64_t>()>0,"Actual queried canonical fallback lacks positive samples");
    else Check(result.queries.empty(),"Unqueried draw created a guest query");
    result.edram=scene.ReadFullEdram();const auto report=scene.report();result.queue=Queue(report);
    Check(!report.contains("stencil_material_cutoff"),"Production Scene exposed diagnostic census");
    if(enabled) {
        result.mechanism=report.at("native_stencil_material_cutoff");
        for(const char* key:{"counter_descriptors","counter_buffers","counter_readbacks"})Check(result.mechanism.at(key)==0,"Mode3 diagnostic resource forbidden");
        const bool selected=name=="fail"||name=="pass"||name=="mixed"||name=="clear-before";
        Check(result.mechanism.at("selected").get<uint64_t>()-selected_before==unsigned(selected),"Non-vacuous selected/fallback expectation: "+name);
        if(name=="query")Check(result.mechanism.at("query_refusals")==1,"Active query refusal witness");
        if(name=="index")Check(result.mechanism.at("refusal_reasons").at("owned_up_cube")==1,"Changed index payload refusal witness");
        if(name=="control"||name=="reference"||name=="write-mask")Check(result.mechanism.at("refusal_reasons").at("exact_state")==1,"Changed exact state refusal witness");
    }
    if(name=="clear-before")Check(!std::equal(seed.begin()+720u*5120,seed.begin()+1440u*5120,result.edram.begin()+720u*5120),"Dependency witness must produce actual color after changing stencil0 to1");
    if(name=="fail")Check(result.edram==seed,"All-zero stencil draw must preserve every packed byte");
    if(name=="fail"||name=="pass"||name=="mixed")Check(std::equal(seed.begin(),seed.begin()+720u*5120,result.edram.begin()),"All-KEEP draw changed a packed depth/stencil bit");
    return result;
}
}
int main(int argc,char** argv)try {
    if(argc!=5&&argc!=6)throw std::runtime_error("Usage: scene-test inspected-capture exact-5dcdf6.bin exact-3216ee.bin fresh-output [donor-event=1936]");
    const uint64_t donor_event=argc==6?std::stoull(argv[5]):1936;
    const std::filesystem::path output=argv[4];Check(!std::filesystem::exists(output),"Fresh output required");std::filesystem::create_directories(output);
    Inputs input;input.capture=argv[1];const auto scene_bytes=Read(input.capture/"scene.json");const auto original=Json::parse(scene_bytes);
    for(const auto& draw:original.at("operations"))if(draw.value("type","")=="draw"&&draw.value("function","")=="BeginIndexedVertices"&&
        draw.at("event").get<uint64_t>()==donor_event&&!draw.at("pixel_shader").is_null()&&draw.at("pixel_shader").get<std::string>()==kStencilMaterialPixel&&draw.at("vertex_shader").get<std::string>()==kStencilMaterialVertex) {
        const auto state=input.Get(draw.at("device_state").get<std::string>());
        if(Word(*state,0x2934)==0x00700571&&Word(*state,0x2900)==0x00ffff00&&Word(*state,0x28fc)==0x00ffff00){input.donor=draw;break;}
    }
    Check(!input.donor.is_null(),"Exact captured target donor missing");
    // This controlled fixture needs no replay history. The captured immutable
    // bytes become declared CPU snapshots in both arms, never guessed aliases.
    for(auto& texture:input.donor.at("textures")) {
        Check(!texture.at("base_payload").is_null(),"Captured texture snapshot missing");input.Get(texture.at("base_payload").get<std::string>());
        if(!texture.at("mip_payload").is_null())input.Get(texture.at("mip_payload").get<std::string>());
        texture["source"]="cpu_snapshot";texture.erase("producers");
    }
    const auto pixel_source=Read(argv[2],1024),vertex_source=Read(argv[3],1024);
    NativeShaderCompiler compiler(output/"canonical-cache");
    const auto vs=compiler.Get(vertex_source,NativeShaderMode::Raster),ps=compiler.Get(pixel_source,NativeShaderMode::FragmentInterlock);
    Check(vs->source_sha256==kStencilMaterialVertex&&ps->source_sha256==kStencilMaterialPixel,"Exact source pair");
    const auto program=CompileNativeStencilMaterial(compiler,pixel_source,NativeStencilMaterialMode::Uninstrumented);
    FsiDevice host(true);auto& device=host.device();auto queue=device.createCommandQueue(plume::RenderCommandListType::DIRECT);
    SceneExecutorConfig base;base.native_fsi=true;base.ordered_pixel_interlock_enabled=true;
    base.batch_transfers=true;base.unified_gpu_queue=true;base.async_gpu_submissions=true;base.max_draw_batch_size=16;
    base.defer_native_queries=true;base.max_deferred_queries=16;base.async_submission_capacity=64;
    base.restore_depth_stencil_export=host.stencil_export_enabled();base.shader_stencil_export_enabled=host.stencil_export_enabled();
    base.time_cpu_stages=false;base.discard_dummy_fsi_attachment=true;base.native_stencil_material_program=program;
    base.payload_snapshot=[&](const std::string& key){return input.Get(key);};
    base.shader=[&](const std::string& key){Check(key==kStencilMaterialVertex,"Unexpected VS");return Module(vs->spirv);};
    base.fsi_shader=[&](const std::string& key){Check(key==kStencilMaterialPixel,"Unexpected PS");return Module(ps->spirv);};
    const NativeFsiPipelineRecipe recipe{std::string(kStencilMaterialVertex),std::string(kStencilMaterialPixel),{3,2,1,4,0,0},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
    Json results=Json::array();
    for(const std::string name:{"fail","pass","mixed","clear-before","control","reference","write-mask","index","query","no-prewarm","missing-program"}) {
        const auto off=Run(input,base,recipe,vs->spirv,ps->spirv,device,*queue,false,name);
        const auto on=Run(input,base,recipe,vs->spirv,ps->spirv,device,*queue,true,name);
        Check(off.edram==on.edram,"Full packed EDRAM differs: "+name);Check(off.queries==on.queries,"Actual guest queries differ: "+name);
        Check(off.queue==on.queue,"Deterministic real work/submission counts differ: "+name);
        results.push_back({{"name",name},{"edram_bytes",off.edram.size()},{"edram_sha256",ContentHash(off.edram)},
            {"actual_queries",off.queries},{"queue",off.queue},{"mechanism",on.mechanism},{"pending_transfer",on.pending_transfer}});
    }
    for(unsigned altered=0;altered<5;++altered) {
        auto config=base;config.native_stencil_material_cutoff=true;auto bad=std::make_shared<NativeStencilMaterialProgram>(*program);
        if(altered==0)bad->derived_version[0]^=1;
        if(altered==1)bad->spirv=ps->spirv;
        if(altered==2)bad->mode=NativeStencilMaterialMode::Observe;
        if(altered==3)config.replay_stencil_material_mode=1;
        if(altered==4)config.implicit_fsi_barriers=true;
        config.native_stencil_material_program=bad;
        bool refused=false;try{SceneExecutor invalid(config,&device,queue.get());}catch(const std::invalid_argument&){refused=true;}
        Check(refused,"Malformed program/overlap constructor refusal");
    }
    Json report{{"passed",true},{"checks",checks},{"capture_scene_sha256",ContentHash(scene_bytes)},
        {"donor_event",input.donor.at("event")},{"mode","uninstrumented"},{"replay_macros",false},
        {"derived_version",program->derived_version},{"cases",results},
        {"scope","Actual canonical shaders and captured UP/state/texture bytes; explicit deterministic EDRAM and declared CPU textures. Full captured-history replays remain a separate qualification."}};
    std::ofstream file(output/"result.json");file<<report.dump(2)<<'\n';Check(bool(file),"Result write");
    std::cout<<"PASS: "<<checks<<" stencil-material production Scene checks; 11 exact full-EDRAM/query/work pairs; no replay macros\n";
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
