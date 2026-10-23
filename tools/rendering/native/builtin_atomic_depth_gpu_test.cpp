#include "renderer/native/builtin_atomic_depth.h"
#include "renderer/native/packed_depth_pass_groups.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/draw_state.h"
#include "renderer/native/pipeline_prewarm.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/query_tracker.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/scene_executor.h"
#include "renderer/native/texture_data.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace plume;
using namespace gow3::renderer;
namespace {
using Words = std::vector<uint32_t>;
using Bytes = std::vector<uint8_t>;
using Json = nlohmann::json;
using Position = std::array<float,4>;
constexpr size_t kEdramBytes = 10u * 1024 * 1024;
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Bytes ReadBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() < 0) throw std::runtime_error("Cannot read " + path.string());
    Bytes bytes(size_t(stream.tellg()));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Truncated " + path.string());
    return bytes;
}
Words ReadWords(const std::filesystem::path& path) {
    auto bytes = ReadBytes(path);
    Check(bytes.size() >= 20 && bytes.size() % 4 == 0, "Invalid fixture SPIR-V");
    Words words(bytes.size()/4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}
Bytes ToBytes(std::span<const uint32_t> words) {
    Bytes bytes(words.size_bytes());
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}
void Upload(RenderBuffer& buffer, const void* data, size_t bytes) {
    auto* mapped = buffer.map();
    Check(mapped, "Fixture buffer map failed");
    std::memcpy(mapped, data, bytes);
    auto& native = static_cast<VulkanBuffer&>(buffer);
    Check(vmaFlushAllocation(native.device->allocator, native.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
        "Fixture buffer flush failed");
    buffer.unmap();
}
Words Seed() {
    Words words(kEdramBytes/4);
    // Every possible S8 byte, alongside arbitrary canonical/noncanonical D24
    // codes. Deliberately not a uniform clear or already-maximal depth image.
    uint32_t random = 0x62964a37;
    for (size_t i = 0; i < words.size(); ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        words[i] = (random & 0xffffff00u) | uint32_t(i & 255);
    }
    return words;
}
std::vector<Position> Fullscreen(float depth) {
    return {{-1,-1,depth,1}, {3,-1,depth,1}, {-1,3,depth,1}};
}
std::vector<Position> Overlapping(bool reverse, bool clipped) {
    std::vector<Position> result;
    // Float24 quantizes float32's low mantissa bits. Equal, ascending and
    // descending values contend within one draw, including opposite slopes.
    const std::array<float,12> depths{.125f,.75f,.5f,.25f,.875f,.5f,
        std::nextafter(.5f, 1.f), std::nextafter(.5f, 0.f),
        .5f + std::ldexp(1.f,-22), 0.f, 1.f, .625f};
    for (unsigned i = 0; i < 48; ++i) {
        const unsigned j = reverse ? 47-i : i;
        const float a = depths[j % depths.size()];
        const float b = depths[(j+3) % depths.size()];
        const float c = depths[(j+7) % depths.size()];
        if (i % 3 == 0) { // full coverage, with true varying depth
            result.insert(result.end(), {{-1,-1,a,1}, {3,-1,b,1}, {-1,3,c,1}});
        } else { // differently placed partly overlapping triangles
            const float shift = float(i % 7)*.08f;
            result.insert(result.end(), {{-.9f+shift,-.9f,a,1}, {.85f,.15f,b,1}, {-.4f,.95f-shift,c,1}});
        }
    }
    if (clipped) {
        // Finite outside-near/far vertices exercise well-defined clipping.
        result.insert(result.end(), {{-1,-1,-.25f,1}, {3,-1,.5f,1}, {-1,3,1.25f,1}});
    }
    return result;
}
struct Case {
    uint32_t width=256, height=64, pitch=320, base=123;
    float constant_bias=0, slope_bias=0;
    bool clipped=false, reverse=false, inset=false;
};
struct Outcome {
    Words after_first, final_bytes;
    std::vector<uint64_t> samples;
};
class DirectFixture {
    RenderDevice& device_;
    RenderCommandQueue& queue_;
    std::unique_ptr<RenderBuffer> storage_;
    std::unique_ptr<RenderShader> vertex_, original_, atomic_;
    std::unique_ptr<RenderPipelineLayout> layout_;
    std::unique_ptr<RenderPipeline> original_pipeline_, atomic_pipeline_;
    std::unique_ptr<RenderDescriptorSet> edram_set_;
    NativeQueryTracker queries_;
    RenderDescriptorRange range_{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1};
    struct Slot {
        std::unique_ptr<RenderBuffer> constants, positions;
        std::unique_ptr<RenderDescriptorSet> position_set;
        std::unique_ptr<RenderCommandList> commands;
    };
    Words ReadStorage() {
        auto readback = device_.createBuffer(RenderBufferDesc::ReadbackBuffer(kEdramBytes));
        auto commands = queue_.createCommandList();
        auto fence = device_.createCommandFence();
        commands->begin();
        commands->barriers(RenderBarrierStage::COPY,
            RenderBufferBarrier(storage_.get(),RenderBufferAccess::READ));
        commands->copyBuffer(readback.get(),storage_.get());
        commands->end();
        queue_.executeCommandLists(commands.get(),fence.get());
        queue_.waitForCommandFence(fence.get());
        const auto* data = readback->map();Check(data,"Fixture readback map failed");
        auto& native = static_cast<VulkanBuffer&>(*readback);
        Check(vmaInvalidateAllocation(native.device->allocator,native.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Fixture readback invalidate failed");
        Words result(kEdramBytes/4);
        std::memcpy(result.data(),data,kEdramBytes);
        readback->unmap();
        return result;
    }
public:
    DirectFixture(RenderDevice& device, RenderCommandQueue& queue,
                  std::span<const uint32_t> vertex, std::span<const uint32_t> original,
                  std::span<const uint32_t> atomic)
        : device_(device),queue_(queue),queries_(device) {
        storage_=device.createBuffer(RenderBufferDesc::DefaultBuffer(kEdramBytes,
            RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        vertex_=device.createShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
        original_=device.createShader(original.data(),original.size_bytes(),"main",RenderShaderFormat::SPIRV);
        atomic_=device.createShader(atomic.data(),atomic.size_bytes(),"main",RenderShaderFormat::SPIRV);
        const std::array<RenderDescriptorSetDesc,5> sets{{{&range_,1},{},{},{},{&range_,1}}};
        const RenderPushConstantRange push(0,0,0,sizeof(ShaderPushConstants),RenderShaderStageFlag::PIXEL);
        layout_=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,sets.data(),sets.size()));
        edram_set_=device.createDescriptorSet(sets[4]);edram_set_->setBuffer(0,storage_.get());
        RenderGraphicsPipelineDesc pipeline;
        pipeline.pipelineLayout=layout_.get();pipeline.vertexShader=vertex_.get();
        pipeline.renderTargetCount=1;pipeline.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
        pipeline.renderTargetBlend[0]=RenderBlendDesc::Copy();pipeline.renderTargetBlend[0].renderTargetWriteMask=0;
        pipeline.cullMode=RenderCullMode::NONE;pipeline.depthClipEnabled=true;
        pipeline.primitiveTopology=RenderPrimitiveTopology::TRIANGLE_LIST;
        pipeline.pixelShader=original_.get();original_pipeline_=device.createGraphicsPipeline(pipeline);
        pipeline.pixelShader=atomic_.get();atomic_pipeline_=device.createGraphicsPipeline(pipeline);
        Check(original_pipeline_&&atomic_pipeline_&&static_cast<VulkanGraphicsPipeline&>(*original_pipeline_).vk&&
            static_cast<VulkanGraphicsPipeline&>(*atomic_pipeline_).vk,"Atomic fixture pipeline creation failed");
    }
    Words RunPassGroups(bool grouped,bool order_witness,bool atomic=false) {
        Check(!atomic||!order_witness,"Atomic GE must not execute ALWAYS order witness");
        constexpr uint32_t width=1280,height=720,draws=33;
        auto expected=Seed();
        auto upload=device_.createBuffer(RenderBufferDesc::UploadBuffer(kEdramBytes));
        Upload(*upload,expected.data(),kEdramBytes);
        auto seed=queue_.createCommandList();auto seeded=device_.createCommandFence();seed->begin();
        seed->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(storage_.get(),RenderBufferAccess::WRITE));
        seed->copyBuffer(storage_.get(),upload.get());seed->end();
        queue_.executeCommandLists(seed.get(),seeded.get());queue_.waitForCommandFence(seeded.get());
        auto target=device_.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* attachment=target.get();RenderFramebufferDesc fd;
        fd.colorAttachments=&attachment;fd.colorAttachmentsCount=1;
        auto framebuffer=device_.createFramebuffer(fd);
        Check(framebuffer&&static_cast<VulkanFramebuffer&>(*framebuffer).vk&&
            static_cast<VulkanFramebuffer&>(*framebuffer).renderPass,"Grouped fixture framebuffer failed");
        std::array<Slot,draws> slots;
        const std::array<float,6> depths{.125f,.75f,.25f,.5f,1.f,0.f};
        const std::array<uint32_t,6> codes{0xc00000,0xe80000,0xd00000,0xe00000,0xf00000,0};
        const std::array<RenderRect,6> rects{{{0,0,1280,720},{39,15,81,33},{0,0,640,720},
            {40,16,1280,719},{79,0,81,720},{0,0,1280,720}}};
        for(uint32_t i=0;i<draws;++i) {
            auto& slot=slots[i];const auto positions=Fullscreen(depths[i%depths.size()]);
            NativeFsiShaderConstants constants;
            constants.raster.pitch=width;constants.raster.depth_base=0;constants.raster.depth_format=1;
            // ALWAYS is a primitive-only noncommutative order witness. Scene
            // admission remains GE only; no alternate production shader/state.
            constants.raster.depth_control=order_witness?0x700776:0x700766;
            slot.positions=device_.createBuffer(RenderBufferDesc::UploadBuffer(positions.size()*sizeof(Position),RenderBufferFlag::STORAGE));
            Upload(*slot.positions,positions.data(),positions.size()*sizeof(Position));
            slot.constants=device_.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(constants),
                RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
            Upload(*slot.constants,&constants,sizeof(constants));
            slot.position_set=device_.createDescriptorSet(RenderDescriptorSetDesc(&range_,1));
            slot.position_set->setBuffer(0,slot.positions.get());
            const auto& rect=rects[i%rects.size()];const uint32_t incoming=codes[i%codes.size()];
            for(uint32_t y=rect.top;y<uint32_t(rect.bottom);++y)for(uint32_t x=rect.left;x<uint32_t(rect.right);++x) {
                // Independent physical tile/bank formula; do not use candidate helper.
                const uint32_t word=(16*(y/16)+x/80)*1280+80*(y%16)+((x%80<40)?x%80+40:x%80-40);
                const uint32_t previous=expected.at(word);
                const uint32_t depth=order_witness?incoming:std::max(previous>>8,incoming);
                expected[word]=(depth<<8)|(previous&255);
            }
        }
        std::vector<std::unique_ptr<RenderCommandList>> commands;
        std::vector<const RenderCommandList*> command_ptrs;
        const uint32_t group_size=grouped?16:1;
        for(uint32_t first=0;first<draws;first+=group_size) {
            const auto end=std::min(draws,first+group_size);
            auto cmd=queue_.createCommandList();cmd->begin();
            // Prepare immutable source dependencies before the subpass. No
            // cross-draw storage access occurs outside the original FSI PS.
            for(uint32_t i=first;i<end;++i)cmd->barriers(RenderBarrierStage::GRAPHICS,
                RenderBufferBarrier(slots[i].positions.get(),RenderBufferAccess::READ));
            cmd->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(storage_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
            cmd->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target.get(),RenderTextureLayout::COLOR_WRITE));
            cmd->setFramebuffer(framebuffer.get());cmd->setViewports(RenderViewport(0,0,width,height));
            cmd->setGraphicsPipelineLayout(layout_.get());cmd->setGraphicsDescriptorSet(edram_set_.get(),4);
            cmd->setPipeline(atomic?atomic_pipeline_.get():original_pipeline_.get());
            for(uint32_t i=first;i<end;++i) {
                const ShaderPushConstants addresses{0,0,slots[i].constants->getDeviceAddress()};
                cmd->setGraphicsPushConstants(0,&addresses);cmd->setGraphicsDescriptorSet(slots[i].position_set.get(),0);
                cmd->setScissors(rects[i%rects.size()]);cmd->drawInstanced(3,1,0,0);
            }
            cmd->end();command_ptrs.push_back(cmd.get());commands.push_back(std::move(cmd));
        }
        auto fence=device_.createCommandFence();
        queue_.executeCommandLists(command_ptrs.data(),uint32_t(command_ptrs.size()),nullptr,0,nullptr,0,fence.get());
        queue_.waitForCommandFence(fence.get());
        auto actual=ReadStorage();Check(actual==expected,"Grouped multi-draw full10MiB independent pixel/order oracle failed");
        Check(actual!=Seed(),"Grouped primitive produced no consequential writes");
        std::cout<<"PACKED_DEPTH_PASS_PRIMITIVE "<<Json{{"grouped",grouped},{"order_witness",order_witness},
            {"draws",draws},{"render_passes",command_ptrs.size()},{"bytes_compared",kEdramBytes},
            {"canonical_shader",!atomic},{"atomic_shader",atomic},{"independent_full_memory_oracle",true}}.dump()<<'\n';
        return actual;
    }
    Outcome Run(const Case& test, bool enabled) {
        auto initial=Seed();
        auto upload=device_.createBuffer(RenderBufferDesc::UploadBuffer(kEdramBytes));
        Upload(*upload,initial.data(),kEdramBytes);
        auto seed_commands=queue_.createCommandList();auto seed_fence=device_.createCommandFence();
        seed_commands->begin();
        seed_commands->barriers(RenderBarrierStage::COPY,
            RenderBufferBarrier(storage_.get(),RenderBufferAccess::WRITE));
        seed_commands->copyBuffer(storage_.get(),upload.get());seed_commands->end();
        queue_.executeCommandLists(seed_commands.get(),seed_fence.get());queue_.waitForCommandFence(seed_fence.get());
        auto target=device_.createTexture(RenderTextureDesc::ColorTarget(test.width,test.height,RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* attachment=target.get();RenderFramebufferDesc fd;
        fd.colorAttachments=&attachment;fd.colorAttachmentsCount=1;
        auto framebuffer=device_.createFramebuffer(fd);
        std::array<Slot,6> slots;
        std::array<const RenderCommandList*,6> command_ptrs{};
        uint32_t query_index=0;
        for (unsigned i=0;i<slots.size();++i) {
            const bool candidate=i==0||i==3;
            const bool queried=!candidate;
            auto positions=candidate?Overlapping(test.reverse^(i==3),test.clipped):Fullscreen(.75f);
            NativeFsiShaderConstants constants;
            auto& raster=constants.raster;
            raster.pitch=test.pitch;raster.depth_base=test.base;raster.depth_format=1;
            raster.depth_control=candidate?0x700766:i==1?0x700762:
                i==2?(1u|(7u<<8)|(2u<<14)):i==4?2u:0u;
            if(candidate) {
                raster.front_constant_bias=raster.back_constant_bias=test.constant_bias;
                raster.front_slope_bias=raster.back_slope_bias=test.slope_bias;
                Check(BuiltinAtomicDepthAllowed(raster,test.width,test.height,0,false,false,true,test.pitch),
                    "Direct candidate failed the exact live-state gate");
            }
            if(i==2)raster.stencil_front=raster.stencil_back=0x00ffffa5;
            auto& slot=slots[i];
            slot.positions=device_.createBuffer(RenderBufferDesc::UploadBuffer(positions.size()*sizeof(Position),RenderBufferFlag::STORAGE));
            Upload(*slot.positions,positions.data(),positions.size()*sizeof(Position));
            slot.constants=device_.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(constants),
                RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
            Upload(*slot.constants,&constants,sizeof(constants));
            slot.position_set=device_.createDescriptorSet(RenderDescriptorSetDesc(&range_,1));
            slot.position_set->setBuffer(0,slot.positions.get());
            const ShaderPushConstants addresses{0,0,slot.constants->getDeviceAddress()};
            slot.commands=queue_.createCommandList();auto& cmd=*slot.commands;cmd.begin();
            // Same full-buffer dependencies as the regular Scene path. Atomic
            // reads/writes must still obey original→atomic and atomic→original.
            cmd.barriers(RenderBarrierStage::GRAPHICS,
                RenderBufferBarrier(storage_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
            cmd.barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(slot.positions.get(),RenderBufferAccess::READ));
            cmd.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target.get(),RenderTextureLayout::COLOR_WRITE));
            cmd.setFramebuffer(framebuffer.get());cmd.setViewports(RenderViewport(0,0,test.width,test.height));
            const RenderRect scissor=candidate&&test.inset?RenderRect(7,3,test.width-5,test.height-2):RenderRect(0,0,test.width,test.height);
            cmd.setScissors(scissor);cmd.setGraphicsPipelineLayout(layout_.get());
            cmd.setGraphicsPushConstants(0,&addresses);
            cmd.setGraphicsDescriptorSet(slot.position_set.get(),0);cmd.setGraphicsDescriptorSet(edram_set_.get(),4);
            cmd.setPipeline(enabled&&candidate?atomic_pipeline_.get():original_pipeline_.get());
            const uint32_t report=0x2000+64*query_index;
            if(queried) {
                queries_.Begin(query_index+1,report+32);queries_.ResetDraw(cmd);queries_.BeginDraw(cmd);
            }
            cmd.drawInstanced(uint32_t(positions.size()),1,0,0);
            if(queried)queries_.EndDraw(cmd);
            cmd.end();
            if(queried) {queries_.EndDeferred(query_index+1,report);++query_index;}
            command_ptrs[i]=slot.commands.get();
        }
        Outcome outcome;
        // First draw itself contains contention; read it before a generic
        // stencil writer can obscure an incorrect low-byte/depth update.
        auto first=device_.createCommandFence();
        queue_.executeCommandLists(command_ptrs[0],first.get());queue_.waitForCommandFence(first.get());
        outcome.after_first=ReadStorage();
        size_t changed=0;
        for(size_t i=0;i<initial.size();++i) {
            Check((outcome.after_first[i]&255)==(initial[i]&255),"Atomic candidate changed S8");
            Check(outcome.after_first[i]>=initial[i],"GE candidate decreased packed depth");
            changed+=outcome.after_first[i]!=initial[i];
        }
        if(std::isfinite(test.constant_bias))Check(changed!=0,"Contention draw produced no packed depth writes");
        // Two submissions are live together. First has read-only query and
        // generic stencil writer; second starts with atomic work, then generic
        // rejecting/always-pass queries. No optimized draw owns a query slot.
        auto a=device_.createCommandFence(),b=device_.createCommandFence();
        queue_.executeCommandLists(command_ptrs.data()+1,2,nullptr,0,nullptr,0,a.get());
        queue_.executeCommandLists(command_ptrs.data()+3,3,nullptr,0,nullptr,0,b.get());
        queue_.waitForCommandFence(a.get());queries_.CompletePrefix(2);
        for(const auto& query:queries_.TakeDeferredPrefix(2))outcome.samples.push_back(query.samples);
        Check(queries_.pending_draws()==2,"First fence retired later generic query draws");
        queue_.waitForCommandFence(b.get());queries_.CompletePrefix(2);
        for(const auto& query:queries_.TakeDeferredResults())outcome.samples.push_back(query.samples);
        Check(outcome.samples.size()==4&&outcome.samples[1]==uint64_t(test.width)*test.height&&
            outcome.samples[2]==0&&outcome.samples[3]==uint64_t(test.width)*test.height,
            "Generic successor query counts changed");
        outcome.final_bytes=ReadStorage();
        const SurfaceLayout surface{test.width,test.height,test.pitch,test.base,1,true};
        for(uint32_t y=0;y<test.height;++y)for(uint32_t x=0;x<test.width;++x)
            Check((outcome.final_bytes[SurfaceByteOffset(surface,x,y)/4]&255)==0xa5,
                "Atomic draw lost preceding generic stencil replacement");
        return outcome;
    }
};
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
Outcome CapturedScene(RenderDevice& device,RenderCommandQueue& queue,
    std::span<const uint32_t> original,const std::array<Words,2>& vertices,
    const std::filesystem::path& inspected,bool enabled,bool asynchronous) {
    std::ifstream stream(inspected/"scene.json");Json plan;stream>>plan;
    std::array<Json,2> draws;
    for(const auto& op:plan.at("operations")) {
        if(op.value("type","")!="draw"||!op.at("pixel_shader").is_null())continue;
        for(size_t i=0;i<draws.size();++i)
            if(draws[i].is_null()&&op.at("vertex_shader")==kAtomicDepthVertexSources[i]&&op.at("textures").empty())draws[i]=op;
    }
    Check(!draws[0].is_null()&&!draws[1].is_null(),"Capture lacks both audited depth families");
    std::map<std::string,Bytes> overrides;
    const auto payload=[&](const std::string& hash) {
        const auto found=overrides.find(hash);
        return found==overrides.end()?ReadBytes(inspected/(hash+".payload.bin")):found->second;
    };
    const auto state=[&](Json& draw,uint32_t control) {
        auto bytes=payload(draw.at("device_state").get<std::string>());
        StoreWord(bytes,0x2934,control);StoreWord(bytes,0x28dc,0);
        StoreWord(bytes,0x2900,0x00ffffa5);StoreWord(bytes,0x28fc,0x00ffffa5);
        auto hash=ContentHash(bytes);overrides.emplace(hash,std::move(bytes));draw["device_state"]=hash;
    };
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.builtin_atomic_depth=enabled;config.batch_transfers=true;config.unified_gpu_queue=true;
    config.async_gpu_submissions=asynchronous;config.max_draw_batch_size=2;config.defer_native_queries=true;
    config.payload=payload;
    config.shader=[&](const std::string& hash) {
        for(size_t i=0;i<vertices.size();++i)if(hash==kAtomicDepthVertexSources[i])return ToBytes(vertices[i]);
        throw std::runtime_error("Unexpected captured fixture shader");
    };
    config.fsi_depth_shader=[&]{return ToBytes(original);};
    SceneExecutor scene(config,&device,&queue);
    const bool atomic_available = enabled && bool(TryBuiltinAtomicDepth(original));
    std::array<NativeFsiPipelineRecipe, 2> recipes;
    // A portable generic recipe prepares both separately keyed pipelines when
    // the guarded variant is available. Actual draw/query eligibility is still
    // checked later; no recipe stands in for dynamic guest state.
    for(size_t i=0;i<draws.size();++i) {
        const auto raw=payload(draws[i].at("device_state").get<std::string>());
        std::array<uint32_t,kDrawStateWords> words;
        for(size_t n=0;n<words.size();++n) {
            const size_t at=kDrawStateOffset+n*4;
            words[n]=(uint32_t(raw.at(at))<<24)|(uint32_t(raw.at(at+1))<<16)|
                (uint32_t(raw.at(at+2))<<8)|raw.at(at+3);
        }
        const auto& depth=draws[i].at("surfaces").at("10");
        const auto decoded=DecodeDrawState(words,depth.at("width"),depth.at("height"),
            draws[i].at("args").at(1),true);
        const auto& pipeline=decoded.pipeline;
        auto& recipe = recipes[i];
        recipe = NativeFsiPipelineRecipe{std::string(kAtomicDepthVertexSources[i]),std::nullopt,
            {uint32_t(pipeline.cullMode),uint32_t(pipeline.frontFace),uint32_t(pipeline.depthClipEnabled),
             uint32_t(pipeline.primitiveTopology),decoded.specialization,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
        scene.PrewarmFsiPipeline(recipe,vertices[i],original);
    }
    const Json prepared = scene.report();
    Check(prepared.at("pipelines_created") == (atomic_available ? 4 : 2) &&
        prepared.at("shader_cache").at("creations") == (atomic_available ? 4 : 3) &&
        !scene.mutation_serial() && !scene.HasPendingGpuWork(),
        "Prewarm changed guest work or lost the generic/atomic identities");
    for (size_t i = 0; i < recipes.size(); ++i)
        scene.PrewarmFsiPipeline(recipes[i], vertices[i], original);
    Check(scene.report().at("pipelines_created") == prepared.at("pipelines_created") &&
        scene.report().at("shader_cache").at("creations") == prepared.at("shader_cache").at("creations"),
        "Repeated startup recipes recreated prepared pipelines/modules");

    if (atomic_available && !asynchronous) {
        // Each hard limit independently refuses only optional extra warmup.
        // Generic admission remains exact and does not lose its selected VS.
        for (unsigned limit = 0; limit < 3; ++limit) {
            auto bounded_config = config;
            if (limit == 0) bounded_config.max_pipeline_cache_entries = 1;
            if (limit == 1) bounded_config.max_shader_cache_entries = 2;
            if (limit == 2)
                bounded_config.max_shader_cache_bytes = vertices[0].size() * sizeof(uint32_t) + original.size_bytes();
            SceneExecutor bounded(bounded_config, &device, &queue);
            bounded.PrewarmFsiPipeline(recipes[0], vertices[0], original);
            bounded.PrewarmFsiPipeline(recipes[0], vertices[0], original);
            const auto& report = bounded.report();
            Check(report.at("pipelines_created") == 1 && report.at("shader_cache").at("creations") == 2 &&
                report.at("pipeline_cache").at("evictions") == 0 &&
                report.at("shader_cache").at("evictions") == 0 &&
                report.at("pipeline_cache").at("pressure_drains") == 0 &&
                report.at("shader_cache").at("pressure_drains") == 0 &&
                !bounded.mutation_serial() && !bounded.HasPendingGpuWork(),
                "Optional prewarm evicted generic state or exceeded a cache limit");
        }
    }
    scene.InitializeEdram(ToBytes(Seed()));
    uint64_t event=0;
    const auto submit=[&](Json draw,uint32_t control) {
        state(draw,control);draw["frame"]=1;draw["event"]=++event;scene.Execute(draw);
    };
    // Atomic→generic→atomic on each actual audited VS. Active query uses the
    // *same*0x700766 state, proving runtime refusal rather than a changed control.
    for(unsigned i=0;i<draws.size();++i) {
        submit(draws[i],0x700766);
        scene.Execute({{"type","query_begin"},{"frame",1},{"event",++event},{"query_generation",i+1},
            {"query_handle",0x1000+32*i},{"report_address",0x2020+64*i}});
        submit(draws[i],0x700766);
        scene.Execute({{"type","query_end"},{"frame",1},{"event",++event},{"query_generation",i+1},
            {"query_handle",0x1000+32*i},{"report_address",0x2000+64*i}});
        submit(draws[i],1u|(7u<<8)|(2u<<14));
        submit(draws[i],0x700766);
    }
    scene.FlushGpuWork();Outcome result;
    for(const auto& query:scene.TakeCompletedQueries())result.samples.push_back(query.result.samples);
    Check(result.samples.size()==2,"Captured Scene lost generic queries");
    Check(std::any_of(result.samples.begin(),result.samples.end(),[](uint64_t n){return n!=0;}),
        "Captured geometry produced no real queried samples");
    const auto& report=scene.report();
    if(enabled) {
        const auto& stats=report.at("builtin_atomic_depth");
        Check(stats.at("selected") == (atomic_available ? 4 : 0) && stats.at("candidates") == 4 &&
            stats.at("module_refusals") == (atomic_available ? 0 : 1) && stats.at("vertex_refusals") == 0,
            "Scene atomic selection/active-query/stencil refusal counts differ");
        Check(report.at("pipelines_created") == (atomic_available ? 4 : 2),
            "Audited generic/atomic pipeline identities collided");
    } else Check(report.at("pipelines_created")==2,"Generic captured pipeline count differs");
    Check(report.at("pipelines_created") == prepared.at("pipelines_created") &&
        report.at("shader_cache").at("creations") == prepared.at("shader_cache").at("creations"),
        "Actual atomic/query/stencil transitions recreated prepared pipelines/modules");
    result.final_bytes=ReadSceneEdram(device,queue,scene.ExportState().edram);
    Check(result.final_bytes!=Seed(),"Captured scene produced no packed EDRAM writes");
    return result;
}
struct GroupedSceneResult {Words edram;Json queries,report;};
GroupedSceneResult CapturedPassGroups(RenderDevice& device,RenderCommandQueue& queue,
    std::span<const uint32_t> original,const std::array<Words,2>& vertices,
    const std::filesystem::path& inspected,bool enabled,uint32_t capacity,bool unpinned=false,bool atomic=false) {
    std::ifstream stream(inspected/"scene.json");Json plan;stream>>plan;
    std::array<Json,2> draws;
    for(const auto& op:plan.at("operations")) {
        if(op.value("type","")!="draw"||!op.at("pixel_shader").is_null()||!op.at("textures").empty())continue;
        const auto& depth=op.at("surfaces").at("10");
        if(depth.at("width")!=1280||depth.at("height")!=720)continue;
        for(size_t i=0;i<draws.size();++i)
            if(draws[i].is_null()&&op.at("vertex_shader")==kAtomicDepthVertexSources[i])draws[i]=op;
    }
    Check(!draws[0].is_null()&&!draws[1].is_null(),"Capture lacks primary audited depth writers");
    std::map<std::string,Bytes> overrides;
    const auto payload=[&](const std::string& hash) {
        const auto found=overrides.find(hash);
        return found==overrides.end()?ReadBytes(inspected/(hash+".payload.bin")):found->second;
    };
    const auto state=[&](Json& draw,uint32_t control) {
        auto bytes=payload(draw.at("device_state").get<std::string>());
        StoreWord(bytes,0x2934,control);StoreWord(bytes,0x28dc,0);
        StoreWord(bytes,0x2900,0x00ffffa5);StoreWord(bytes,0x28fc,0x00ffffa5);
        auto hash=ContentHash(bytes);overrides.emplace(hash,std::move(bytes));draw["device_state"]=hash;
    };
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.packed_depth_render_pass_groups=enabled;config.batch_transfers=true;config.unified_gpu_queue=true;
    config.builtin_atomic_depth=atomic;
    config.async_gpu_submissions=true;config.async_submission_capacity=capacity;config.max_draw_batch_size=16;
    config.defer_native_queries=config.retire_completed_query_prefixes=true;
    config.native_early_depth=true;config.payload=payload;
    config.shader=[&](const std::string& hash) {
        for(size_t i=0;i<vertices.size();++i)if(hash==kAtomicDepthVertexSources[i])return ToBytes(vertices[i]);
        throw std::runtime_error("Unexpected grouped Scene shader");
    };
    Words module(original.begin(),original.end());if(unpinned)module[2]^=1;
    config.fsi_depth_shader=[&]{return ToBytes(module);};
    SceneExecutor scene(config,&device,&queue);
    for(size_t i=0;i<draws.size();++i) {
        const auto raw=payload(draws[i].at("device_state").get<std::string>());
        std::array<uint32_t,kDrawStateWords> words;
        for(size_t n=0;n<words.size();++n) {
            const size_t at=kDrawStateOffset+n*4;
            words[n]=(uint32_t(raw.at(at))<<24)|(uint32_t(raw.at(at+1))<<16)|(uint32_t(raw.at(at+2))<<8)|raw.at(at+3);
        }
        const auto decoded=DecodeDrawState(words,1280,720,draws[i].at("args").at(1),true);
        const auto& pipe=decoded.pipeline;
        const NativeFsiPipelineRecipe recipe{std::string(kAtomicDepthVertexSources[i]),std::nullopt,
            {uint32_t(pipe.cullMode),uint32_t(pipe.frontFace),uint32_t(pipe.depthClipEnabled),
             uint32_t(pipe.primitiveTopology),decoded.specialization,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
        scene.PrewarmFsiPipeline(recipe,vertices[i],module);
    }
    const Json prepared=scene.report();
    Check(prepared.at("pipelines_created")==unsigned(atomic&&!unpinned?4:2)&&prepared.at("shader_cache").at("creations")==unsigned(atomic&&!unpinned?4:3)&&
        !scene.HasPendingGpuWork(),"Packed groups changed canonical startup pipeline/module identities");
    scene.InitializeEdram(ToBytes(Seed()));uint64_t event=0;
    const auto record=[&](Json operation) {
        operation["frame"]=17;operation["event"]=++event;
        scene.FlushCommittedState();scene.Execute(operation);
    };
    const auto draw=[&](unsigned index,uint32_t control) {auto op=draws[index];state(op,control);record(op);};
    const auto begin=[&](uint64_t generation) {record({{"type","query_begin"},{"query_generation",generation},
        {"query_handle",0x1000},{"report_address",0x2020}});};
    const auto end=[&](uint64_t generation) {record({{"type","query_end"},{"query_generation",generation},
        {"query_handle",0x1000},{"report_address",0x2000}});};
    // Warm any immutable resize/geometry state before exceeding the logical
    // slot limit. Long groups alternate actual VS identities and owned inputs.
    draw(0,0x700766);draw(1,0x700766);scene.FlushGpuWork();
    const uint32_t count=2*capacity+5;
    for(uint32_t i=0;i<count;++i)draw(i&1,0x700766);
    // A real empty query observes the accepted but unsubmitted grouped tail.
    begin(3000);end(3000);
    begin(3001);draw(0,0x700766);draw(1,0x700766);end(3001);
    draw(0,1u|(7u<<8)|(2u<<14)); // canonical stencil replacement must close the pass
    for(uint32_t i=0;i<19;++i)draw(i&1,0x700766);
    begin(3002);draw(0,0x700762);draw(1,0x700762);end(3002); // future read-only packed query
    scene.FlushGpuWork();GroupedSceneResult result;result.queries=Json::array();
    for(const auto& query:scene.TakeCompletedQueries())result.queries.push_back({query.frame,query.generation,
        query.end_event,query.result.handle,query.result.begin_address,query.result.end_address,
        query.result.draws,query.result.samples});
    Check(result.queries.size()==3&&result.queries[0][6]==0&&result.queries[0][7]==0&&
        result.queries[1][6]==2&&result.queries[1][7].get<uint64_t>()>0&&result.queries[2][6]==2&&
        result.queries[2][7].get<uint64_t>()>0,"Grouped Scene query identity/positive/empty proof is vacuous");
    result.edram=ReadSceneEdram(device,queue,scene.ExportState().edram);result.report=scene.report();
    Check(result.edram!=Seed(),"Grouped Scene actual captured geometry wrote no packed bytes");
    const auto& report=result.report;const auto& work=report.at("gpu_work_queue");
    const auto& stats=report.at("packed_depth_render_pass_groups");
    Check(work.at("accepted_work")==work.at("completed_work")&&!scene.HasPendingGpuWork()&&
        work.at("maximum_lists").get<uint64_t>()<=capacity,"Grouped Scene lost logical fence/owner accounting");
    Check(report.at("pipelines_created")==prepared.at("pipelines_created")&&
        report.at("shader_cache").at("creations")==prepared.at("shader_cache").at("creations"),
        "Packed grouping created an alternate module or pipeline");
    if(enabled&&!unpinned) {
        Check(stats.at("selected").get<uint64_t>()>capacity&&stats.at("maximum_group_draws")==16&&
            stats.at("render_passes_avoided").get<uint64_t>()>capacity&&
            stats.at("query_refusals")==4&&stats.at("refusals").get<uint64_t>()>=5,
            "Packed grouping actual selection or query/generic refusal is vacuous");
        Check(stats.at("selected").get<uint64_t>()==stats.at("closed_render_pass_groups").get<uint64_t>()+
                stats.at("render_passes_avoided").get<uint64_t>()&&
            stats.at("eligible").get<uint64_t>()==stats.at("selected").get<uint64_t>()+
                stats.at("preparation_refusals").get<uint64_t>()&&
            stats.at("candidates").get<uint64_t>()==stats.at("eligible").get<uint64_t>()+
                stats.at("refusals").get<uint64_t>()&&
            stats.at("edram_barriers_avoided")==stats.at("render_passes_avoided")&&
            stats.at("dummy_barriers_avoided")==stats.at("render_passes_avoided"),
            "Packed pass/group/barrier conservation differs");
    } else Check(stats.at("selected")==0&&stats.at("render_passes_avoided")==0,
        "Disabled or unpinned shader joined a packed render pass group");
    Check(stats.at("atomic_selected")==uint64_t(atomic?stats.at("selected").get<uint64_t>():0)&&
        stats.at("atomic_render_passes_avoided")==uint64_t(atomic?stats.at("render_passes_avoided").get<uint64_t>():0),
        "Actual atomic grouped member/barrier coverage differs");
    std::cout<<"PACKED_DEPTH_PASS_SCENE "<<Json{{"capacity",capacity},{"enabled",enabled},{"unpinned",unpinned},{"atomic",atomic},
        {"candidate",stats},{"queue",work},{"queries",result.queries},{"bytes_compared",kEdramBytes}}.dump()<<'\n';
    // Destructor must close and fence an accepted partial render pass before
    // destroying per-draw descriptors, upload pages and the exact framebuffer.
    draw(0,0x700766);draw(1,0x700766);
    Check(scene.HasPendingDraws(),"Grouped destructor tail completed before its owner witness");
    return result;
}
} // namespace

int main(int argc,char** argv)try {
    if(argc!=6&&argc!=8)throw std::runtime_error(
        "Usage: builtin-atomic-depth-gpu-test builtin.spv fixture-vertex.spv a14c.spv 3f77.spv inspected-directory [--packed-pass-groups 64|128]");
    const auto original=ReadWords(argv[1]),fixture_vertex=ReadWords(argv[2]);
    const std::array<Words,2> vertices{ReadWords(argv[3]),ReadWords(argv[4])};
    auto transformed=TryBuiltinAtomicDepth(original);
    Check(bool(transformed),"Pinned original builtin was refused");
    for(size_t i=0;i<vertices.size();++i)
        Check(AuditedAtomicDepthVertex(kAtomicDepthVertexSources[i],ToBytes(vertices[i])),"Audited vertex binary mismatch");
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    DirectFixture fixture(device,*queue,fixture_vertex,original,*transformed);
    if(argc==8) {
        Check(std::string(argv[6])=="--packed-pass-groups","Unknown atomic fixture option");
        const uint32_t capacity=std::stoul(argv[7]);Check(capacity==64||capacity==128,"Grouped capacity must be64or128");
        Check(std::endian::native==std::endian::little,"Packed fixture requires little-endian host");
        for(bool order:{false,true}) {
            const auto off=fixture.RunPassGroups(false,order),on=fixture.RunPassGroups(true,order);
            Check(off==on,"Grouped cross-draw packed memory/order parity failed");
        }
        const auto off=CapturedPassGroups(device,*queue,original,vertices,argv[5],false,capacity);
        const auto on=CapturedPassGroups(device,*queue,original,vertices,argv[5],true,capacity);
        const auto refused=CapturedPassGroups(device,*queue,original,vertices,argv[5],true,capacity,true);
        Check(off.edram==on.edram&&off.edram==refused.edram&&off.queries==on.queries&&off.queries==refused.queries,
            "Actual grouped Scene or changed-module fallback altered full EDRAM/query identity");
        const auto atomic_primitive=fixture.RunPassGroups(true,false,true);
        const auto original_primitive=fixture.RunPassGroups(false,false);
        Check(atomic_primitive==original_primitive,"Cross-draw atomic maximum/stencil differs from independent original oracle");
        const auto atomic_on=CapturedPassGroups(device,*queue,original,vertices,argv[5],true,capacity,false,true);
        const auto atomic_refused=CapturedPassGroups(device,*queue,original,vertices,argv[5],true,capacity,true,true);
        Check(atomic_on.edram==off.edram&&atomic_on.queries==off.queries&&
              atomic_refused.edram==off.edram&&atomic_refused.queries==off.queries,
              "Actual atomic groups or refused transform changed full memory/real queries");
        std::cout<<"PACKED_DEPTH_PASS_GROUPS_PASS capacity="<<capacity
            <<" primitive_pairs=3 scene_arms=5 full_memory_bytes=10485760 canonical_and_atomic_shaders=true\n";
        return 0;
    }
    std::array<Case,8> cases;
    cases[1].reverse=true;
    cases[2].constant_bias=std::ldexp(1.f,-24);cases[2].inset=true;
    cases[3].constant_bias=-std::ldexp(1.f,-24);cases[3].slope_bias=.0625f;
    cases[4].base=2047;cases[4].clipped=true;
    cases[5].width=83;cases[5].height=35;cases[5].pitch=240;cases[5].base=2046;cases[5].reverse=true;
    // NaN enters the existing fragment offset/pack path. No NaN vertex or
    // undefined raster-coverage result is used as a claimed numeric oracle.
    cases[6].constant_bias=std::numeric_limits<float>::quiet_NaN();
    cases[7].constant_bias=-.125f;cases[7].slope_bias=-.03125f;cases[7].clipped=true;
    for(size_t i=0;i<cases.size();++i) {
        const auto off=fixture.Run(cases[i],false),on=fixture.Run(cases[i],true);
        Check(off.after_first==on.after_first,"Within-one-draw overlapping triangle packed parity failed");
        Check(off.final_bytes==on.final_bytes&&off.samples==on.samples,"Atomic/generic successor parity failed");
        std::cout<<"Direct case "<<i<<": exact10MiB, preserved S8, query samples";
        for(auto samples:on.samples)std::cout<<' '<<samples;
        std::cout<<'\n';
    }
    for(bool asynchronous:{false,true}) {
        const auto off=CapturedScene(device,*queue,original,vertices,argv[5],false,asynchronous);
        const auto on=CapturedScene(device,*queue,original,vertices,argv[5],true,asynchronous);
        Check(off.final_bytes==on.final_bytes&&off.samples==on.samples,
            "Actual audited Scene atomic/generic output or query parity failed");
        // Changing only the SPIR-V generator word preserves valid generic GPU
        // behavior but fails the exact builtin pin. Warmup must keep Native's
        // generic pipeline and never admit or repeatedly retry the candidate.
        auto unpinned = original;
        unpinned[2] ^= 1u;
        const auto refused = CapturedScene(device, *queue, unpinned, vertices, argv[5], true, asynchronous);
        Check(off.final_bytes == refused.final_bytes && off.samples == refused.samples,
            "Unpinned startup builtin changed generic Native rendering/query behavior");
    }
    std::cout<<"PASS:8 paired contention/bias/clipping/wrapping cases; actual audited Scene sync/async selection,query refusal,pipeline separation and startup reuse/pressure/refusal; exact full packed EDRAM\n";
}catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
}
