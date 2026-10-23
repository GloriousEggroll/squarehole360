#include "renderer/native/scene_executor.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/fsi_state_specialization_compiler.h"
#include "renderer/native/fsi_state_specialization_recipe.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

using namespace plume;
using namespace gow3::renderer;
namespace fs=std::filesystem;
namespace {
using Json=nlohmann::json;
using Words=std::vector<uint32_t>;
using QueryOracle=std::vector<std::array<uint64_t,8>>;
constexpr uint32_t kEdramBytes=10u*1024*1024,kSeed=0xe000005a;
constexpr std::array<uint32_t,2> kDepthControls{0x700776,0x700762};

void Require(bool value,std::string_view message) {
    if(!value)throw std::runtime_error(std::string(message));
}
std::vector<uint8_t> ReadContainer(const fs::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    const auto size=input.tellg();
    Require(bool(input)&&size>0&&size<=16u*1024*1024,"Invalid bounded vertex container");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));input.seekg(0);
    Require(bool(input.read(reinterpret_cast<char*>(bytes.data()),size)),"Truncated vertex container");
    return bytes;
}
std::vector<uint8_t> ByteVector(std::span<const uint32_t> words) {
    std::vector<uint8_t> bytes(words.size_bytes());
    std::memcpy(bytes.data(),words.data(),bytes.size());return bytes;
}
// Same full-capacity Scene readback as the existing read-only query GPU fixture.
// A sampled pixel cannot establish preservation of generic packed ownership.
Words ReadEdram(RenderDevice& device,RenderCommandQueue& queue,const NativeMemoryState& state) {
    Require(state.capacity==kEdramBytes,"Unexpected full EDRAM capacity");
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(state.capacity));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    const auto cmd=static_cast<VulkanCommandList&>(*commands).vk;
    auto& buffer=static_cast<VulkanBuffer&>(*readback);
    VkBufferMemoryBarrier dependency{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    dependency.buffer=state.buffer;dependency.size=VK_WHOLE_SIZE;
    dependency.srcQueueFamilyIndex=dependency.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    dependency.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
        0,nullptr,1,&dependency,0,nullptr);
    const VkBufferCopy range{0,0,state.capacity};vkCmdCopyBuffer(cmd,state.buffer,buffer.vk,1,&range);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,
        1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto* mapped=readback->map();Require(mapped,"Full EDRAM readback mapping failed");
    Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
        "Full EDRAM readback invalidate failed");
    Words result(state.capacity/4);std::memcpy(result.data(),mapped,state.capacity);readback->unmap();return result;
}
struct Budgets {const char* name;bool enabled,admit;uint32_t pipelines,modules;uint64_t bytes;};
struct Outcome {Words edram;QueryOracle queries;Json diagnostic;};
Json Owners(const Json& report) {
    const auto& pipelines=report.at("pipeline_cache");const auto& shaders=report.at("shader_cache");
    return {{"pipelines",pipelines.at("resident")},{"pipeline_creations",pipelines.at("creations")},
        {"pipeline_misses",pipelines.at("misses")},{"pipeline_evictions",pipelines.at("evictions")},
        {"pipeline_drains",pipelines.at("pressure_drains")},{"modules",shaders.at("resident")},
        {"module_creations",shaders.at("creations")},{"module_misses",shaders.at("misses")},
        {"module_evictions",shaders.at("evictions")},{"module_drains",shaders.at("pressure_drains")},
        {"module_bytes",shaders.at("spirv_bytes")}};
}
Outcome Run(RenderDevice& device,RenderCommandQueue& queue,
            const CompiledNativeShader& vertex,const CompiledNativeShader& canonical,
            const std::shared_ptr<const FsiStateSpecializationCatalog>& catalog,const Budgets& budget) {
    const uint64_t generic_bytes=(vertex.spirv.size()+canonical.spirv.size())*sizeof(uint32_t);
    const uint64_t derived_bytes=catalog->depth_only->spirv.size()*sizeof(uint32_t);
    NativeFsiPipelineRecipe generic{std::string(kReadonlyQueryVertexSource),std::nullopt,
        {uint32_t(RenderCullMode::NONE),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
         uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
    std::array<NativeFsiStateRecipe,2> recipes;
    for(size_t i=0;i<recipes.size();++i) {
        NativeFsiConstants state;state.pitch=640;state.depth_base=1440;
        state.depth_control=kDepthControls[i];state.depth_format=1;
        recipes[i]={generic,NativeFsiStateSpecialization::From(state)};
        Require(ValidFsiStateRecipe(recipes[i]),"Invalid exact final-state fixture recipe");
    }
    Require(recipes[0].state!=recipes[1].state,"Two exact state keys collapsed");
    std::map<std::string,std::vector<uint8_t>> payloads;
    const auto add=[&](std::vector<uint8_t> bytes) {
        auto key=ContentHash(bytes);payloads.emplace(key,std::move(bytes));return key;
    };
    const auto floats=[&](std::initializer_list<float> values) {
        std::vector<uint8_t> bytes(values.size()*4);std::memcpy(bytes.data(),values.begin(),bytes.size());
        return add(std::move(bytes));
    };
    // Real pinned user VS fetch contract, reused from RoomScene in the read-only
    // fixture. Streams select vertices, translation/scale and a full triangle;
    // four guest constant registers form the identity matrix at raster depth .5.
    const std::array<std::string,3> geometry_ids{
        add({0,1,2,0}),floats({0,0,0,1,1,1}),floats({-1,-1,.5f,3,-1,.5f,-1,3,.5f})};
    std::vector<uint8_t> state(0x2a80);
    const auto word=[](std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(24-8*i));
    };
    const auto number=[&](std::vector<uint8_t>& bytes,size_t at,float value) {
        word(bytes,at,std::bit_cast<uint32_t>(value));
    };
    for(unsigned i=0;i<4;++i)number(state,0x780+16*i+4*i,1.f);
    word(state,0x2880,640);word(state,0x28bc,640|(360<<16));word(state,0x28c4,0x80000000);
    word(state,0x28c8,640|(360<<16));word(state,0x28dc,0);word(state,0x2944,1<<19);word(state,0x294c,0x43f);
    word(state,0x2948,(1<<11)|(1<<12));word(state,0x29c0,1);
    number(state,0x2908,320);number(state,0x290c,320);number(state,0x2910,-180);
    number(state,0x2914,180);number(state,0x2918,1);
    const Json depth{{"depth",true},{"edram_base",1440},{"format",1},{"format_info",0},
        {"surface_info",640},{"width",640},{"height",360}};
    Json geometry=Json::array();
    for(unsigned i=0;i<3;++i)geometry.push_back({{"kind",14},{"fetch_slot",93+i},{"endian",0},
        {"address","00007000"},{"bytes",payloads.at(geometry_ids[i]).size()},{"sha256",geometry_ids[i]}});
    uint64_t shader_provider_calls=0,builtin_provider_calls=0,observer_calls=0;
    std::array<std::atomic<uint64_t>,2> observed{};
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.specialize_fsi_pipeline_state=budget.enabled;config.fsi_state_programs=catalog;
    config.max_pipeline_cache_entries=budget.pipelines;config.max_shader_cache_entries=budget.modules;
    config.max_shader_cache_bytes=budget.bytes;config.batch_transfers=true;config.unified_gpu_queue=true;
    config.async_gpu_submissions=true;config.max_draw_batch_size=16;config.defer_native_queries=true;
    config.payload=[&](const std::string& key){return payloads.at(key);};
    config.shader=[&](const std::string& key) {
        ++shader_provider_calls;Require(key==kReadonlyQueryVertexSource,"Unexpected guest shader source");
        return ByteVector(vertex.spirv);
    };
    config.fsi_depth_shader=[&]{++builtin_provider_calls;return ByteVector(canonical.spirv);};
    config.observe_fsi_state=[&](const NativeFsiStateRecipe& recipe)->std::atomic<uint64_t>* {
        ++observer_calls;
        for(size_t i=0;i<recipes.size();++i)if(recipe==recipes[i])return &observed[i];
        throw std::runtime_error("Accepted Scene final-state recipe differs from prewarmed exact key");
    };
    SceneExecutor scene(config,&device,&queue);
    Require(!scene.report().at("device").at("software").get<bool>(),
        "Hardware budget fixture selected a software Vulkan device: "+scene.report().at("device").dump());
    scene.PrewarmFsiPipeline(generic,vertex.spirv,canonical.spirv);
    const auto generic_owners=Owners(scene.report());
    Require(generic_owners.at("pipelines")==1&&generic_owners.at("modules")==2&&
        generic_owners.at("module_bytes")==generic_bytes&&generic_owners.at("pipeline_evictions")==0&&
        generic_owners.at("module_evictions")==0,"Generic startup owners are not resident");
    if(budget.enabled)for(const auto& recipe:recipes)
        Require(scene.PrewarmFsiStatePipeline(recipe,vertex.spirv)==budget.admit,
            "Real Scene optional budget admission differed from the independent case");
    const auto startup_owners=Owners(scene.report());
    if(!budget.admit)Require(startup_owners==generic_owners,
        "Refused optional startup changed or evicted generic owners");
    else {
        Require(startup_owners.at("pipelines")==3&&startup_owners.at("pipeline_creations")==3&&
            startup_owners.at("modules")==3&&startup_owners.at("module_creations")==3&&
            startup_owners.at("module_bytes")==generic_bytes+derived_bytes&&
            startup_owners.at("pipeline_evictions")==0&&startup_owners.at("module_evictions")==0,
            "Two exact states failed to share one derived module or displaced generic owners");
        // Existing resident states must remain usable when both hard counts are full.
        for(const auto& recipe:recipes)Require(scene.PrewarmFsiStatePipeline(recipe,vertex.spirv),
            "Resident exact state was refused at its own full hard budget");
        Require(Owners(scene.report())==startup_owners,"Repeated exact state prewarm recreated an owner");
    }
    // Probe the original keys again, proving that their actual cached handles
    // still exist after optional admission/refusal rather than merely comparing
    // total occupancy. The miss/creation counters must remain unchanged.
    scene.PrewarmFsiPipeline(generic,vertex.spirv,canonical.spirv);
    Require(Owners(scene.report())==startup_owners,"Optional startup lost an original generic owner");
    Require(scene.mutation_serial()==0&&!scene.HasPendingGpuWork(),"Prewarm created guest GPU ownership");
    scene.InitializeEdram(ByteVector(Words(kEdramBytes/4,kSeed)));
    uint64_t event=0;
    for(unsigned i=0;i<4;++i) {
        auto raw=state;word(raw,0x2934,kDepthControls[i%2]);
        // Dynamic bias deliberately differs between the two occurrences of each
        // state. It must stay live while exact PSO state remains shared.
        const float bias=i==0?-.125f:i==2?.125f:0.f;
        number(raw,0x2a54,bias);number(raw,0x2a5c,bias);const auto state_id=add(std::move(raw));
        const uint32_t report_address=0x2000+64*i;
        scene.Execute({{"type","query_begin"},{"frame",1},{"event",++event},{"query_generation",i+1},
            {"query_handle",0x1000+32*i},{"report_address",report_address+32}});
        scene.Execute({{"type","draw"},{"frame",1},{"event",++event},{"function","DrawPrimitive"},
            {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",kReadonlyQueryVertexSource},
            {"pixel_shader",nullptr},{"surfaces",{{"10",depth}}},{"geometry",geometry},
            {"vertex_slots",{93,94,95}},{"textures",Json::array()}});
        scene.Execute({{"type","query_end"},{"frame",1},{"event",++event},{"query_generation",i+1},
            {"query_handle",0x1000+32*i},{"report_address",report_address}});
    }
    scene.FlushGpuWork();Outcome result;
    const auto completed=scene.TakeCompletedQueries();Require(completed.size()==4,"Missing completed Native query");
    constexpr std::array<uint64_t,4> samples{230400,230400,230400,0};
    for(unsigned i=0;i<completed.size();++i) {
        const auto& query=completed[i];const auto& count=query.result;
        Require(query.frame==1&&query.generation==i+1&&query.end_event==3u*(i+1)&&
            count.handle==0x1000+32*i&&count.begin_address==0x2020+64*i&&
            count.end_address==0x2000+64*i&&count.draws==1&&count.samples==samples[i],
            "Accepted Native query identity, depth pass samples or dynamic bias changed");
        result.queries.push_back({query.frame,query.generation,query.end_event,count.handle,
            count.begin_address,count.end_address,count.draws,count.samples});
    }
    result.edram=ReadEdram(device,queue,scene.ExportState().edram);
    const auto report=scene.report();const auto final_owners=Owners(report);
    Require(final_owners==startup_owners&&shader_provider_calls==0&&builtin_provider_calls==0,
        "Accepted draws recompiled, recreated or evicted a prewarmed generic/optional owner");
    const auto& work=report.at("gpu_work_queue");
    Require(report.at("draws")==4&&work.at("draw_lists")==4&&
        work.at("accepted_work")==work.at("completed_work")&&!scene.HasPendingGpuWork()&&
        scene.DeferredQueryCount()==0,"Accepted queried Native work did not fully retire");
    const auto& stats=report.at("fsi_state_specialization");
    Require(stats.at("candidates")==4&&stats.at("prepared")==uint64_t(budget.admit?2:0)&&
        stats.at("modules_created")==uint64_t(budget.admit?1:0)&&
        stats.at("prewarm_refusals")==uint64_t(budget.enabled&&!budget.admit?2:0)&&
        stats.at("selected")==uint64_t(budget.admit?4:0)&&stats.at("queried_selected")==stats.at("selected")&&
        stats.at("residency_refusals")==uint64_t(budget.enabled&&!budget.admit?4:0)&&
        stats.at("observation_refusals")==0,"Optional residency/refusal coverage is vacuous or changed");
    Require(stats.at("resident_handoffs")==uint64_t(budget.admit?4:0)&&
        stats.at("coverage_pointer_hits")==uint64_t(budget.admit?2:0)&&
        stats.at("module_bytes")==uint64_t(budget.admit?derived_bytes:0),
        "Resident handoff, stable coverage pointer or module bytes lack actual expected execution");
    const auto& coverage=stats.at("coverage");Require(coverage.size()==2&&observer_calls==2,
        "Accepted coverage omitted an exact final state");
    std::array<bool,2> seen{};
    for(const auto& row:coverage) {
        const auto words=row.at("fsi_state").get<std::array<uint32_t,15>>();size_t index=recipes.size();
        for(size_t i=0;i<recipes.size();++i)if(words==recipes[i].state.words)index=i;
        Require(index<recipes.size()&&!seen[index]&&row.at("vertex")==generic.vertex&&
            row.at("pixel").is_null()&&row.at("raster")==generic.raster&&
            row.at("candidates")==2&&row.at("queried_candidates")==2&&
            row.at("selected")==uint64_t(budget.admit?2:0)&&row.at("queried_selected")==row.at("selected"),
            "Accepted generic fallback or exact selected state lacks positive queried coverage");
        seen[index]=true;
    }
    Require(observed[0].load()==2&&observed[1].load()==2,"Accepted hot observations lost counts");
    Require(std::any_of(result.edram.begin(),result.edram.end(),[](uint32_t value){return value!=kSeed;})&&
        std::all_of(result.edram.begin(),result.edram.end(),[](uint32_t value){return (value&255u)==0x5a;}),
        "Depth writes are vacuous or corrupted preserved packed stencil bytes");
    result.diagnostic={{"case",budget.name},{"budgets",{{"pipelines",budget.pipelines},
        {"modules",budget.modules},{"spirv_bytes",budget.bytes}}},{"enabled",budget.enabled},
        {"admitted",budget.admit},{"device",report.at("device")},{"generic_owners",generic_owners},
        {"startup_owners",startup_owners},{"final_owners",final_owners},{"stats",stats},
        {"queries",result.queries},{"full_edram_bytes",kEdramBytes},{"full_edram_sha256",ContentHash(ByteVector(result.edram))},
        {"shader_provider_calls",shader_provider_calls},{"builtin_provider_calls",builtin_provider_calls}};
    return result;
}
}
int main(int argc,char** argv)try {
    if(argc!=3)throw std::runtime_error("Usage: gow3-native-fsi-state-specialization-budget-gpu-test pinned-vertex-container NEW-cache-directory");
    const auto source=ReadContainer(argv[1]);Require(ContentHash(source)==kReadonlyQueryVertexSource,
        "Fixture requires the actual pinned user-owned vertex container");
    const fs::path cache=argv[2];Require(!fs::exists(cache),"Fixture cache must be new");fs::create_directories(cache);
    NativeShaderCompiler compiler(cache/"canonical");
    const auto vertex=compiler.Get(source),canonical=compiler.GetDepthOnlyFragmentInterlock();
    const auto program=TryPrepareDepthOnlyFsiStateSpecialization(compiler,cache/"derived");
    Require(vertex->stage==NativeShaderStage::Vertex&&vertex->mode==NativeShaderMode::Raster&&
        vertex->source_sha256==kReadonlyQueryVertexSource&&ContentHash(ByteVector(vertex->spirv))==kReadonlyQueryVertexSpirv&&
        canonical->stage==NativeShaderStage::Pixel&&canonical->mode==NativeShaderMode::FragmentInterlock&&
        ContentHash(ByteVector(canonical->spirv))==kReadonlyQueryBuiltinSpirv,
        "Actual compiled generic VS/builtin differs from the independent raster/query oracle");
    Require(program&&program->depth_only&&program->source_sha256==canonical->source_sha256&&
        program->compiler_version==NativeShaderCompiler::CacheVersion()&&
        program->variant_version==FsiStateSpecializationCompilerVersion()&&
        program->specialization_mask==canonical->specialization_mask&&program->spirv!=canonical->spirv&&
        ContentHash(ByteVector(program->spirv))==program->compiled_spirv_sha256,
        "Real compiled builtin derived program lacks canonical identity and source proof");
    auto mutable_catalog=std::make_shared<FsiStateSpecializationCatalog>();mutable_catalog->depth_only=program;
    const std::shared_ptr<const FsiStateSpecializationCatalog> catalog=mutable_catalog;
    const uint64_t generic_bytes=(vertex->spirv.size()+canonical->spirv.size())*sizeof(uint32_t);
    const uint64_t all_bytes=generic_bytes+program->spirv.size()*sizeof(uint32_t);
    Require(all_bytes<=64u*1024*1024&&program->spirv.size()*sizeof(uint32_t)<=16u*1024*1024,
        "Fixture modules exceed shipping hard bounds");
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    Require(bool(queue),"Native fixture queue unavailable");
    const auto baseline=Run(device,*queue,*vertex,*canonical,catalog,{"generic",false,false,3,3,all_bytes});
    std::cout<<"FSI_STATE_BUDGET_GPU "<<baseline.diagnostic.dump()<<'\n';
    for(const Budgets& budget:std::array<Budgets,4>{{
        {"pipeline_limit",true,false,1,3,all_bytes},
        {"module_limit",true,false,3,2,all_bytes},
        {"byte_limit",true,false,3,3,generic_bytes},
        {"positive_two_states_one_module",true,true,3,3,all_bytes}}}) {
        const auto candidate=Run(device,*queue,*vertex,*canonical,catalog,budget);
        Require(candidate.edram==baseline.edram&&candidate.queries==baseline.queries,
            "Optional refusal/selection changed full packed memory or completed Native query transcript");
        Require(candidate.diagnostic.at("device")==baseline.diagnostic.at("device"),"Fixture GPU device changed");
        std::cout<<"FSI_STATE_BUDGET_GPU "<<candidate.diagnostic.dump()<<'\n';
    }
    std::cout<<Json{{"status","PASS"},{"real_scene_cases",5},{"query_oracles_per_case",4},
        {"full_edram_bytes_per_case",kEdramBytes},{"canonical_version",NativeShaderCompiler::CacheVersion()},
        {"variant_version",program->variant_version},{"derived_builtin_sha256",program->compiled_spirv_sha256},
        {"device",baseline.diagnostic.at("device")}}.dump()<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
