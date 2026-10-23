#include "descriptor_content_scene_test.h"
#include "renderer/native/descriptor_content_cache.h"
#include "renderer/native/texture_data.h"
#include "fullscreen_spv.h"
#include "descriptor_batch_test_spv.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <tuple>
#include <unordered_map>
using namespace plume;
using namespace gow3::renderer;
namespace {
using Json=nlohmann::json;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T,size_t N>std::vector<uint8_t> ShaderBytes(const T(&data)[N]) {
    std::vector<uint8_t> bytes(sizeof(data));std::memcpy(bytes.data(),data,sizeof(data));return bytes;
}
std::vector<uint8_t> ReadEdram(RenderDevice& device,RenderCommandQueue& queue,SceneExecutor& scene) {
    const auto exported=scene.ExportState();
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(2048*5120));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    const auto command=static_cast<VulkanCommandList&>(*commands).vk;
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer=exported.edram.buffer;barrier.size=VK_WHOLE_SIZE;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,1,&barrier,0,nullptr);
    const VkBufferCopy copy{0,0,2048*5120};
    vkCmdCopyBuffer(command,exported.edram.buffer,static_cast<VulkanBuffer&>(*readback).vk,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    auto mapped=readback->map();Require(mapped!=nullptr,"Descriptor content readback mapping failed");
    auto& buffer=static_cast<VulkanBuffer&>(*readback);
    Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
        "Descriptor content readback invalidation failed");
    std::vector<uint8_t> bytes(2048*5120);std::memcpy(bytes.data(),mapped,bytes.size());readback->unmap();
    return bytes;
}
struct Result {
    std::vector<uint8_t> edram;
    std::vector<std::array<uint64_t,3>> queries;
    Json descriptors,contents;
};
}

// Real paired execution proves active descriptor contents, rather than just
// metadata hits. All dimensions are dynamically sampled by the unchanged GLSL
// shader; changed black/red inputs change real occlusion samples. Per-slot
// rings exceed both logical capacities. GPU alias history, sixteen cached
// sampler variants followed by transient owners, partial auto-flush failure,
// exact query identities and full EDRAM guards have independent oracles.
void CheckDescriptorContentCache(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig base,const std::vector<uint8_t>& state) {
    const std::string state_id(64,'3'),vs_id(64,'1'),ps_id(64,'2');
    const std::array<uint32_t,3> slots{31,17,11};
    std::array<std::array<uint32_t,6>,3> fetches;
    for(unsigned i=0;i<3;++i)fetches[i]={2u|(1u<<22),(0x100000u+i*0x20000u)|38u,
        i==2?5u<<26:0u,(1u<<4)|(2u<<7)|(3u<<10),0,(i+1)<<9};
    base.payload={};base.shader_snapshot={};base.observe_pipeline={};
    base.native_fsi=base.defer_native_queries=base.batch_transfers=base.unified_gpu_queue=true;
    base.async_gpu_submissions=true;base.max_draw_batch_size=16;
    base.shader=[&](const std::string& key) {
        if(key==vs_id)return ShaderBytes(fullscreen_spv);
        Require(key==ps_id,"Descriptor content requested unexpected shader");return ShaderBytes(descriptor_batch_test_spv);
    };
    base.fsi_shader=[&](const std::string& key) {
        Require(key==ps_id,"Descriptor content requested unexpected FSI shader");return ShaderBytes(descriptor_batch_test_spv);
    };
    const auto run=[&](bool enabled,bool batched) {
        auto config=base;config.cache_draw_descriptors=enabled;config.batch_draw_descriptors=batched;
        std::unordered_map<std::string,OwnedPayload::Snapshot> inputs;
        inputs[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
        config.payload_snapshot=[&](const std::string& key){return inputs.at(key);};
        auto texture=[&](unsigned which,bool black) {
            const auto layout=DescribeTexture(fetches[which]);size_t bytes=0;
            for(const auto& level:layout.levels)
                bytes=std::max(bytes,size_t(TextureBlockOffset(layout,level,0,0,0)+16));
            std::vector<uint8_t> data(bytes);
            std::array<float,4> color{0,0,0,1};if(!black)color[which]=1;
            for(const auto& level:layout.levels)
                std::memcpy(data.data()+TextureBlockOffset(layout,level,0,0,0),color.data(),16);
            const std::string hash(64,char('4'+which+(black?3:0)));
            inputs[hash]=std::make_shared<const std::vector<uint8_t>>(std::move(data));
            return Json{{"slot",slots[which]},{"source","cpu_snapshot"},{"fetch",fetches[which]},
                {"base_payload",hash},{"mip_payload",nullptr}};
        };
        const auto red=texture(0,false),black=texture(0,true),green=texture(1,false),blue=texture(2,false);
        const Json rgb=Json::array({red,green,blue}),blackgb=Json::array({black,green,blue});
        SceneExecutor scene(config,&device,&queue);uint64_t event=0,generation=1800;
        std::vector<uint8_t> seed(2048*5120);
        for(size_t at=0;at<seed.size();++at)seed[at]=uint8_t(at*13+7);
        scene.InitializeEdram(seed);Result result;
        const Json surface{{"depth",false},{"edram_base",0},{"format",0},{"format_info",0},
            {"surface_info",80},{"width",16},{"height",16}};
        auto draw=[&](const Json& textures) {
            scene.Execute({{"type","draw"},{"frame",7},{"event",++event},{"function","DrawPrimitive"},
                {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vs_id},
                {"pixel_shader",ps_id},{"surfaces",{{"6",surface}}},{"geometry",Json::array()},
                {"vertex_slots",Json::array()},{"textures",textures}});
        };
        auto begin=[&] {
            scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2020}});
        };
        auto end=[&](uint64_t draws,uint64_t samples) {
            scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2000}});
            scene.FlushGpuWork();const auto completed=scene.TakeCompletedQueries();
            Require(completed.size()==1&&completed[0].generation==generation&&
                completed[0].result.draws==draws&&completed[0].result.samples==samples,
                "Descriptor contents changed actual sparse-dimensional query samples or identities");
            result.queries.push_back({generation,draws,samples});++generation;
        };
        const uint32_t capacity=config.async_submission_capacity;
        begin();for(uint32_t i=0;i<2*capacity+3;++i)draw(rgb);
        end(2*capacity+3,uint64_t(2*capacity+3)*256);
        begin();for(uint32_t i=0;i<capacity+1;++i)draw(blackgb);end(capacity+1,0);
        begin();for(uint32_t i=0;i<capacity+1;++i)draw(rgb);end(capacity+1,uint64_t(capacity+1)*256);
        // Restore the last repeated-slot input after a chunk-sized auto-flush.
        // Every original image/sampler destination remains last-write-wins.
        Json repeated=Json::array();for(unsigned i=0;i<35;++i)repeated.push_back(i&1?black:red);
        repeated.push_back(green);repeated.push_back(blue);
        begin();draw(repeated);end(1,256);
        // An original writer may already have auto-flushed descriptors when a
        // later source fails. Abort must invalidate all possibly changed cache
        // stamps while preserving earlier accepted GPU work and its owners.
        const auto aborts_before=enabled&&batched?scene.report().at("draw_descriptor_content_cache").at("aborts").get<uint64_t>():0;
        auto invalid=black;invalid["base_payload"]=std::string(64,'0');
        auto failing=repeated;failing.push_back(invalid);bool refused=false;
        try{draw(failing);}catch(const std::out_of_range&){refused=true;}
        Require(refused,"Descriptor preparation accepted a missing later payload");
        if(enabled&&batched)Require(scene.report().at("draw_descriptor_content_cache").at("aborts")==aborts_before+1,
            "Partial descriptor preparation did not invalidate its transaction");
        begin();for(uint32_t i=0;i<capacity+1;++i)draw(rgb);end(capacity+1,uint64_t(capacity+1)*256);
        // Canonical GPU history creates immutable image+view generations for
        // the same physical address. Sampler-only fetch variants share that
        // generation but have distinct immutable bindings. At variant16 the
        // existing per-image16 bound retains transient DrawSlot owners instead.
        constexpr uint32_t address=0x5000;
        std::vector<uint8_t> physical(4096);std::array<float,4> physical_red{1,0,0,1};
        std::memcpy(physical.data(),physical_red.data(),16);scene.InitializeMemory(address,physical);
        const Json identity{{"base","00005000"},{"format",38},{"width",1},{"height",1}};
        auto alias=[&](unsigned variant) {
            auto fetch=fetches[0];fetch[1]=address|38;
            fetch[0]|=((variant&3u)<<10)|(((variant/4)&3u)<<13);
            if(variant>=16)fetch[4]=(variant-15)<<12;
            return Json{{"slot",31},{"source","gpu_alias_or_previous_frame"},{"identity",identity},{"fetch",fetch}};
        };
        begin();for(unsigned variant=0;variant<20;++variant)draw(Json::array({alias(variant),green,blue}));end(20,20*256);
        begin();for(uint32_t i=0;i<capacity+1;++i)draw(Json::array({alias(0),green,blue}));
        end(capacity+1,uint64_t(capacity+1)*256);
        const auto update_id=std::string(64,'a');
        for(bool is_black:{true,false}) {
            std::array<float,4> color{is_black?0.f:1.f,0,0,1};
            std::memcpy(physical.data(),color.data(),16);
            inputs[update_id]=std::make_shared<const std::vector<uint8_t>>(physical);
            // Keep an older alias-consuming draw pending before the overwrite;
            // the original CPU write ordering and immutable owner retirement
            // are unchanged by descriptor metadata reuse.
            begin();draw(Json::array({alias(0),green,blue}));
            Require(scene.HasPendingGpuWork(),"Alias replacement fixture lost its still-owned older generation");
            const uint64_t previous_samples=is_black?256:0;
            scene.Execute({{"type","cpu_memory_write"},{"frame",7},{"event",++event},
                {"address",address},{"bytes",4096},{"payload",update_id},{"coherence_epoch",event}});
            for(uint32_t i=0;i<capacity+1;++i)draw(Json::array({alias(0),green,blue}));
            end(capacity+2,previous_samples+(is_black?0:uint64_t(capacity+1)*256));
        }
        begin();for(unsigned variant=0;variant<20;++variant)draw(Json::array({alias(variant),green,blue}));end(20,20*256);
        // EndFrame snapshots the existing bounded binding-cache diagnostics.
        scene.Execute({{"type","present"},{"frame",7},{"event",++event},
            {"frontbuffer",identity},{"frontbuffer_fetch",alias(0).at("fetch")},
            {"provenance","descriptor content fixture"}});
        const auto report=scene.report();
        Require(report.at("texture_binding_cache").at("transient_created").get<uint64_t>()>=4,
            "Descriptor candidate did not exercise the existing transient binding-owner bound");
        if(batched)result.descriptors=report.at("draw_descriptors");
        if(enabled) {
            result.contents=report.at("draw_descriptor_content_cache");
            Require(result.contents.at("enabled")==batched&&
                result.contents.at("metadata_bytes").get<uint64_t>()<=
                uint64_t(capacity)*sizeof(NativeDescriptorContentCache),
                "Descriptor contents changed the fixed DrawSlot metadata bound");
            if(batched) {
                for(unsigned set=0;set<3;++set)Require(result.contents.at("skipped_images_by_set").at(set).get<uint64_t>()>0,
                    "Descriptor candidate was vacuous for one actively sampled image dimension");
                Require(result.contents.at("skipped_samplers").get<uint64_t>()>0&&
                    result.contents.at("aborts")==1&&result.contents.at("generation_refusals")==0&&
                    result.contents.at("invalid_handle_refusals")==0,
                    "Descriptor candidate lost successful sampler reuse or original failure accounting");
            }else Require(result.contents.at("draws")==0&&result.contents.at("metadata_bytes")==0&&
                result.contents.at("skipped_images")==0&&result.contents.at("skipped_samplers")==0,
                "Descriptor cache changed the nonbatched ordinary Native path");
        }else Require(!report.contains("draw_descriptor_content_cache"),"Descriptor cache default/OFF created telemetry or metadata");
        result.edram=ReadEdram(device,queue,scene);
        Require(result.edram==seed,"Descriptor sampling changed unrelated packed10MiB EDRAM guard bytes");
        draw(Json::array({alias(19),green,blue}));
        Require(scene.HasPendingGpuWork(),"Descriptor content shutdown released its actual transient tail too early");
        return result;
    };
    for(bool batched:{false,true}) {
        const auto ordinary=run(false,batched),cached=run(true,batched);
        Require(ordinary.edram==cached.edram&&ordinary.queries==cached.queries,
            "Descriptor metadata reuse changed original image/sampler/query/packed EDRAM output");
        if(batched)Require(cached.descriptors.at("writes").get<uint64_t>()<ordinary.descriptors.at("writes").get<uint64_t>()&&
            cached.descriptors.at("update_calls").get<uint64_t>()<ordinary.descriptors.at("update_calls").get<uint64_t>(),
            "Descriptor content cache did not reduce real physical descriptor writes and updates");
    }
    std::cout<<"PASS: actual descriptor content OFF/ON and nonbatched fallback; retired64/128 DrawSlot rings, 2D31/3D17/cube11, samplers/transient owners, same-address history, partial-write failure, exact real queries and full10MiB guards\n";
}
