#include "renderer/native/scene_executor.h"
#include "renderer/native/packed_color_pass_groups.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/draw_state.h"
#include "fullscreen_spv.h"
#include "renderer/native/packed_color_variant8_catalog.h"
#include "renderer/native/fsi_state_specialization_compiler.h"
#include "renderer/native/native_monotonic_depth_module.h"
#include "renderer/native/texture_data.h"
#include "native_fsi_state_specialization_shader_version.h"
#include <bit>
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
struct Outcome {Words memory;Json queries,stats;};
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
void StoreWord(Bytes& bytes,size_t offset,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(offset+i)=uint8_t(value>>(24-8*i));
}


Words Primitive(RenderDevice& device,RenderCommandQueue& queue,std::span<const uint32_t> pixel,bool grouped) {
    constexpr uint32_t bytes=10u*1024*1024,count=33;
    Words expected(bytes/4,0x5a5a5a5a);
    for(uint32_t n=0;n<720*1280;++n)expected[n]=n&255;
    const auto upload=[&](RenderBuffer& b,const void* data,size_t size) {
        auto* mapped=b.map();Check(mapped,"Upload map failed");std::memcpy(mapped,data,size);
        auto& vk=static_cast<VulkanBuffer&>(b);Check(vmaFlushAllocation(vk.device->allocator,vk.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Flush failed");b.unmap();
    };
    auto packed=device.createBuffer(RenderBufferDesc::DefaultBuffer(bytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    auto initial=device.createBuffer(RenderBufferDesc::UploadBuffer(bytes));upload(*initial,expected.data(),bytes);
    auto seed=queue.createCommandList();auto fence=device.createCommandFence();seed->begin();seed->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(packed.get(),RenderBufferAccess::WRITE));seed->copyBuffer(packed.get(),initial.get());seed->end();queue.executeCommandLists(seed.get(),fence.get());queue.waitForCommandFence(fence.get());
    auto vs=device.createShader(fullscreen_spv,sizeof(fullscreen_spv),"main",RenderShaderFormat::SPIRV);
    auto ps=device.createShader(pixel.data(),pixel.size_bytes(),"main",RenderShaderFormat::SPIRV);
    RenderDescriptorRange range(RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1);
    const std::array<RenderDescriptorSetDesc,5> sets{{{},{},{},{},{&range,1}}};
    RenderPushConstantRange push(0,0,0,sizeof(ShaderPushConstants),RenderShaderStageFlag::PIXEL);
    auto layout=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,sets.data(),sets.size()));
    auto set=device.createDescriptorSet(sets[4]);set->setBuffer(0,packed.get());
    auto target=device.createTexture(RenderTextureDesc::ColorTarget(1280,720,RenderFormat::R8G8B8A8_UNORM));
    const RenderTexture* attachment=target.get();RenderFramebufferDesc fd;fd.colorAttachments=&attachment;fd.colorAttachmentsCount=1;
    auto framebuffer=device.createFramebuffer(fd);RenderGraphicsPipelineDesc p;p.pipelineLayout=layout.get();p.vertexShader=vs.get();p.pixelShader=ps.get();p.renderTargetCount=1;p.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
    p.renderTargetBlend[0]=RenderBlendDesc::Copy();p.renderTargetBlend[0].renderTargetWriteMask=0;p.cullMode=RenderCullMode::NONE;p.depthClipEnabled=true;p.primitiveTopology=RenderPrimitiveTopology::TRIANGLE_LIST;
    auto pipeline=device.createGraphicsPipeline(p);Check(pipeline&&static_cast<VulkanGraphicsPipeline&>(*pipeline).vk,"Primitive pipeline failed");
    const std::array<std::array<float,4>,4> colors{{{1,0,0,1},{0,.5f,0,1},{0,0,.25f,1},{2,.5f,.25f,1}}};
    const std::array<uint32_t,4> words{0xc0000180,0xc0040000,0xc8000000,0xc8040200};
    const std::array<RenderRect,5> rects{{{0,0,1280,720},{39,15,81,33},{0,0,640,720},{40,16,1280,719},{79,0,81,720}}};
    std::array<std::unique_ptr<RenderBuffer>,count> constants;
    for(uint32_t i=0;i<count;++i) {
        NativeFsiShaderConstants c;c.raster.pitch=1280;c.raster.depth_format=1;c.raster.depth_control=0x700766;
        c.raster.color[0]={720,3,15,0x10001};c.raster.blend_constant=colors[i%4];
        constants[i]=device.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(c),RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));upload(*constants[i],&c,sizeof(c));
        const auto& rect=rects[i%rects.size()];
        for(uint32_t y=rect.top;y<uint32_t(rect.bottom);++y)for(uint32_t x=rect.left;x<uint32_t(rect.right);++x) {
            const uint32_t tile=(y/16)*16+x/80,offset=(y%16)*80;
            const uint32_t d=tile*1280+offset+(x%80<40?x%80+40:x%80-40),color=(720+tile)*1280+offset+x%80;
            expected[d]=0xe0000000|(expected[d]&255);expected[color]=words[i%4];
        }
    }
    std::vector<std::unique_ptr<RenderCommandList>> commands;std::vector<const RenderCommandList*> pointers;
    const uint32_t group=grouped?16:1;
    for(uint32_t first=0;first<count;first+=group) {
        auto cmd=queue.createCommandList();cmd->begin();cmd->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(packed.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
        cmd->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target.get(),RenderTextureLayout::COLOR_WRITE));cmd->setFramebuffer(framebuffer.get());cmd->setViewports(RenderViewport(0,0,1280,720));cmd->setGraphicsPipelineLayout(layout.get());cmd->setGraphicsDescriptorSet(set.get(),4);cmd->setPipeline(pipeline.get());
        for(uint32_t i=first;i<std::min(first+group,count);++i) {
            const ShaderPushConstants addresses{0,0,constants[i]->getDeviceAddress()};cmd->setGraphicsPushConstants(0,&addresses);cmd->setScissors(rects[i%rects.size()]);cmd->drawInstanced(3,1,0,0);
        }
        cmd->end();pointers.push_back(cmd.get());commands.push_back(std::move(cmd));
    }
    queue.executeCommandLists(pointers.data(),uint32_t(pointers.size()),nullptr,0,nullptr,0,fence.get());queue.waitForCommandFence(fence.get());
    NativeMemoryState state;state.buffer=static_cast<VulkanBuffer&>(*packed).vk;state.capacity=bytes;
    const auto actual=ReadSceneEdram(device,queue,state);Check(actual==expected,"Independent two-plane color-order/depth/stencil/untouched-memory oracle differs");
    std::cout<<"PRIMITIVE grouped="<<grouped<<" draws=33 passes="<<pointers.size()<<" exact_full_memory=10485760\n";return actual;
}

Outcome Run(RenderDevice& device,RenderCommandQueue& queue,const std::filesystem::path& inspected,
            const std::filesystem::path& compiled,uint32_t capacity,bool enabled,bool prewarm,bool unpinned) {
    std::ifstream f(inspected/"scene.json");Json scene;f>>scene;
    std::array<Json,2> draws;
    for(const auto& op:scene.at("operations")) {
        if(op.value("type","")!="draw"||op.at("pixel_shader").is_null())continue;
        for(unsigned i=0;i<2;++i) {
            const auto& pair=kRetainedOpaqueCatalogPairs[i+1];
            if(draws[i].is_null()&&op.at("pixel_shader")==pair.pixel&&op.at("vertex_shader")==pair.vertex&&
               op.at("surfaces").size()==2&&op.at("surfaces").at("6").at("edram_base")==720)draws[i]=op;
        }
    }
    Check(!draws[0].is_null()&&!draws[1].is_null(),"Missing actual material cohorts");
    std::map<std::string,Bytes> overrides;
    const auto payload=[&](const std::string& h) {const auto it=overrides.find(h);return it==overrides.end()?ReadBytes(inspected/(h+".payload.bin")):it->second;};
    const auto shader=[&](const std::string& h,bool pixel) {
        auto b=ReadBytes(compiled/(h+(pixel?".fsi.spv":".spv")));
        if(unpinned)b.at(8)^=1; // Generator word only; valid equivalent code, unaudited identity.
        return b;
    };
    SceneExecutorConfig config;config.native_fsi=config.ordered_pixel_interlock_enabled=true;
    config.batch_transfers=config.unified_gpu_queue=config.async_gpu_submissions=true;
    config.max_draw_batch_size=16;config.async_submission_capacity=capacity;
    config.defer_native_queries=true;config.retire_completed_query_prefixes=true;
    config.packed_color_render_pass_groups=enabled;config.payload=payload;config.capture=inspected;config.compiled=compiled;
    config.shader=[&](const std::string& h){return shader(h,false);};
    config.fsi_shader=[&](const std::string& h){return shader(h,true);};
    SceneExecutor executor(config,&device,&queue);
    if(prewarm)for(const auto& draw:draws) {
        const auto bytes=payload(draw.at("device_state"));std::array<uint32_t,kDrawStateWords> words;
        for(unsigned n=0;n<words.size();++n) {const auto at=kDrawStateOffset+n*4;words[n]=(uint32_t(bytes.at(at))<<24)|(uint32_t(bytes.at(at+1))<<16)|(uint32_t(bytes.at(at+2))<<8)|bytes.at(at+3);}
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto state=DecodeDrawState(words,1280,720,4,true);const auto& p=state.pipeline;
        const std::string vs=draw.at("vertex_shader"),ps=draw.at("pixel_shader");
        executor.PrewarmFsiPipeline({vs,ps,{uint32_t(p.cullMode),uint32_t(p.frontFace),uint32_t(p.depthClipEnabled),
            uint32_t(p.primitiveTopology),state.specialization,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1},
            AsWords(shader(vs,false)),AsWords(shader(ps,true)));
    }
    Bytes initial(10u*1024*1024,0x5a);
    // Primary depth zero with arbitrary stencil; color and unused bytes retain
    // nonuniform sentinel input. Actual captured geometry must change colors.
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x) {
        const auto at=NativePackedColorPassWord(x,y,true)*4;initial[at]=uint8_t(x+y);initial[at+1]=initial[at+2]=initial[at+3]=0;
    }
    executor.InitializeEdram(initial);uint64_t event=0;
    const auto record=[&](Json op) {op["event"]=++event;op["frame"]=17;executor.FlushCommittedState();executor.Execute(op);};
    const auto draw=[&](unsigned i){record(draws.at(i));};
    const auto query=[&](bool begin,uint64_t generation) {record({{"type",begin?"query_begin":"query_end"},{"query_generation",generation},{"query_handle",0x1000},{"report_address",begin?0x2020:0x2000}});};
    // Warm texture uploads, then exercise capacity retirement with retained
    // sampled images and separate per-draw descriptor/constant ownership.
    draw(0);draw(1);executor.FlushGpuWork();
    for(uint32_t n=0;n<2*capacity+5;++n)draw(n%2);
    query(true,1);query(false,1); // Empty query closes an accepted partial group.
    query(true,2);draw(0);draw(1);query(false,2);
    // Changed depth control must break the pass; restore and continue.
    auto changed=draws[0];auto state=payload(changed.at("device_state"));StoreWord(state,0x2934,0x700762);
    const auto hash=ContentHash(state);overrides.emplace(hash,std::move(state));changed["device_state"]=hash;record(changed);
    for(uint32_t n=0;n<19;++n)draw(n%2);
    query(true,3);draw(0);query(false,3);
    executor.FlushGpuWork();Outcome result;result.queries=Json::array();
    for(const auto& q:executor.TakeCompletedQueries())result.queries.push_back({q.frame,q.generation,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});
    Check(result.queries.size()==3&&result.queries[0][6]==0&&result.queries[0][7]==0&&result.queries[1][6]==2&&result.queries[1][7].get<uint64_t>()>0&&result.queries[2][7].get<uint64_t>()>0,"Real query witness vacuous");
    result.memory=ReadSceneEdram(device,queue,executor.ExportState().edram);result.stats=executor.report().at("packed_color_render_pass_groups");
    const auto& s=result.stats;const auto& work=executor.report().at("gpu_work_queue");
    Check(work.at("accepted_work")==work.at("completed_work")&&!executor.HasPendingGpuWork()&&work.at("maximum_lists").get<uint64_t>()<=capacity,"Logical FIFO ownership incomplete");
    Check(result.memory!=AsWords(initial),"Captured materials wrote no bytes");
    if(enabled&&!unpinned) {
        Check(s.at("selected").get<uint64_t>()>capacity&&s.at("render_passes_avoided").get<uint64_t>()>capacity&&s.at("maximum_group_draws")==16,"Grouping not exercised");
        Check(s.at("sampled_barriers_avoided").get<uint64_t>()>capacity&&s.at("query_refusals")==3&&s.at("refusals").get<uint64_t>()>=4,"Sample/query/refusal coverage missing");
        Check(s.at("selected").get<uint64_t>()==s.at("closed_render_pass_groups").get<uint64_t>()+s.at("render_passes_avoided").get<uint64_t>()&&s.at("eligible").get<uint64_t>()==s.at("selected").get<uint64_t>()+s.at("preparation_refusals").get<uint64_t>(),"Counter conservation failed");
    } else Check(s.at("selected")==0&&s.at("render_passes_avoided")==0,"Disabled/unpinned draw grouped");
    // Accepted partial-group shutdown must fence the sampled-image owners.
    draw(0);draw(1);Check(executor.HasPendingDraws(),"Partial shutdown witness missing");
    std::cout<<Json{{"capacity",capacity},{"enabled",enabled},{"prewarm",prewarm},{"unpinned",unpinned},{"groups",s},{"queries",result.queries}}.dump()<<'\n';return result;
}
// Optional sixth argument adds this proved variant8 fixture. The existing
// five-argument canonical fixture and independent primitive remain unchanged.
constexpr std::string_view kVariant8Vs="20d0f93a552fb740d8f3e650b3ceed46b00284cd7c33017a5340c51506c79a49";
constexpr std::string_view kVariant8Ps="e413a5d1f8ecffb4f709964a31952aa5e377bb4a331dcc399dd1f28468401c87";
constexpr std::string_view kVariant8Spirv="bb23b3fae316a32b9bfac1847fb16b5ec3da2968749a2e144e608e31f505652e";
NativeFsiStateRecipe Variant8Recipe() {
    NativeFsiStateRecipe r;
    r.generic={std::string(kVariant8Vs),std::string(kVariant8Ps),{3,1,1,4,0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
    r.state.words={7341926,1,0,3,15,65537,0,0,65537,0,0,65537,0,0,65537};
    Check(ValidFsiStateRecipe(r)&&NativePackedColorVariant8Tuple(r.generic.vertex,*r.generic.pixel,
        r.generic.raster,r.state,r.generic.color_format,r.generic.depth_format),"Variant8 donor is outside proved53 tuples");
    return r;
}
std::shared_ptr<FsiStateSpecializationProgram> Variant8Program(const std::filesystem::path& path,bool foreign) {
    const auto bytes=ReadBytes(path);
    const auto word=[&](size_t at) {return uint32_t(bytes.at(at))|(uint32_t(bytes.at(at+1))<<8)|
        (uint32_t(bytes.at(at+2))<<16)|(uint32_t(bytes.at(at+3))<<24);};
    const auto text=[&](size_t at){return std::string(reinterpret_cast<const char*>(bytes.data()+at),64);};
    Check(bytes.size()>=400&&std::memcmp(bytes.data(),"G3FSIC01",8)==0,"Variant8 cache header mismatch");
    Check(word(8)==1794&&word(12)==93596/4&&bytes.size()==400+93596,"Variant8 cache extent/mask mismatch");
    Check(text(16)==kVariant8Ps&&text(80)==kNativeFsiStateSpecializationShaderVersion,"Variant8 cache source/version mismatch");
    Bytes signed_bytes(bytes.begin(),bytes.begin()+336);signed_bytes.insert(signed_bytes.end(),bytes.begin()+400,bytes.end());
    Check(ContentHash(signed_bytes)==text(336),"Variant8 cache integrity mismatch");
    Bytes code(bytes.begin()+400,bytes.end());
    Check(ContentHash(code)==kVariant8Spirv,"Variant8 donor payload is not the audited exact module");
    auto p=std::make_shared<FsiStateSpecializationProgram>();
    p->source_sha256=std::string(kVariant8Ps);p->compiler_version=FsiStateSpecializationCompilerVersion();
    p->variant_version=text(80);p->baseline_hlsl_sha256=text(144);p->modified_hlsl_sha256=text(208);
    p->modified_common_sha256=text(272);p->specialization_mask=word(8);p->disk_cache_hit=true;
    p->spirv=AsWords(code);if(foreign)p->spirv.at(2)^=1; // Only the SPIR-V generator word.
    p->compiled_spirv_sha256=ContentHash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p->spirv.data()),p->spirv.size()*4));
    Check(NativeMonotonicDepthModuleCertified(kVariant8Ps,true,NativeShaderVariant::PipelineStateEpilogueV1,
        {reinterpret_cast<const uint8_t*>(p->spirv.data()),p->spirv.size()*4})==!foreign,"Foreign module certificate control failed");
    return p;
}
struct Variant8Inputs {std::array<Json,2> draw;Json resolve;NativeFsiStateRecipe recipe;};
Variant8Inputs Variant8Donors(const std::filesystem::path& inspected) {
    std::ifstream file(inspected/"scene.json");Json scene;file>>scene;Variant8Inputs in;in.recipe=Variant8Recipe();Json alternate;
    for(const auto& op:scene.at("operations")) {
        if(op.at("event")==384)in.draw[0]=op;
        if(op.at("event")==317)alternate=op;
        if(op.at("event")==549)in.resolve=op;
    }
    Check(in.draw[0].value("type","")=="draw"&&in.draw[0].at("frame")==17672&&
        in.draw[0].at("vertex_shader")==kVariant8Vs&&in.draw[0].at("pixel_shader")==kVariant8Ps&&
        in.draw[0].at("device_state")=="f70362078fc5b42825bf38ef5059388b2526b1a3479fa9b6e21ea2c20fc7a646",
        "Wrong actual variant8 donor");
    in.draw[1]=in.draw[0];bool replaced=false;
    for(auto& texture:in.draw[1].at("textures"))if(texture.at("slot")==0)
        for(const auto& other:alternate.at("textures"))if(other.at("slot")==0){texture=other;replaced=true;}
    Check(replaced&&in.draw[0].at("textures")!=in.draw[1].at("textures"),"Missing controlled second texture");
    for(const auto& draw:in.draw)for(const auto& texture:draw.at("textures"))
        Check(texture.at("source")=="cpu_snapshot"&&texture.at("producers").empty(),"Variant8 fixture texture aliases packed output");
    Check(in.resolve.value("type","")=="resolve"&&in.resolve.at("source_slot")==4&&
        in.resolve.at("destination").at("base")=="13aa3000"&&in.resolve.at("flags")==4,"Wrong actual depth-conversion donor");
    return in;
}
Words Variant8Seed() {
    Words seed(10u*1024*1024/4,0x5a5a5a5a);
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x)seed[NativePackedColorPassWord(x,y,true)]=(x+y)&255;
    return seed;
}
uint64_t ColorDifferences(const Words& a,const Words& b) {
    Check(a.size()==10u*1024*1024/4&&a.size()==b.size(),"Wrong full-memory witness size");
    uint64_t n=0;for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x){const auto at=NativePackedColorPassWord(x,y,false);n+=a[at]!=b[at];}return n;
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
enum class Variant8Mode {Disabled,Certified,Foreign,ReplacedUncertifiedModule,ReplacedCertifiedModule};
struct Variant8Outcome {Words before_query,after_query,resolved;Json queries,report,witness;};
Variant8Outcome RunVariant8(RenderDevice& device,RenderCommandQueue& queue,const std::filesystem::path& inspected,
    const std::filesystem::path& compiled,const std::filesystem::path& cache,uint32_t capacity,Variant8Mode mode) {
    const bool enabled=mode!=Variant8Mode::Disabled;
    const bool foreign=mode==Variant8Mode::Foreign||mode==Variant8Mode::ReplacedCertifiedModule;
    const auto in=Variant8Donors(inspected);std::map<std::string,Bytes> overrides;
    const auto payload=[&](const std::string& h){const auto it=overrides.find(h);return it==overrides.end()?ReadBytes(inspected/(h+".payload.bin")):it->second;};
    const auto shader=[&](const std::string& h,bool pixel){return ReadBytes(compiled/(h+(pixel?".fsi.spv":".spv")));};
    auto program=Variant8Program(cache,foreign);auto catalog=std::make_shared<FsiStateSpecializationCatalog>();catalog->programs.emplace(std::string(kVariant8Ps),program);
    SceneExecutorConfig config;config.native_fsi=config.ordered_pixel_interlock_enabled=true;
    config.batch_transfers=config.unified_gpu_queue=config.async_gpu_submissions=true;
    config.max_draw_batch_size=16;config.async_submission_capacity=capacity;config.defer_native_queries=config.retire_completed_query_prefixes=true;
    config.packed_color_render_pass_groups=enabled;config.specialize_fsi_pipeline_state=config.fsi_state_material_composition=true;
    config.fsi_state_programs=catalog;config.test_retained_resource_pool_diagnostics=true;
    config.payload=payload;config.capture=inspected;config.compiled=compiled;
    config.shader=[&](const std::string& h){return shader(h,false);};config.fsi_shader=[&](const std::string& h){return shader(h,true);};
    SceneExecutor executor(config,&device,&queue);const auto vertex=AsWords(shader(std::string(kVariant8Vs),false));
    Check(ContentHash(shader(std::string(kVariant8Vs),false))=="9ac9a54dd8b3bf64470adfc2d886fb61f95f3b558d8a2208fd6909867a728a1a","Variant8 VS pin mismatch");
    executor.PrewarmFsiPipeline(in.recipe.generic,vertex,AsWords(shader(std::string(kVariant8Ps),true)));
    Check(executor.PrewarmFsiStatePipeline(in.recipe,vertex),"Variant8 actual state PSO was not prepared");
    Variant8Outcome out;out.witness={{"mode",int(mode)},{"actual_donor",384},{"alternate_texture_donor",317},{"controlled_texture_substitution",true}};
    if(mode==Variant8Mode::ReplacedUncertifiedModule||mode==Variant8Mode::ReplacedCertifiedModule) {
        const bool replacement_foreign=mode==Variant8Mode::ReplacedUncertifiedModule;
        const auto replacement=Variant8Program(cache,replacement_foreign);
        const auto w=executor.ReplacePackedColorVariant8ModuleForFixture(in.recipe.generic,in.recipe.state,replacement->spirv);
        Check(w.old_shader&&w.new_shader&&w.old_shader!=w.new_shader&&w.pipeline&&w.distinct_vulkan_modules&&
            w.pipeline_unchanged&&w.frontier_unchanged&&w.mutation_unchanged&&w.accepted_work==w.completed_work&&
            w.old_module_certified==!foreign&&w.new_module_certified==!replacement_foreign&&
            w.pipeline_certified_before==!foreign&&w.pipeline_certified_after==!foreign&&w.displaced_bytes==93596,
            "Variant8 actual module/PSO owner replacement witness failed");
        out.witness["replacement"]={{"old_shader",w.old_shader},{"new_shader",w.new_shader},{"pipeline",w.pipeline},
            {"distinct_vulkan_modules",w.distinct_vulkan_modules},{"old_module_certified",w.old_module_certified},
            {"new_module_certified",w.new_module_certified},{"pipeline_certified_before",w.pipeline_certified_before},
            {"pipeline_certified_after",w.pipeline_certified_after},{"pipeline_unchanged",w.pipeline_unchanged},
            {"frontier_unchanged",w.frontier_unchanged},{"mutation_unchanged",w.mutation_unchanged},
            {"accepted_work",w.accepted_work},{"completed_work",w.completed_work},{"displaced_bytes",w.displaced_bytes}};
    }
    const auto seed=Variant8Seed();const auto reset=[&]{executor.InitializeEdram({reinterpret_cast<const uint8_t*>(seed.data()),seed.size()*4});};
    const auto read=[&]{executor.FlushGpuWork();return ReadSceneEdram(device,queue,executor.ExportState().edram);};
    uint64_t event=0;const auto record=[&](Json op){op["event"]=++event;op["frame"]=17;executor.FlushCommittedState();executor.Execute(op);};
    const auto draw=[&](unsigned i){record(in.draw.at(i));};
    const auto query=[&](bool begin,uint64_t generation){record({{"type",begin?"query_begin":"query_end"},{"query_generation",generation},{"query_handle",0x1000},{"report_address",begin?0x2020:0x2000}});};
    // Prove same geometry's two sampled payloads produce distinct real color
    // writes before using it as a repeated cross-draw overlap witness.
    reset();draw(0);const auto a=read();reset();draw(1);const auto b=read();
    const auto overlap=ColorDifferences(a,b);Check(overlap>0&&ColorDifferences(a,seed)>0&&ColorDifferences(b,seed)>0,"Textured overlap witness is vacuous");
    out.witness["single_draw_texture_color_differences"]=overlap;
    reset();for(uint32_t n=0;n<2*capacity+5;++n)draw(n%2);
    out.before_query=read(); // Before any queried redraw can mask missing writes.
    Check(out.before_query==a,"Repeated last-A overlap did not reproduce ordinary A full-memory result");
    // Empty and actual queries must seal groups and preserve sample results.
    draw(0);draw(1);query(true,1);query(false,1);query(true,2);draw(0);draw(1);query(false,2);
    auto changed=in.draw[0];auto bytes=payload(changed.at("device_state"));StoreWord(bytes,0x2934,0x700762);
    const auto hash=ContentHash(bytes);overrides.emplace(hash,std::move(bytes));changed["device_state"]=hash;record(changed);
    for(unsigned n=0;n<19;++n)draw(n%2);
    // Real captured depth conversion enters the normal transfer path with an
    // accepted partial group. Compare its bounded physical output separately.
    auto fetch=in.resolve.at("destination_fetch").get<std::array<uint32_t,6>>();const uint32_t address=0x13aa3000;
    fetch[1]=(fetch[1]&0xfff)|address;const auto ranges=TextureMemoryRanges(DescribeTexture(fetch),false);
    Check(!ranges.empty(),"Conversion layout has no ranges");uint64_t end=address;
    for(const auto& range:ranges){Check(range.first>=address,"Unexpected conversion range");end=std::max(end,uint64_t(range.first)+range.second);}
    Check(end>address&&end-address<=8u*1024*1024,"Unbounded conversion witness");const uint32_t size=uint32_t(end-address);
    // Initialize padding before opening the group; the resolve itself seals it.
    executor.InitializeMemory(address,Bytes(size,0xa5));draw(0);draw(1);
    const auto before_resolve=executor.ObserveAcceptedWork();record(in.resolve);const auto after_resolve=executor.ObserveAcceptedWork();
    Check(after_resolve.available&&before_resolve.available&&after_resolve.transfers>before_resolve.transfers,"Actual conversion accepted no transfer work");
    executor.FlushGpuWork();const auto state=executor.ExportState();out.resolved=ReadPhysicalRange(device,queue,state.physical,address,size);
    Check(out.resolved!=Words(size/4,0xa5a5a5a5),"Actual conversion output is vacuous");
    for(unsigned n=0;n<19;++n)draw(n%2);query(true,3);draw(0);query(false,3);
    out.after_query=read();out.queries=Json::array();
    for(const auto& q:executor.TakeCompletedQueries())out.queries.push_back({q.frame,q.generation,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples});
    Check(out.queries.size()==3&&out.queries[0][6]==0&&out.queries[0][7]==0&&out.queries[1][6]==2&&out.queries[1][7].get<uint64_t>()>0&&out.queries[2][7].get<uint64_t>()>0,"Variant8 query witness vacuous");
    out.report=executor.report();const auto& s=out.report.at("packed_color_render_pass_groups");const auto& work=out.report.at("gpu_work_queue");
    Check(out.report.at("fsi_state_specialization").at("selected").get<uint64_t>()>2*capacity,"Variant8 selected module path not exercised");
    Check(work.at("accepted_work")==work.at("completed_work")&&!executor.HasPendingGpuWork()&&work.at("maximum_lists").get<uint64_t>()<=capacity,"Variant8 FIFO ownership incomplete");
    if(mode==Variant8Mode::Certified) {
        Check(s.at("selected").get<uint64_t>()>capacity&&s.at("render_passes_avoided").get<uint64_t>()>capacity&&s.at("maximum_group_draws")==16,"Variant8 cap16/capacity grouping absent");
        Check(s.at("sampled_barriers_avoided").get<uint64_t>()>capacity&&
            s.at("variant8_selected")==s.at("selected")&&s.at("variant8_render_passes_avoided")==s.at("render_passes_avoided")&&
            s.at("variant8_module_refusals")==0&&s.at("variant8_pipeline_refusals")==0,"Variant8 sampled/certificate coverage absent");
    } else Check(s.at("selected")==0&&s.at("render_passes_avoided")==0,"Disabled or uncertified variant8 owner grouped");
    Check(s.at("selected").get<uint64_t>()==s.at("closed_render_pass_groups").get<uint64_t>()+s.at("render_passes_avoided").get<uint64_t>()&&
        s.at("eligible").get<uint64_t>()==s.at("selected").get<uint64_t>()+s.at("preparation_refusals").get<uint64_t>(),"Variant8 group accounting mismatch");
    Check(out.report.at("fsi_state_material_composition").at("query_refusals")==3&&
        out.report.at("fsi_state_specialization").at("queried_selected")==0,"Query did not use the ordinary canonical fallback");
    if(enabled) {
        const bool module_refusal=mode==Variant8Mode::Foreign||mode==Variant8Mode::ReplacedUncertifiedModule;
        const bool pipeline_refusal=mode==Variant8Mode::Foreign||mode==Variant8Mode::ReplacedCertifiedModule;
        Check((s.at("variant8_module_refusals").get<uint64_t>()>capacity)==module_refusal&&
            (s.at("variant8_pipeline_refusals").get<uint64_t>()>capacity)==pipeline_refusal,"Module and PSO certificate refusal controls were not independent");
    }
    out.witness["pre_query_full_memory_bytes"]=out.before_query.size()*4;
    out.witness["post_query_full_memory_bytes"]=out.after_query.size()*4;
    out.witness["actual_resolve_bytes"]=out.resolved.size()*4;
    const auto digest=[](const Words& words){return ContentHash({reinterpret_cast<const uint8_t*>(words.data()),words.size()*4});};
    out.witness["pre_query_sha256"]=digest(out.before_query);out.witness["post_query_sha256"]=digest(out.after_query);
    out.witness["resolve_sha256"]=digest(out.resolved);
    out.witness["variant8_selected"]=out.report.at("fsi_state_specialization").at("selected");
    out.witness["canonical_query_refusals"]=out.report.at("fsi_state_material_composition").at("query_refusals");
    draw(0);draw(1);Check(executor.HasPendingDraws(),"Variant8 partial shutdown witness absent");
    std::cout<<Json{{"variant8",true},{"capacity",capacity},{"witness",out.witness},{"groups",s},{"queries",out.queries}}.dump()<<'\n';return out;
}
void Variant8Suite(RenderDevice& device,RenderCommandQueue& queue,const std::filesystem::path& inspected,
    const std::filesystem::path& compiled,const std::filesystem::path& cache,uint32_t capacity) {
    const auto baseline=RunVariant8(device,queue,inspected,compiled,cache,capacity,Variant8Mode::Disabled);
    for(auto mode:{Variant8Mode::Certified,Variant8Mode::Foreign,Variant8Mode::ReplacedUncertifiedModule,Variant8Mode::ReplacedCertifiedModule}) {
        const auto candidate=RunVariant8(device,queue,inspected,compiled,cache,capacity,mode);
        Check(candidate.before_query==baseline.before_query&&candidate.after_query==baseline.after_query&&candidate.resolved==baseline.resolved&&candidate.queries==baseline.queries,
            "Variant8 pre-query/final full-memory, conversion output or query parity differs");
    }
}

}
int main(int argc,char** argv)try {
    Check(argc==5||argc==6,"Usage: packed-color-pass-scene-test inspected compiled capacity fixture.spv [variant8.g3fsi]");const uint32_t capacity=std::stoul(argv[3]);Check(capacity==64||capacity==128,"Invalid capacity");
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto primitive=AsWords(ReadBytes(argv[4]));
    Check(Primitive(device,*queue,primitive,false)==Primitive(device,*queue,primitive,true),"Cross-draw primitive order differs");
    if(argc==6) {
        Variant8Suite(device,*queue,argv[1],argv[2],argv[5],capacity);
        std::cout<<"PASS: "<<checks<<" checks; actual variant8 textured overlap, cap16/capacity, full10MiB and resolve/query parity, owner certificate refusals\n";return 0;
    }
    auto baseline=Run(device,*queue,argv[1],argv[2],capacity,false,false,false);
    for(const auto& mode:std::array<std::array<bool,2>,3>{{{false,false},{true,false},{true,true}}}) {
        const auto candidate=Run(device,*queue,argv[1],argv[2],capacity,true,mode[0],mode[1]);
        Check(candidate.memory==baseline.memory&&candidate.queries==baseline.queries,"Full packed memory or real query results differ");
    }
    std::cout<<"PASS: "<<checks<<" checks; actual textured material Scene, full10MiB, cold/prewarmed/unknown modules, query boundaries, capacity and partial shutdown\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
