#include "renderer/native/scene_executor.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/content_hash.h"
#include <bit>
#include <map>
#include "renderer/native/fsi_device.h"
#include "renderer/native/query_tracker.h"
#include "renderer/native/pipeline_prewarm.h"
#include "fsi_vertex_spv.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace plume;
using namespace gow3::renderer;
namespace {
using Words=std::vector<uint32_t>;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
Words Read(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    Check(f&&f.tellg()>=20&&f.tellg()%4==0,"Invalid fixture module");
    Words v(size_t(f.tellg())/4);f.seekg(0);
    Check(bool(f.read(reinterpret_cast<char*>(v.data()),v.size()*4)),"Truncated fixture module");return v;
}
void Upload(RenderBuffer& buffer,const void* data,size_t bytes) {
    auto* destination=buffer.map();Check(destination,"Fixture mapping failed");
    std::memcpy(destination,data,bytes);auto& vk=static_cast<VulkanBuffer&>(buffer);
    Check(vmaFlushAllocation(vk.device->allocator,vk.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Fixture flush failed");buffer.unmap();
}
struct Draw {
    bool readonly=true;
    uint32_t control=0x700762,base=1440,instances=1;
    float front_bias=0,back_bias=0;
    RenderRect scissor{0,0,640,360};
};
struct Outcome {Words bytes;std::vector<uint64_t> samples;};
class Fixture {
    static constexpr size_t kBytes=10u*1024*1024;
    RenderDevice& device_;RenderCommandQueue& queue_;
    std::unique_ptr<RenderBuffer> storage_;
    std::unique_ptr<RenderShader> vertex_,original_,readonly_;
    std::unique_ptr<RenderPipelineLayout> layout_;
    std::unique_ptr<RenderPipeline> original_pipeline_,readonly_pipeline_;
    std::unique_ptr<RenderDescriptorSet> descriptors_;
    std::unique_ptr<RenderTexture> target_;
    std::unique_ptr<RenderFramebuffer> framebuffer_;
    NativeQueryTracker queries_;
    struct Slot {std::unique_ptr<RenderBuffer> constants;std::unique_ptr<RenderCommandList> commands;};
    std::vector<Slot> slots_;
public:
    Fixture(RenderDevice& device,RenderCommandQueue& queue,std::span<const uint32_t> original,
            std::span<const uint32_t> readonly,uint32_t capacity)
        :device_(device),queue_(queue),queries_(device,capacity) {
        storage_=device.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        vertex_=device.createShader(fsi_vertex_spv,sizeof(fsi_vertex_spv),"main",RenderShaderFormat::SPIRV);
        original_=device.createShader(original.data(),original.size_bytes(),"main",RenderShaderFormat::SPIRV);
        readonly_=device.createShader(readonly.data(),readonly.size_bytes(),"main",RenderShaderFormat::SPIRV);
        const RenderDescriptorRange range{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1};
        const std::array<RenderDescriptorSetDesc,5> sets{{{}, {}, {}, {}, {&range,1}}};
        const RenderPushConstantRange push(0,0,0,sizeof(ShaderPushConstants),RenderShaderStageFlag::PIXEL);
        layout_=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,sets.data(),sets.size()));
        descriptors_=device.createDescriptorSet(sets[4]);descriptors_->setBuffer(0,storage_.get());
        RenderGraphicsPipelineDesc pipeline;pipeline.pipelineLayout=layout_.get();pipeline.vertexShader=vertex_.get();
        pipeline.renderTargetCount=1;pipeline.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
        pipeline.renderTargetBlend[0]=RenderBlendDesc::Copy();pipeline.renderTargetBlend[0].renderTargetWriteMask=0;
        pipeline.depthClipEnabled=true;pipeline.cullMode=RenderCullMode::NONE;
        pipeline.pixelShader=original_.get();original_pipeline_=device.createGraphicsPipeline(pipeline);
        pipeline.pixelShader=readonly_.get();readonly_pipeline_=device.createGraphicsPipeline(pipeline);
        Check(original_pipeline_&&readonly_pipeline_&&static_cast<VulkanGraphicsPipeline&>(*original_pipeline_).vk&&
              static_cast<VulkanGraphicsPipeline&>(*readonly_pipeline_).vk,"Readonly fixture pipeline creation failed");
        target_=device.createTexture(RenderTextureDesc::ColorTarget(640,360,RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* attachment=target_.get();RenderFramebufferDesc fd;fd.colorAttachments=&attachment;fd.colorAttachmentsCount=1;
        framebuffer_=device.createFramebuffer(fd);
    }
    Outcome Run(std::span<const Draw> draws,bool specialized) {
        Check(draws.size()==queries_.capacity(),"Fixture must fill both half-capacity fences");
        const uint32_t half=uint32_t(draws.size()/2);
        Words initial(kBytes/4,0xe000005a); // exact float24 .5, nonzero unchanged stencil.
        auto upload=device_.createBuffer(RenderBufferDesc::UploadBuffer(kBytes));Upload(*upload,initial.data(),kBytes);
        auto copy=queue_.createCommandList();auto seed_fence=device_.createCommandFence();copy->begin();
        copy->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(storage_.get(),RenderBufferAccess::WRITE));
        copy->copyBuffer(storage_.get(),upload.get());copy->end();
        queue_.executeCommandLists(copy.get(),seed_fence.get());queue_.waitForCommandFence(seed_fence.get());
        slots_.clear();slots_.reserve(draws.size());
        std::vector<const RenderCommandList*> commands;commands.reserve(draws.size());
        for(size_t i=0;i<draws.size();++i) {
            const auto& draw=draws[i];NativeFsiShaderConstants constants;
            auto& r=constants.raster;r.pitch=640;r.depth_base=draw.base;r.depth_format=1;r.depth_control=draw.control;
            r.front_constant_bias=draw.front_bias;r.back_constant_bias=draw.back_bias;
            // Deliberately aliased bound color, but zero masks and builtin export0.
            r.color[0].base=draw.base;r.color[0].format=7;r.color[0].mask=0;
            Slot slot;slot.constants=device_.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(constants),
                RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
            Upload(*slot.constants,&constants,sizeof(constants));
            const ShaderPushConstants addresses{0,0,slot.constants->getDeviceAddress()};
            slot.commands=queue_.createCommandList();auto& cmd=*slot.commands;cmd.begin();
            // These are the same whole-buffer graphics read/write dependencies
            // retained by Scene, including readonly->writer WAR and writer->read RAW.
            cmd.barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(storage_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
            cmd.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target_.get(),RenderTextureLayout::COLOR_WRITE));
            cmd.setFramebuffer(framebuffer_.get());cmd.setViewports(RenderViewport(0,0,640,360));cmd.setScissors(draw.scissor);
            cmd.setGraphicsPipelineLayout(layout_.get());cmd.setGraphicsPushConstants(0,&addresses);
            cmd.setGraphicsDescriptorSet(descriptors_.get(),4);
            cmd.setPipeline(specialized&&draw.readonly?readonly_pipeline_.get():original_pipeline_.get());
            // Each report occupies64 bytes: its begin sample is at +32,
            // and its matching end sample is at +0. Preserve the real tracker
            // address/alignment contract independently of the physical slot.
            const uint32_t report=0x2000u+64u*uint32_t(i);
            queries_.Begin(uint32_t(i+1),report+32);queries_.ResetDraw(cmd);queries_.BeginDraw(cmd);
            cmd.drawInstanced(3,draw.instances,0,0);queries_.EndDraw(cmd);cmd.end();
            queries_.EndDeferred(uint32_t(i+1),report);
            slots_.push_back(std::move(slot));commands.push_back(slots_.back().commands.get());
        }
        bool pending_rejected=false;try{(void)queries_.TakeDeferredResults();}catch(const std::exception&){pending_rejected=true;}
        Check(pending_rejected,"Unfenced query result escaped");
        auto first=device_.createCommandFence(),second=device_.createCommandFence();
        queue_.executeCommandLists(commands.data(),half,nullptr,0,nullptr,0,first.get());
        queue_.executeCommandLists(commands.data()+half,half,nullptr,0,nullptr,0,second.get());
        // Both halves were submitted before either wait. Owners remain in slots_.
        Outcome result;queue_.waitForCommandFence(first.get());queries_.CompletePrefix(half);
        for(const auto& q:queries_.TakeDeferredPrefix(half))result.samples.push_back(q.samples);
        Check(queries_.pending_draws()==half,"First fence retired a later query slot");
        queue_.waitForCommandFence(second.get());queries_.CompletePrefix(half);
        for(const auto& q:queries_.TakeDeferredResults())result.samples.push_back(q.samples);
        Check(result.samples.size()==draws.size()&&queries_.pending_draws()==0,"Query identities lost across fences");
        auto readback=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));copy->begin();
        copy->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(storage_.get(),RenderBufferAccess::READ));
        copy->copyBuffer(readback.get(),storage_.get());copy->end();
        queue_.executeCommandLists(copy.get(),seed_fence.get());queue_.waitForCommandFence(seed_fence.get());
        const auto* data=readback->map();auto& native=static_cast<VulkanBuffer&>(*readback);
        Check(vmaInvalidateAllocation(native.device->allocator,native.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Readback invalidate failed");
        result.bytes.resize(kBytes/4);std::memcpy(result.bytes.data(),data,kBytes);readback->unmap();slots_.clear();return result;
    }
};
std::vector<uint8_t> ByteVector(std::span<const uint32_t> words) {
    std::vector<uint8_t> bytes(words.size_bytes());std::memcpy(bytes.data(),words.data(),bytes.size());return bytes;
}
Words ReadSceneEdram(RenderDevice& device,RenderCommandQueue& queue,const NativeMemoryState& state) {
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
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto* mapped=readback->map();
    Check(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Scene invalidate failed");
    Words result(state.capacity/4);std::memcpy(result.data(),mapped,state.capacity);readback->unmap();return result;
}
Outcome RoomScene(RenderDevice& device,RenderCommandQueue& queue,std::span<const uint32_t> vertex,
                  std::span<const uint32_t> original,bool specialized,bool startup=true,uint32_t pipeline_limit=2048) {
    using Json=nlohmann::json;
    std::map<std::string,std::vector<uint8_t>> payloads;
    const auto add=[&](std::vector<uint8_t> bytes) {auto key=ContentHash(bytes);payloads.emplace(key,std::move(bytes));return key;};
    const auto floats=[&](std::initializer_list<float> values) {
        std::vector<uint8_t> bytes(values.size()*4);std::memcpy(bytes.data(),values.begin(),bytes.size());return add(std::move(bytes));
    };
    // Exact observed shader fetch contract, derived from its user-owned source:
    // stream93 selects vertex indices0/1/2;94 is zero translation/unit scale;
    //95 contains a fullscreen triangle. The shader's four c-registers are an
    // identity matrix, giving true raster depth.5 (not a pixel input parameter).
    const std::array<std::string,3> geometry_ids{
        add({0,1,2,0}),floats({0,0,0,1,1,1}),floats({-1,-1,.5f,3,-1,.5f,-1,3,.5f})};
    std::vector<uint8_t> state(0x2a80);
    const auto word=[](std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(24-8*i));
    };
    const auto number=[&](std::vector<uint8_t>& bytes,size_t at,float value){word(bytes,at,std::bit_cast<uint32_t>(value));};
    for(unsigned i=0;i<4;++i)number(state,0x780+16*i+4*i,1.f);
    word(state,0x2880,640);word(state,0x28bc,640|(360<<16));word(state,0x28c4,0x80000000);
    word(state,0x28c8,640|(360<<16));word(state,0x28dc,0);word(state,0x2944,1<<19);word(state,0x294c,0x43f);
    word(state,0x2948,(1<<11)|(1<<12));word(state,0x29c0,1);
    number(state,0x2908,320);number(state,0x290c,320);number(state,0x2910,-180);number(state,0x2914,180);number(state,0x2918,1);
    const Json depth{{"depth",true},{"edram_base",1440},{"format",1},{"format_info",0},
        {"surface_info",640},{"width",640},{"height",360}};
    Json geometry=Json::array();
    for(unsigned i=0;i<3;++i)geometry.push_back({{"kind",14},{"fetch_slot",93+i},{"endian",0},
        {"address","00007000"},{"bytes",payloads.at(geometry_ids[i]).size()},{"sha256",geometry_ids[i]}});
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.builtin_readonly_depth_queries=specialized;config.max_pipeline_cache_entries=pipeline_limit;config.batch_transfers=true;config.unified_gpu_queue=true;
    config.async_gpu_submissions=true;config.max_draw_batch_size=64;config.defer_native_queries=true;
    config.payload=[&](const std::string& key){return payloads.at(key);};
    config.shader=[&](const std::string& key) {Check(key==kReadonlyQueryVertexSource,"Wrong pinned VS source");return ByteVector(vertex);};
    config.fsi_depth_shader=[&]{return ByteVector(original);};
    SceneExecutor scene(config,&device,&queue);
    NativeFsiPipelineRecipe recipe{std::string(kReadonlyQueryVertexSource),std::nullopt,
        {uint32_t(RenderCullMode::NONE),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
         uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
    scene.PrewarmFsiPipeline(recipe,vertex,original);
    const bool ready=specialized&&startup&&scene.PrewarmReadonlyDepthQueryPipeline(recipe,vertex,original);
    scene.InitializeEdram(ByteVector(Words(10u*1024*1024/4,0xe000005a)));
    uint64_t event=0;
    for(unsigned i=0;i<4;++i) {
        auto raw=state;word(raw,0x2934,i%2==0?0x700776:0x700762);
        const float bias=i==0?-.125f:i==2?.125f:0.f;
        number(raw,0x2a54,bias);number(raw,0x2a5c,bias);const auto state_id=add(std::move(raw));
        const uint32_t report=0x2000+64*i;
        scene.Execute({{"type","query_begin"},{"frame",1},{"event",++event},{"query_generation",i+1},
            {"query_handle",0x1000+32*i},{"report_address",report+32}});
        scene.Execute({{"type","draw"},{"frame",1},{"event",++event},{"function","DrawPrimitive"},
            {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",kReadonlyQueryVertexSource},
            {"pixel_shader",nullptr},{"surfaces",{{"10",depth}}},{"geometry",geometry},
            {"vertex_slots",{93,94,95}},{"textures",Json::array()}});
        scene.Execute({{"type","query_end"},{"frame",1},{"event",++event},{"query_generation",i+1},
            {"query_handle",0x1000+32*i},{"report_address",report}});
        Check(scene.HasPendingGpuWork(),"Observed room draw unexpectedly drained between queries");
    }
    scene.FlushGpuWork();Outcome result;
    for(const auto& query:scene.TakeCompletedQueries())result.samples.push_back(query.result.samples);
    Check(result.samples==std::vector<uint64_t>({230400,230400,230400,0}),"Actual observed pinned VS query sequence differs");
    const auto& report=scene.report();
    Check(report.at("pipelines_created")==unsigned(ready?2:1),"Dynamic readonly/generic pipeline key collided");
    if(specialized) {
        const auto& stats=report.at("builtin_readonly_queries");
        Check(stats.at("selected")==unsigned(ready?2:0)&&stats.at("queried_selected")==stats.at("selected")&&
            stats.at("module_refusals")==0&&stats.at("vertex_refusals")==0&&
            (ready?stats.at("residency_refusals")==0:stats.at("residency_refusals")==2),
            "Actual Scene resident-only query gate selected an absent or unaudited module");
        std::cout<<"READONLY_QUERY_SCENE_COVERAGE "<<Json{{"startup",startup},{"ready",ready},
            {"pipeline_limit",pipeline_limit},{"stats",stats},{"pipelines",report.at("pipelines_created")}}.dump()<<'\n';
    }
    result.bytes=ReadSceneEdram(device,queue,scene.ExportState().edram);return result;
}
}
int main(int argc,char** argv)try {
    if(argc<3||argc>4)throw std::runtime_error("Usage: builtin-readonly-query-gpu-test original-builtin.spv pinned-vs.spv [64|128]");
    const uint32_t capacity=argc==4?uint32_t(std::stoul(argv[3])):64;
    Check(capacity==64||capacity==128,"Unsupported owner capacity");
    const auto original=Read(argv[1]),room_vertex=Read(argv[2]);auto transformed=TryBuiltinReadonlyQuery(original);
    Check(bool(transformed),"Builtin pin refused: original Native shader must be used");
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    Fixture fixture(device,*queue,original,*transformed,capacity);
    std::vector<Draw> cases(capacity);
    for(size_t i=0;i<cases.size();++i) {
        cases[i].front_bias=cases[i].back_bias=i%3==0?-.125f:i%3==1?0.f:.125f;
        cases[i].instances=i%4==0?2:1;
        if(i%5==0)cases[i].scissor={17,11,623,349};
        if(i%7==0)cases[i].base=2047; // wrapped tile base, unique depth pixels.
    }
    cases[1]=Draw{}; // Exactly the room's230400 passing sample count.
    const auto off=fixture.Run(cases,false),on=fixture.Run(cases,true);
    Check(off.bytes==on.bytes&&off.samples==on.samples,"Readonly numeric/query parity failed");
    Check(on.bytes==Words(10u*1024*1024/4,0xe000005a),"Readonly draw changed packed bytes");
    for(size_t i=0;i<cases.size();++i) {
        const auto& d=cases[i];const uint64_t expected=d.front_bias<0?0:
            uint64_t(d.scissor.right-d.scissor.left)*(d.scissor.bottom-d.scissor.top)*d.instances;
        Check(on.samples[i]==expected,"Exact late GE query count failed");
    }
    Check(on.samples[1]==230400,"Exact room query count failed");
    // Prior/following writes, including crossing the two fence submissions.
    // The slot ring is reused by each Run after exact fence retirement.
    for(size_t i=0;i<cases.size();++i) {
        cases[i]=Draw{};
        if(i%4==0||i%4==2) {
            cases[i].readonly=false;cases[i].control=2|4|(7<<4);
            cases[i].front_bias=cases[i].back_bias=i%4==0?-.125f:.125f;
        }
    }
    const auto mixed_off=fixture.Run(cases,false),mixed_on=fixture.Run(cases,true);
    Check(mixed_off.bytes==mixed_on.bytes&&mixed_off.samples==mixed_on.samples,"Mixed RAW/WAR parity failed");
    for(size_t i=0;i<cases.size();++i)Check(mixed_on.samples[i]==(i%4==3?0u:230400u),"Mixed writer/query sample count failed");
    // Nonfinite bias cannot pass live Scene validation. This direct-module
    // parity case still proves the transform preserves its original NaN math.
    std::fill(cases.begin(),cases.end(),Draw{});cases[0].front_bias=cases[0].back_bias=std::numeric_limits<float>::quiet_NaN();
    cases[1].front_bias=-.125f;cases[1].back_bias=.125f;
    const auto edge_off=fixture.Run(cases,false),edge_on=fixture.Run(cases,true);
    Check(edge_off.bytes==edge_on.bytes&&edge_off.samples==edge_on.samples,"NaN/front-back exact parity failed");
    const auto room_off=RoomScene(device,*queue,room_vertex,original,false);
    const auto room_on=RoomScene(device,*queue,room_vertex,original,true);
    Check(room_off.bytes==room_on.bytes&&room_off.samples==room_on.samples,"Observed VS Scene exact parity failed");
    const auto missed=RoomScene(device,*queue,room_vertex,original,true,false);
    const auto bounded=RoomScene(device,*queue,room_vertex,original,true,true,2);
    Check(room_off.bytes==missed.bytes&&room_off.samples==missed.samples&&
          room_off.bytes==bounded.bytes&&room_off.samples==bounded.samples,
          "Absent/budget-refused startup candidate changed ordinary Native query/storage output");
    // Actual Scene prewarm uses the audited pinned vertex, without fake draws.
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.builtin_readonly_depth_queries=true;
    SceneExecutor scene(config,&device,queue.get());
    NativeFsiPipelineRecipe recipe{std::string(kReadonlyQueryVertexSource),std::nullopt,
        {uint32_t(RenderCullMode::NONE),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
         uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
    scene.PrewarmFsiPipeline(recipe,room_vertex,original); // Legacy recipe stays generic.
    Check(scene.PrewarmReadonlyDepthQueryPipeline(recipe,room_vertex,original)&&
          scene.PrewarmReadonlyDepthQueryPipeline(recipe,room_vertex,original),"Actual pinned resident startup refused");
    Check(scene.report().at("pipelines_created")==2,"Generic/readonly pipeline key collision");
    Check(scene.report().at("builtin_readonly_queries").at("prepared")==1,"Readonly prewarm was not observed");
    Check(!scene.mutation_serial()&&!scene.HasPendingGpuWork(),"Prewarm mutated or submitted guest work");
    std::cout<<"PASS: "<<(6*capacity+16)<<" real queried draws, capacity="<<capacity<<", exact230400 samples,2-fence slot reuse, full10MiB/bias/NaN/alias clipping/overlap/RAW/WAR, pinned VS and resident/startup-budget fallback parity\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
