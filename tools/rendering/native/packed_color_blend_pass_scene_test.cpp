// Report-local controlled blending integration fixture. Never a capture replay.
#include "renderer/native/scene_executor.h"
#include "renderer/native/packed_color_pass_groups.h"
#include "renderer/native/packed_color_variant8_catalog.h"
#include "renderer/native/packed_color_blend_variant8_catalog.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/draw_state.h"
#include "renderer/native/fsi_state_specialization_compiler.h"
#include "renderer/native/texture_data.h"
#include "native_fsi_state_specialization_shader_version.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
using namespace plume;
using namespace gow3::renderer;
namespace {
using Json=nlohmann::json;using Words=std::vector<uint32_t>;using Bytes=std::vector<uint8_t>;
uint64_t checks=0;
void Check(bool x,const char* why) {++checks;if(!x)throw std::runtime_error(why);}
Bytes ReadBytes(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);Check(f&&f.tellg()>=0,"Fixture file absent");Bytes b(size_t(f.tellg()));f.seekg(0);Check(bool(f.read(reinterpret_cast<char*>(b.data()),b.size())),"Truncated fixture");return b;
}
Words AsWords(const Bytes& b) {Check(b.size()%4==0,"Unaligned shader extent");Words w(b.size()/4);std::memcpy(w.data(),b.data(),b.size());return w;}
Words ReadSceneEdram(RenderDevice& device, RenderCommandQueue& queue, const NativeMemoryState& state) {
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(state.capacity));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    const auto command=static_cast<VulkanCommandList&>(*commands).vk;
    auto& buffer=static_cast<VulkanBuffer&>(*readback);
    VkBufferMemoryBarrier dependency{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    dependency.buffer=state.buffer;dependency.size=VK_WHOLE_SIZE;
    dependency.srcQueueFamilyIndex=dependency.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    dependency.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,1,&dependency,0,nullptr);
    const VkBufferCopy copy{0,0,state.capacity};vkCmdCopyBuffer(command,state.buffer,buffer.vk,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto* mapped=readback->map();Check(mapped,"Scene readback map failed");
    Check(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
        "Scene readback invalidate failed");
    Words words(state.capacity/4);std::memcpy(words.data(),mapped,state.capacity);readback->unmap();return words;
}
Words ReadPhysicalRange(RenderDevice& device,RenderCommandQueue& queue,const NativeMemoryState& state,uint32_t address,uint32_t size) {
    Check(size&&size%4==0&&address<=state.capacity&&size<=state.capacity-address,"Invalid conversion witness range");
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(size));auto list=queue.createCommandList();auto fence=device.createCommandFence();
    list->begin();const auto command=static_cast<VulkanCommandList&>(*list).vk;auto& buffer=static_cast<VulkanBuffer&>(*readback);
    VkBufferMemoryBarrier dep{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};dep.buffer=state.buffer;dep.offset=address;dep.size=size;
    dep.srcQueueFamilyIndex=dep.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;dep.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dep.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&dep,0,nullptr);
    const VkBufferCopy copy{address,0,size};vkCmdCopyBuffer(command,state.buffer,buffer.vk,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    list->end();queue.executeCommandLists(list.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto* data=readback->map();Check(data,"Conversion witness map failed");
    Check(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Conversion witness invalidate failed");
    Words result(size/4);std::memcpy(result.data(),data,size);readback->unmap();return result;
}

struct Donor {
    const char* name;uint32_t event;const char* vertex;const char* pixel;
    const char* state_digest;const char* vertex_digest;const char* pixel_digest;
    uint32_t pixel_bytes;std::array<uint32_t,6> raster;uint32_t blend;
};
constexpr Donor kAlpha{"alpha",934,
    "8481e1ad2902eb836ac9f35912140c17dd250fa5127c4f540d0cf8624e168055",
    "0a674266d6cbdded11253ca5e9e5f8ad356af447f3196fe9b0d3c504ef1a8440",
    "87921b78c77d4b1981b1faad451d540bf09410ece6ee123f6acdb20388e00a25",
    "fef8f2c1702fc6f06c8475031d337b376a16b1067958d520cec0802ed0611400",
    "3450135f2b8d1df1ea392a6cd633fa395034a5c3489cde2ca2dc6d292dea11aa",
    54396,{3,1,1,4,0,0},0x01000706};
constexpr Donor kAdd{"additive",579,
    "8682d758c0051a0f54e586e899d53c40faa884f00501b406857f86280d740c1b",
    "47e0cd17d835e8c97e9e28e50a07a783fab4b3ba31db9083f7a9a3e6e6f195ef",
    "a49fc819fa7ea410d70de5a740582b160660c000cbb08e26624e62bc78bdc55e",
    "b2e3747c79ce4575e5e5dffa881ee92ad762ef783ae32e4bcbfa58cabd6bfd8f",
    "cd80aae13d8c9f45b7c317140f79eb974f78f54070fbfd905bd56b207ec96740",
    79384,{3,2,1,4,0,0},0x01000101};
constexpr Donor kOld{"old53",384,
    "20d0f93a552fb740d8f3e650b3ceed46b00284cd7c33017a5340c51506c79a49",
    "e413a5d1f8ecffb4f709964a31952aa5e377bb4a331dcc399dd1f28468401c87",
    "f70362078fc5b42825bf38ef5059388b2526b1a3479fa9b6e21ea2c20fc7a646",
    "9ac9a54dd8b3bf64470adfc2d886fb61f95f3b558d8a2208fd6909867a728a1a",
    "bb23b3fae316a32b9bfac1847fb16b5ec3da2968749a2e144e608e31f505652e",
    93596,{3,1,1,4,0,0},0x10001};

NativeFsiStateRecipe Recipe(const Donor& d) {
    NativeFsiStateRecipe r;
    r.generic={d.vertex,std::string(d.pixel),d.raster,int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
    r.state.words={0x700762,1,1,3,15,d.blend,0,0,65537,0,0,65537,0,0,65537};
    if(d.event==kOld.event) {r.state.words[0]=0x700766;r.state.words[2]=0;}
    Check(ValidFsiStateRecipe(r),"Invalid actual donor recipe");
    const bool member=NativePackedColorBlendVariant8Tuple(r.generic.vertex,*r.generic.pixel,
        r.generic.raster,r.state,r.generic.color_format,r.generic.depth_format);
    Check(member==(d.event!=kOld.event),"Donor escaped its exact proved cohort");
    return r;
}
std::shared_ptr<FsiStateSpecializationProgram> Program(const std::filesystem::path& cache,const Donor& d,bool foreign) {
    const auto bytes=ReadBytes(cache/(std::string(d.pixel)+".g3fsi"));
    const auto word=[&](size_t at){return uint32_t(bytes.at(at))|(uint32_t(bytes.at(at+1))<<8)|
        (uint32_t(bytes.at(at+2))<<16)|(uint32_t(bytes.at(at+3))<<24);};
    const auto text=[&](size_t at){return std::string(reinterpret_cast<const char*>(bytes.data()+at),64);};
    Check(bytes.size()==400+d.pixel_bytes&&std::memcmp(bytes.data(),"G3FSIC01",8)==0&&
        word(8)==1794&&word(12)*4==d.pixel_bytes,"Donor state-module extent/mask mismatch");
    Check(text(16)==d.pixel&&text(80)==kNativeFsiStateSpecializationShaderVersion,"Donor state-module identity mismatch");
    Bytes signed_bytes(bytes.begin(),bytes.begin()+336);signed_bytes.insert(signed_bytes.end(),bytes.begin()+400,bytes.end());
    Check(ContentHash(signed_bytes)==text(336),"Donor cache checksum mismatch");
    Bytes code(bytes.begin()+400,bytes.end());Check(ContentHash(code)==d.pixel_digest,"Donor SPIR-V is not the exact proved module");
    auto p=std::make_shared<FsiStateSpecializationProgram>();p->source_sha256=d.pixel;
    p->compiler_version=FsiStateSpecializationCompilerVersion();p->variant_version=text(80);
    p->baseline_hlsl_sha256=text(144);p->modified_hlsl_sha256=text(208);p->modified_common_sha256=text(272);
    p->specialization_mask=word(8);p->disk_cache_hit=true;p->spirv=AsWords(code);
    if(foreign)p->spirv.at(2)^=1; // Valid SPIR-V generator word only; effects unchanged, byte certificate false.
    p->compiled_spirv_sha256=ContentHash({reinterpret_cast<const uint8_t*>(p->spirv.data()),p->spirv.size()*4});
    return p;
}
struct Inputs {std::array<Json,2> draw;Json old,resolve;};
Inputs LoadInputs(const std::filesystem::path& inspected,const Donor& d) {
    std::ifstream f(inspected/"scene.json");Json scene;f>>scene;Inputs in;Json alternate;
    for(const auto& op:scene.at("operations")) {
        if(op.at("event")==d.event)in.draw[0]=op;
        if(op.at("event")==971)alternate=op;
        if(op.at("event")==kOld.event)in.old=op;
        if(op.at("event")==549)in.resolve=op;
    }
    const auto match=[&](const Json& op,const Donor& x){return op.value("type","")=="draw"&&op.at("frame")==17672&&
        op.at("vertex_shader")==x.vertex&&op.at("pixel_shader")==x.pixel&&op.at("device_state")==x.state_digest;};
    Check(match(in.draw[0],d)&&match(in.old,kOld),"Actual captured donor identity differs");
    in.draw[1]=in.draw[0];
    if(d.event==kAlpha.event) {
        // Controlled fixture variation only: real A geometry/shader/state,
        // real event971 CPU texture records in matching slots0/1. No fake capture.
        unsigned changed=0;
        for(auto& t:in.draw[1].at("textures"))for(const auto& other:alternate.at("textures"))
            if(t.at("slot")==other.at("slot")){t=other;++changed;}
        Check(changed==2&&in.draw[0].at("textures")!=in.draw[1].at("textures"),"Missing real second texture set");
    }
    for(const auto* draw:{&in.draw[0],&in.draw[1],&in.old})for(const auto& t:draw->at("textures"))
        Check(t.at("source")=="cpu_snapshot"&&t.at("producers").empty(),"Fixture sample is not an independent CPU image");
    Check(in.resolve.value("type","")=="resolve"&&in.resolve.at("source_slot")==4&&
        in.resolve.at("destination").at("base")=="13aa3000"&&in.resolve.at("flags")==4,"Depth resolve donor differs");
    return in;
}
Words Seed() {
    Words seed(10u*1024*1024/4,0x5a5a5a5a);
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x) {
        seed[NativePackedColorPassWord(x,y,true)]=(x+y)&255u; // Depth0; varied stencil must survive unchanged.
        seed[NativePackedColorPassWord(x,y,false)]=0; // Finite black, avoiding a saturation oracle.
    }
    return seed;
}
void CheckNonColorExact(const Words& actual,const Words& seed) {
    Check(actual.size()==10u*1024*1024/4&&actual.size()==seed.size(),"Full EDRAM extent differs");
    constexpr size_t first=720u*1280,last=1440u*1280;
    Check(std::equal(actual.begin(),actual.begin()+first,seed.begin())&&
        std::equal(actual.begin()+last,actual.end(),seed.begin()+last),"Depth/stencil or outside-color bytes changed");
}
uint64_t ColorDifference(const Words& a,const Words& b) {
    Check(a.size()==b.size(),"Comparison extent differs");uint64_t n=0;
    for(size_t at=720u*1280;at<1440u*1280;++at)n+=(a[at]&0x3fffffffu)!=(b[at]&0x3fffffffu);
    return n;
}
uint64_t AlphaWitness(const Words& seed,const Words& a,const Words& b,const Words& ab,const Words& ba) {
    uint64_t n=0;constexpr uint32_t mask=0x3fffffffu;
    for(size_t at=720u*1280;at<1440u*1280;++at) {
        const auto s=seed[at]&mask,x=a[at]&mask,y=b[at]&mask,xy=ab[at]&mask,yx=ba[at]&mask;
        n+=(x!=s&&y!=s&&xy!=x&&xy!=y&&xy!=yx);
    }
    return n;
}
uint64_t AdditiveWitness(const Words& a,const Words& aa,const Words& aaa) {
    uint64_t n=0;
    for(size_t at=720u*1280;at<1440u*1280;++at) {
        bool positive=false,nondecreasing=true;
        for(unsigned shift:{0u,10u,20u}) {
            const auto x=(a[at]>>shift)&1023,y=(aa[at]>>shift)&1023,z=(aaa[at]>>shift)&1023;
            nondecreasing&=x<=y&&y<=z;positive|=0<x&&x<y&&y<z&&z<1023;
        }
        n+=positive&&nondecreasing;
    }
    return n;
}
std::string Digest(const Words& w){return ContentHash({reinterpret_cast<const uint8_t*>(w.data()),w.size()*4});}
void SaveWords(const std::filesystem::path& p,const Words& w) {
    Check(!std::filesystem::exists(p),"Refusing to overwrite fixture artifact");std::ofstream f(p,std::ios::binary);
    f.write(reinterpret_cast<const char*>(w.data()),w.size()*4);Check(bool(f),"Cannot write fixture artifact");
}
enum class Mode {Disabled,Certified,Foreign,ReplacedUncertifiedModule,ReplacedCertifiedModule};
struct Outcome {std::map<std::string,Words> memory;Words resolved;Json queries,report,witness;};
Json GroupDelta(const Json& after,const Json& before) {
    Json delta=after;
    for(auto i=delta.begin();i!=delta.end();++i)if(i.key()!="maximum_group_draws"&&i.value().is_number_unsigned()) {
        const auto old=before.at(i.key()).get<uint64_t>(),now=i.value().get<uint64_t>();
        Check(now>=old,"Group counter moved backwards after warmup");i.value()=now-old;
    }
    return delta;
}

Outcome Run(RenderDevice& device,RenderCommandQueue& queue,const std::filesystem::path& inspected,
    const std::filesystem::path& compiled,const std::filesystem::path& cache,const std::filesystem::path& output,
    const Donor& donor,uint32_t capacity,Mode mode) {
    const bool enabled=mode!=Mode::Disabled,foreign=mode==Mode::Foreign||mode==Mode::ReplacedCertifiedModule;
    const auto in=LoadInputs(inspected,donor);const auto recipe=Recipe(donor);
    const auto dir=output/(std::string(donor.name)+"-"+std::to_string(int(mode)));
    Check(!std::filesystem::exists(dir),"Refusing existing fixture output");std::filesystem::create_directories(dir);
    const auto payload=[&](const std::string& h){return ReadBytes(inspected/(h+".payload.bin"));};
    const auto shader=[&](const std::string& h,bool pixel){return ReadBytes(compiled/(h+(pixel?".fsi.spv":".spv")));};
    auto catalog=std::make_shared<FsiStateSpecializationCatalog>();
    catalog->programs.emplace(donor.pixel,Program(cache,donor,foreign));catalog->programs.emplace(kOld.pixel,Program(cache,kOld,false));
    SceneExecutorConfig config;config.native_fsi=config.ordered_pixel_interlock_enabled=true;
    config.batch_transfers=config.unified_gpu_queue=config.async_gpu_submissions=true;
    config.max_draw_batch_size=16;config.async_submission_capacity=capacity;
    config.defer_native_queries=config.retire_completed_query_prefixes=true;
    config.packed_color_render_pass_groups=true;config.packed_color_blend_pass_groups=enabled;
    config.specialize_fsi_pipeline_state=config.fsi_state_material_composition=true;
    config.fsi_state_programs=catalog;config.test_retained_resource_pool_diagnostics=true;
    config.payload=payload;config.capture=inspected;config.compiled=compiled;
    config.shader=[&](const std::string& h){return shader(h,false);};config.fsi_shader=[&](const std::string& h){return shader(h,true);};
    SceneExecutor executor(config,&device,&queue);
    for(const auto* d:{&donor,&kOld}) {
        const auto vs=shader(d->vertex,false);Check(ContentHash(vs)==d->vertex_digest,"Actual vertex byte pin mismatch");
        const auto r=Recipe(*d);executor.PrewarmFsiPipeline(r.generic,AsWords(vs),AsWords(shader(d->pixel,true)));
        Check(executor.PrewarmFsiStatePipeline(r,AsWords(vs)),"Actual donor state PSO was not prepared");
    }
    Outcome out;out.witness={{"donor",donor.name},{"mode",int(mode)},{"event",donor.event},{"capacity",capacity},
        {"texture_variation_event",donor.event==kAlpha.event?Json(971):Json(nullptr)},{"controlled_fixture_not_capture_replay",true}};
    if(mode==Mode::ReplacedUncertifiedModule||mode==Mode::ReplacedCertifiedModule) {
        const bool replacement_foreign=mode==Mode::ReplacedUncertifiedModule;
        const auto replacement=Program(cache,donor,replacement_foreign);
        const auto w=executor.ReplacePackedColorBlendVariant8ModuleForFixture(recipe.generic,recipe.state,replacement->spirv);
        Check(w.old_shader&&w.new_shader&&w.old_shader!=w.new_shader&&w.pipeline&&w.distinct_vulkan_modules&&
            w.pipeline_unchanged&&w.frontier_unchanged&&w.mutation_unchanged&&w.accepted_work==w.completed_work&&
            w.old_module_certified==!foreign&&w.new_module_certified==!replacement_foreign&&
            w.pipeline_certified_before==!foreign&&w.pipeline_certified_after==!foreign&&w.displaced_bytes==donor.pixel_bytes,
            "Independent actual current-module/creation-PSO witness failed");
        out.witness["replacement"]={{"old_shader",w.old_shader},{"new_shader",w.new_shader},{"pipeline",w.pipeline},
            {"distinct_vulkan_modules",w.distinct_vulkan_modules},{"old_module_certified",w.old_module_certified},
            {"new_module_certified",w.new_module_certified},{"pipeline_certified_before",w.pipeline_certified_before},
            {"pipeline_certified_after",w.pipeline_certified_after},{"pipeline_unchanged",w.pipeline_unchanged},
            {"frontier_unchanged",w.frontier_unchanged},{"mutation_unchanged",w.mutation_unchanged}};
    }
    const auto seed=Seed();const auto reset=[&]{executor.InitializeEdram({reinterpret_cast<const uint8_t*>(seed.data()),seed.size()*4});};
    const auto read=[&]{executor.FlushGpuWork();return ReadSceneEdram(device,queue,executor.ExportState().edram);};
    uint64_t event=0;const auto record=[&](Json op){op["event"]=++event;op["frame"]=17;executor.FlushCommittedState();executor.Execute(op);};
    const auto draw=[&](unsigned i){record(in.draw.at(i));};
    const auto query=[&](bool begin,uint64_t generation){record({{"type",begin?"query_begin":"query_end"},
        {"query_generation",generation},{"query_handle",0x1000},{"report_address",begin?0x2020:0x2000}});};
    // Cold CPU uploads require standalone lists. Warm all immutable donor
    // inputs in this Scene, retain raw reports, and assert subsequent deltas.
    // EDRAM resets do not invalidate ResolveMemory's physical CPU textures.
    reset();draw(0);draw(1);record(in.old);executor.FlushGpuWork();
    const auto warmup_report=executor.report();
    Check(!executor.HasPendingGpuWork(),"Donor warmup did not complete");
    const auto sequence=[&](std::string name,std::initializer_list<unsigned> order) {
        reset();for(auto i:order)draw(i);auto memory=read();CheckNonColorExact(memory,seed);
        SaveWords(dir/(name+".bin"),memory); // Preserve failed nonvacuity evidence too.
        out.memory.emplace(std::move(name),std::move(memory));
    };
    sequence("A",{0});
    if(donor.event==kAlpha.event) {
        sequence("B",{1});sequence("AB",{0,1});sequence("BA",{1,0});
        const auto n=AlphaWitness(seed,out.memory.at("A"),out.memory.at("B"),out.memory.at("AB"),out.memory.at("BA"));
        out.witness["alpha_same_words_noncommutative_and_both_contributions"]=n;
        std::ofstream(dir/"nonvacuity.json")<<out.witness.dump(2)<<'\n';
        Check(n>0,"Alpha overlap is vacuous or only last-writer overwrite");
    } else {
        sequence("AA",{0,0});sequence("AAA",{0,0,0});
        const auto n=AdditiveWitness(out.memory.at("A"),out.memory.at("AA"),out.memory.at("AAA"));
        out.witness["additive_same_words_three_unsaturated_steps"]=n;
        std::ofstream(dir/"nonvacuity.json")<<out.witness.dump(2)<<'\n';
        Check(n>0,"Additive overlap is vacuous, overwrite-only or saturated");
    }
    // Candidate path runs uninterrupted across the16-member cap. The ordinary
    // baseline alone snapshots the penultimate draw, proving observable final work.
    reset();Words prior;
    constexpr uint32_t repeated_draws=33;
    for(uint32_t n=0;n<repeated_draws;++n) {
        if(mode==Mode::Disabled&&n+1==repeated_draws)prior=read();
        draw(donor.event==kAlpha.event?n%2:0);
    }
    out.memory["long-before-query"]=read();CheckNonColorExact(out.memory.at("long-before-query"),seed);
    SaveWords(dir/"long-before-query.bin",out.memory.at("long-before-query"));
    if(mode==Mode::Disabled) {
        SaveWords(dir/"long-penultimate.bin",prior);
        const auto n=ColorDifference(prior,out.memory.at("long-before-query"));
        out.witness["long_final_draw_changed_rgb_words"]=n;
        std::ofstream(dir/"nonvacuity.json")<<out.witness.dump(2)<<'\n';
        Check(n>0,"Final repeated draw has no observable color contribution");
    }
    // Preserve all pre-query memory before any queried redraw can hide lost work.
    draw(0);draw(1);query(true,1);query(false,1);
    query(true,2);draw(0);draw(1);query(false,2);
    auto fetch=in.resolve.at("destination_fetch").get<std::array<uint32_t,6>>();constexpr uint32_t address=0x13aa3000;
    fetch[1]=(fetch[1]&0xfff)|address;const auto ranges=TextureMemoryRanges(DescribeTexture(fetch),false);
    uint64_t end=address;for(const auto& r:ranges){Check(r.first>=address,"Unexpected depth resolve range");end=std::max(end,uint64_t(r.first)+r.second);}
    Check(end>address&&end-address<=8u*1024*1024,"Depth resolve witness unbounded");const uint32_t size=uint32_t(end-address);
    for(const auto* input:{&in.draw[0],&in.draw[1],&in.old})for(const auto& t:input->at("textures"))for(const bool mips:{false,true})
        for(const auto& r:TextureMemoryRanges(DescribeTexture(t.at("fetch").get<std::array<uint32_t,6>>()),mips,true))
            Check(uint64_t(r.first)+r.second<=address||uint64_t(r.first)>=end,
                "Resolve destination aliases a warmed donor CPU texture");
    executor.InitializeMemory(address,Bytes(size,0xa5));draw(0);draw(1);
    const auto before=executor.ObserveAcceptedWork();record(in.resolve);const auto after=executor.ObserveAcceptedWork();
    Check(after.available&&before.available&&after.transfers>before.transfers,"Depth conversion accepted no transfer");
    executor.FlushGpuWork();out.resolved=ReadPhysicalRange(device,queue,executor.ExportState().physical,address,size);
    Check(out.resolved!=Words(size/4,0xa5a5a5a5),"Depth conversion output vacuous");
    for(unsigned n=0;n<19;++n)draw(donor.event==kAlpha.event?n%2:0);
    query(true,3);draw(0);query(false,3);out.memory["after-query"]=read();CheckNonColorExact(out.memory.at("after-query"),seed);
    out.queries=Json::array();for(const auto& q:executor.TakeCompletedQueries())
        out.queries.push_back({q.frame,q.generation,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});
    Check(out.queries.size()==3&&out.queries[0][6]==0&&out.queries[0][7]==0&&out.queries[1][6]==2&&
        out.queries[1][7].get<uint64_t>()>0&&out.queries[2][7].get<uint64_t>()>0,"Real query witness is vacuous");
    // Old53 and new19 alternate on the same primary surfaces. Old depth writes
    // are intentionally permitted only here, separately from the unchanged-depth controls.
    reset();for(unsigned n=0;n<19;++n){record(in.old);draw(n%2);}out.memory["mixed-old53-new19"]=read();
    out.report=executor.report();
    Json deltas={{"packed_color_render_pass_groups",GroupDelta(out.report.at("packed_color_render_pass_groups"),warmup_report.at("packed_color_render_pass_groups"))},
        {"packed_color_blend_pass_groups",GroupDelta(out.report.at("packed_color_blend_pass_groups"),warmup_report.at("packed_color_blend_pass_groups"))}};
    Json memories=Json::object();for(const auto& [name,w]:out.memory)memories[name]={{"bytes",w.size()*4},{"sha256",Digest(w)}};
    for(const auto* name:{"after-query","mixed-old53-new19"})SaveWords(dir/(std::string(name)+".bin"),out.memory.at(name));
    SaveWords(dir/"resolved.bin",out.resolved);
    out.witness["memories"]=memories;out.witness["resolved"]={{"bytes",out.resolved.size()*4},{"sha256",Digest(out.resolved)}};
    {std::ofstream f(dir/"report.json");f<<Json{{"witness",out.witness},{"queries",out.queries},{"report",out.report},
        {"warmup_report",warmup_report},{"group_deltas",deltas}}.dump(2)<<'\n';
     Check(bool(f),"Fixture report write failed");}
    const auto& work=out.report.at("gpu_work_queue");
    Check(work.at("accepted_work")==work.at("completed_work")&&!executor.HasPendingGpuWork()&&
        work.at("maximum_lists").get<uint64_t>()<=capacity,"Fixture FIFO ownership incomplete");
    Check(out.report.at("fsi_state_material_composition").at("query_refusals")==3&&
        out.report.at("fsi_state_specialization").at("queried_selected")==0,"Queried canonical fallback changed");
    const auto& all=deltas.at("packed_color_render_pass_groups");
    const auto& blend=deltas.at("packed_color_blend_pass_groups");
    const auto count=[](const Json& s,const char* name){return s.at(name).get<uint64_t>();};
    Check(all.at("enabled")==true&&blend.at("enabled")==enabled,"Unexpected grouping controls");
    Check(count(all,"variant8_selected")==19&&count(all,"variant8_module_refusals")==0&&
        count(all,"variant8_pipeline_refusals")==0,"Existing old53 positive path changed");
    Check(count(all,"selected")==count(all,"closed_render_pass_groups")+count(all,"render_passes_avoided")&&
        count(all,"eligible")==count(all,"selected")+count(all,"preparation_refusals")&&
        count(all,"candidates")==count(all,"eligible")+count(all,"refusals")&&
        count(all,"maximum_group_draws")<=16,"Parent group conservation failed");
    Check(count(blend,"candidates")==count(blend,"eligible")+count(blend,"refusals")&&
        count(blend,"eligible")==count(blend,"selected")+count(blend,"preparation_refusals")&&
        count(blend,"selected")==count(blend,"alpha_selected")+count(blend,"additive_selected")&&
        count(blend,"query_refusals")==0&&count(blend,"timestamp_refusals")==0&&
        count(blend,"preparation_refusals")==0,"Blend group conservation/fallback changed");
    constexpr uint64_t eligible_blend_draws=6+repeated_draws+2+2+19+19;
    Check(count(blend,"candidates")==uint64_t(enabled)*eligible_blend_draws,"Exact blend donor coverage changed");
    if(mode==Mode::Certified) {
        Check(count(blend,"selected")==eligible_blend_draws&&count(blend,"refusals")==0&&
            count(blend,"render_passes_avoided")>16&&count(all,"maximum_group_draws")==16&&
            count(all,"sampled_barriers_avoided")>16,"Actual sampled blend continuation/cap16 absent");
        Check(count(blend,"old_to_blend_continued")>0&&count(blend,"blend_to_old_continued")>0,
            "Actual compatible old53/new19 continuation absent");
        Check(count(blend,donor.event==kAlpha.event?"alpha_selected":"additive_selected")==eligible_blend_draws,
            "Wrong exact blend operation selected");
    } else Check(count(blend,"selected")==0&&count(blend,"render_passes_avoided")==0&&
        count(blend,"old_to_blend_continued")==0&&count(blend,"blend_to_old_continued")==0,
        "Disabled or uncertified blend owner continued a pass");
    const bool bad_module=mode==Mode::Foreign||mode==Mode::ReplacedUncertifiedModule;
    const bool bad_pipeline=mode==Mode::Foreign||mode==Mode::ReplacedCertifiedModule;
    Check(count(blend,"module_refusals")==uint64_t(bad_module)*eligible_blend_draws&&
        count(blend,"pipeline_refusals")==uint64_t(bad_pipeline)*eligible_blend_draws,
        "Current-module and creation-PSO refusal controls were not independent");
    draw(0);draw(1);Check(executor.HasPendingDraws(),"Accepted partial shutdown witness absent");
    return out;
}
void Suite(RenderDevice& device,RenderCommandQueue& queue,const std::filesystem::path& inspected,
    const std::filesystem::path& compiled,const std::filesystem::path& cache,const std::filesystem::path& output,uint32_t capacity) {
    Json summaries=Json::array();
    for(const auto* d:{&kAlpha,&kAdd}) {
        const auto baseline=Run(device,queue,inspected,compiled,cache,output,*d,capacity,Mode::Disabled);
        for(const auto mode:{Mode::Certified,Mode::Foreign,Mode::ReplacedUncertifiedModule,Mode::ReplacedCertifiedModule}) {
            const auto on=Run(device,queue,inspected,compiled,cache,output,*d,capacity,mode);
            Check(on.memory==baseline.memory&&on.resolved==baseline.resolved&&on.queries==baseline.queries,
                "Blend full-memory, actual resolve or query parity differs");
            summaries.push_back({{"donor",d->name},{"mode",int(mode)},{"full_memory_exact",true},{"resolve_exact",true},{"queries_exact",true}});
        }
    }
    std::ofstream f(output/"summary.json");f<<Json{{"schema","packed-color-blend-scene-fixture-v1"},{"passed",true},
        {"checks",checks},{"capacity",capacity},{"comparisons",summaries},{"performance_claim",false}}.dump(2)<<'\n';
    Check(bool(f),"Fixture summary write failed");
}
}
int main(int argc,char** argv)try {
    Check(argc==6,"Usage: packed-color-blend-pass-scene-test inspected compiled cache output capacity");
    const uint32_t capacity=std::stoul(argv[5]);Check(capacity==64,"Fixture requires the bounded64 capacity");
    Check(!std::filesystem::exists(argv[4]),"Refusing existing output root");std::filesystem::create_directories(argv[4]);
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    Suite(device,*queue,argv[1],argv[2],argv[3],argv[4],capacity);
    std::cout<<"PASS: "<<checks<<" blend fixture checks; exact full-memory/queries and actual owner controls\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
