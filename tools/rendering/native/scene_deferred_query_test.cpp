#include "renderer/native/scene_executor.h"
#include "renderer/native/attachmentless_fsi.h"
#include "renderer/native/gpu_work_queue.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/live_draw_json.h"
#include "fullscreen_spv.h"
#include "retired_vertex_test_spv.h"
#include "raw_float_scene_test.h"
#include "multi_draw_scene_test.h"
#include "query_test_spv.h"
#include "alias_query_test_spv.h"
#include "alias_pair_query_test_spv.h"
#include "fsi_extent_test_spv.h"
#include "fsi_barrier_test_spv.h"
#include "descriptor_batch_test_spv.h"
#include "descriptor_content_scene_test.h"
#include "renderer/native/texture_data.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>
using namespace plume;
using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F&& f,const char* message) {
    bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,message);
}
template<class T,size_t N>std::vector<uint8_t> ShaderBytes(const T(&data)[N]) {
    std::vector<uint8_t> bytes(sizeof(data));std::memcpy(bytes.data(),data,sizeof(data));return bytes;
}
// A later query deliberately remains in an unsubmitted command list. This is
// stronger than hoping the GPU is slow: no fence can accidentally cover that
// tail, so publication of the earlier prefix must preserve its actual owners.
// Paired real GPU execution keeps the exact same draw/query schedule. Only
// inline diagnostic timestamps differ; queue counts and output bytes must match.
void CheckQueuedCpuWrites(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig base,Json draw,const std::vector<uint8_t>& state,const std::string& alias_ps_id) {
    Require(!base.queue_cpu_memory_writes,"Queued CPU writes changed their default");
    base.defer_native_queries=base.batch_transfers=base.unified_gpu_queue=true;
    base.max_draw_batch_size=16;base.async_gpu_submissions=true;
    const std::string state_id=draw.at("device_state");
    const std::string texture_id(64,'a'),report_id(64,'b'),copy_id(64,'c');
    const std::array<uint32_t,6> fetch{2u|(1u<<22),0x5000u|38u,0,0,0,1u<<9};
    const Json identity{{"base","00005000"},{"format",38},{"width",1},{"height",1}};
    auto run=[&](bool enabled,bool prefixes) {
        auto config=base;config.queue_cpu_memory_writes=enabled;config.retire_completed_query_prefixes=prefixes;
        std::unordered_map<std::string,OwnedPayload::Snapshot> bank;
        bank[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
        config.payload={};config.payload_snapshot=[&](const std::string& key) {
            const auto found=bank.find(key);Require(found!=bank.end(),"Missing CPU-write fixture payload");return found->second;
        };
        SceneExecutor scene(config,&device,&queue);uint64_t event=0;
        scene.InitializeEdram(std::vector<uint8_t>(2048*5120));
        scene.InitializeMemory(0x5000,std::vector<uint8_t>(4096));
        // Warm the dummy framebuffer so this test observes only CPU-write fences.
        draw["event"]=++event;scene.Execute(draw);scene.FlushGpuWork();
        auto stats=[&]{return scene.report().at("gpu_work_queue");};
        auto update=[&](uint32_t address,const std::string& key,uint32_t count=4096) {
            scene.Execute({{"type","cpu_memory_write"},{"frame",7},{"event",++event},
                {"address",address},{"bytes",count},{"payload",key},{"coherence_epoch",event}});
        };
        auto sample=[&] {
            draw["event"]=++event;draw["pixel_shader"]=alias_ps_id;
            draw["textures"]=Json::array({{{"slot",0},{"source","gpu_alias_or_previous_frame"},
                {"identity",identity},{"fetch",fetch}}});scene.Execute(draw);
        };
        auto begin=[&](uint64_t generation,uint32_t report=0x2000) {
            scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",report+32}});
        };
        auto end=[&](uint64_t generation,uint32_t report=0x2000) {
            scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",report}});
        };
        for(unsigned phase=0;phase<3;++phase) {
            std::vector<uint8_t> bytes(4096,uint8_t(0x31+phase));
            const std::array<float,4> pixel{phase==1?0.f:1.f,0,0,1};std::memcpy(bytes.data(),pixel.data(),16);
            auto owner=std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
            std::weak_ptr<const std::vector<uint8_t>> weak=owner;bank[texture_id]=owner;
            const auto before=stats().at("waits").get<uint64_t>();update(0x5000,texture_id);
            bank.erase(texture_id);owner.reset();
            Require(weak.expired(),"CPU update retained a caller snapshot instead of its immutable upload");
            if(enabled)Require(scene.HasPendingGpuWork()&&stats().at("waits")==before,
                "Disjoint CPU update failed to share the ordered FIFO");
            begin(700+phase,0x2000+phase*64);sample();
            if(phase==1) {
                // Change the same address inside an active query. Its earlier
                // discarded black sample and later red sample must total256.
                std::vector<uint8_t> red(4096,0x77);const std::array<float,4> pixel{1,0,0,1};
                std::memcpy(red.data(),pixel.data(),16);
                bank[texture_id]=std::make_shared<const std::vector<uint8_t>>(std::move(red));
                const auto before_active=stats().at("waits").get<uint64_t>();
                update(0x5000,texture_id);bank.erase(texture_id);
                if(enabled)Require(scene.HasPendingGpuWork()&&stats().at("waits")==before_active,
                    "Disjoint update inside an active query inserted an eager fence");
                sample();
            }
            end(700+phase,0x2000+phase*64);
        }
        scene.FlushGpuWork();const auto queries=scene.TakeCompletedQueries();
        Require(queries.size()==3,"Queued CPU updates lost exact query generations");
        std::array<uint64_t,3> samples{};
        for(size_t i=0;i<samples.size();++i) {
            Require(queries[i].generation==700+i&&queries[i].result.draws==(i==1?2u:1u),
                "Queued CPU update changed query generation/draw identity");
            samples[i]=queries[i].result.samples;
        }
        Require(samples==std::array<uint64_t,3>{256,256,256},
            "Same-address CPU update/alias replacement changed actual GPU visibility");
        // Isolate a zero-draw query behind a new CPU upload. Older deferred
        // query records cannot mask an incorrect transfer completion frontier.
        std::vector<uint8_t> final_bytes(4096,0x33);const std::array<float,4> final_pixel{1,0,0,1};
        std::memcpy(final_bytes.data(),final_pixel.data(),16);
        bank[texture_id]=std::make_shared<const std::vector<uint8_t>>(std::move(final_bytes));
        update(0x5000,texture_id);bank.erase(texture_id);
        begin(703,0x20c0);end(703,0x20c0);
        if(enabled) {
            if(prefixes)Require(scene.TakeCompletedQueries().empty(),"Zero-draw result escaped its upload frontier");
            else Reject([&]{scene.TakeCompletedQueries();},"CPU update/zero-draw query escaped its actual covering fence");
        }
        scene.FlushGpuWork();const auto zero_query=scene.TakeCompletedQueries();
        Require(zero_query.size()==1&&zero_query[0].generation==703&&
            zero_query[0].result.draws==0&&zero_query[0].result.samples==0,
            "Zero-draw end lost its exact ordered completion result");
        const auto physical=scene.ReadPhysical(0x5000,4096);
        const auto writes=scene.report().at("cpu_memory_writes");
        Require(writes.at("queued_uploads")==uint64_t(enabled?5:0)&&
            writes.at("queued_invalidations")==uint64_t(enabled?5:0),
            "Scene CPU-write counters missed queued transfer ownership");
        // A report-page update must eagerly complete the old query first.
        // Runtime additionally publishes it to guest memory before the update;
        // the pure capability fixture checks that publication order explicitly.
        begin(704);sample();end(704);
        bank[report_id]=std::make_shared<const std::vector<uint8_t>>(4096,0xcc);
        const auto before_report=stats().at("waits").get<uint64_t>();update(0x2000,report_id);
        Require(!scene.HasPendingGpuWork()&&stats().at("waits").get<uint64_t>()>before_report,
            "Query-report alias removed the eager completion guard");
        const auto old=scene.TakeCompletedQueries();
        Require(old.size()==1&&old[0].generation==704&&old[0].result.samples==256,
            "CPU report initialization lost an older actual query result");
        Require(scene.ReadPhysical(0x2000,4096)==std::vector<uint8_t>(4096,0xcc),
            "CPU report initialization changed GPU-history bytes");
        sample();const auto before_null=stats().at("waits").get<uint64_t>();
        scene.Execute({{"type","cpu_memory_write"},{"frame",7},{"event",++event},
            {"address",0x5000},{"bytes",4},{"payload",nullptr}});
        if(enabled)Require(scene.HasPendingGpuWork()&&stats().at("waits")==before_null,
            "Null CPU update unnecessarily completed disjoint GPU work");
        Reject([&]{scene.ReadPhysical(0x5000,4);},"Freed/null CPU update kept readable GPU ownership");
        Require(!scene.HasPendingGpuWork(),"Undefined readback released no accepted owners");
        scene.InitializeMemory(0x5000,physical); // public initialization remains synchronous
        sample();
        bank[copy_id]=std::make_shared<const std::vector<uint8_t>>(physical.begin(),physical.begin()+16);
        scene.Execute({{"type","memory_copy"},{"frame",7},{"event",++event},
            {"source",0x5000},{"destination",0x9000},{"bytes",16},{"source_kind","cpu_snapshot"},{"payload",copy_id}});
        Require(!scene.HasPendingGpuWork()&&scene.ReadPhysical(0x9000,16)==
            std::vector<uint8_t>(physical.begin(),physical.begin()+16),
            "Queued CPU flag changed synchronous memory_copy behavior");
        scene.InitializeMemory(0xa000,std::vector<uint8_t>(16,0x71));
        scene.Execute({{"type","cpu_memory_write"},{"frame",7},{"event",++event},
            {"address",0xa003},{"bytes",5},{"payload",nullptr}});
        bank[copy_id]=std::make_shared<const std::vector<uint8_t>>(16,0x29);
        scene.Execute({{"type","memory_copy"},{"frame",7},{"event",++event},
            {"source",0xa000},{"destination",0xb000},{"bytes",16},{"source_kind","mixed_memory"},
            {"payload",copy_id},{"gpu_ranges",{{0xa000,0xa003},{0xa008,0xa010}}}});
        std::vector<uint8_t> mixed_expected(16,0x71);
        std::fill(mixed_expected.begin()+3,mixed_expected.begin()+8,0x29);
        Require(!scene.HasPendingGpuWork()&&scene.ReadPhysical(0xb000,16)==mixed_expected,
            "Scene mixed copy lost GPU fragments or immutable CPU holes");
        Reject([&]{scene.ReadPhysical(0xa000,16);},"Mixed copy fabricated source ownership");
        // Accepted prior work and the CPU ownership invalidation survive a
        // missing later payload. Runtime sees this changed mutation serial and
        // escalates the transaction rather than replaying it through Xenia.
        sample();const auto serial=scene.mutation_serial();
        Reject([&]{update(0x5000,texture_id);},"Missing immutable CPU-update payload was accepted");
        Require(scene.mutation_serial()!=serial&&!scene.HasPendingGpuWork(),
            "CPU-update failure abandoned accepted work or hid authoritative invalidation");
        return std::pair{physical,samples};
    };
    for(bool prefixes:{false,true})Require(run(false,prefixes)==run(true,prefixes),
        "Paired queued/eager Scene bytes/query results differ");
    std::cout<<"PASS: queued/eager CPU page writes, exact GPU alias visibility/history, report-overlap guard, null ownership, synchronous memcpy/readback and failure drain\n";
}
// Paired real Scene draws consume the vertex stream through BDA. Reusing a
// current generation would change its actual occlusion count and packed EDRAM.
// All presents deliberately use the same guest frame ID; only the existing
// full-fence cache epoch is allowed to return an inactive physical allocation.
void CheckRetiredVertexBuffers(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig base,Json draw,const std::vector<uint8_t>& state) {
    Require(!base.recycle_vertex_buffers,"Retired vertex recycling changed its default");
    base.defer_native_queries=base.batch_transfers=base.unified_gpu_queue=true;
    base.max_draw_batch_size=16;base.async_gpu_submissions=true;
    base.max_dormant_vertex_bytes=base.max_dormant_vertex_entries=0;
    const std::string state_id=draw.at("device_state"),vs_id(64,'f'),ps_id(64,'9');
    const std::array<std::string,5> ids{std::string(64,'a'),std::string(64,'b'),
        std::string(64,'c'),std::string(64,'d'),std::string(64,'e')};
    auto run=[&](bool enabled,bool prefixes) {
        auto config=base;config.recycle_vertex_buffers=enabled;
        config.retire_completed_query_prefixes=prefixes;
        std::unordered_map<std::string,OwnedPayload::Snapshot> bank;
        bank[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
        for(unsigned i=0;i<ids.size();++i) {
            const float left=(i==1||i==3)?-9.f:-1.f;
            const float z=float(i)/8.f;
            const std::array<float,12> positions{left,-1,z,1,left+4,-1,z,1,left,3,z,1};
            std::vector<uint8_t> bytes(sizeof(positions));std::memcpy(bytes.data(),positions.data(),bytes.size());
            bank[ids[i]]=std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        }
        config.payload={};config.shader_snapshot={};config.observe_pipeline={};
        config.payload_snapshot=[&](const std::string& key) {
            const auto found=bank.find(key);Require(found!=bank.end(),"Missing vertex fixture payload");return found->second;
        };
        config.shader=[&](const std::string& key) {
            Require(key==vs_id,"Unexpected vertex fixture shader");return ShaderBytes(retired_vertex_test_spv);
        };
        config.fsi_shader=[&](const std::string& key) {
            Require(key==ps_id,"Unexpected vertex fixture pixel shader");return ShaderBytes(fsi_barrier_test_spv);
        };
        SceneExecutor scene(config,&device,&queue);uint64_t event=0;
        scene.InitializeEdram(std::vector<uint8_t>(2048*5120));
        scene.InitializeMemory(0x1000,std::vector<uint8_t>(16));
        const std::array<uint32_t,6> front{2u|(1u<<22),0x1000u|38u,0,0,0,1u<<9};
        auto present=[&] {
            scene.Execute({{"type","present"},{"frame",7},{"event",++event},
                {"frontbuffer",{{"base","00001000"},{"format",38},{"width",1},{"height",1}}},
                {"frontbuffer_fetch",front},{"provenance","retired vertex fixture"}});
        };
        auto sample_key=[&](const std::string& key) {
            auto op=draw;op["event"]=++event;op["vertex_shader"]=vs_id;op["pixel_shader"]=ps_id;
            op["vertex_slots"]=Json::array({0});op["textures"]=Json::array();
            op["geometry"]=Json::array({{{"kind",14},{"fetch_slot",0},{"endian",0},
                {"address","00007000"},{"bytes",48},{"sha256",key}}});scene.Execute(op);
        };
        auto sample=[&](unsigned i){sample_key(ids.at(i));};
        auto begin=[&](uint64_t generation) {
            scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2020}});
        };
        auto end=[&](uint64_t generation) {
            scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2000}});
        };
        auto stats=[&]{return scene.report().at("vertex_buffer_pool");};
        auto require_counts=[&](uint64_t allocations,uint64_t reuses,uint64_t returns,uint64_t idle) {
            const auto report=stats();Require(report.at("enabled")==enabled&&
                report.at("actual_allocations")==allocations&&report.at("actual_reuses")==reuses&&
                report.at("returns")==returns&&report.at("idle_buffers")==idle&&
                report.at("idle_allocation_bytes").get<uint64_t>()<=16ull*1024*1024&&
                report.at("maximum_idle_buffers")==128&&report.at("maximum_idle_allocation_bytes")==16ull*1024*1024,
                "Retired vertex physical allocation/reuse/fence accounting differs");
        };
        std::array<uint64_t,5> samples{};
        auto result=[&](unsigned phase,unsigned draws,uint64_t expected) {
            const auto completed=scene.TakeCompletedQueries();Require(completed.size()==1&&
                completed[0].generation==1200+phase&&completed[0].frame==7&&
                completed[0].result.draws==draws&&completed[0].result.samples==expected,
                "Retired vertex geometry changed exact actual query visibility");
            samples.at(phase)=completed[0].result.samples;
        };
        begin(1200);sample(0);sample(1);end(1200);
        Require(scene.HasPendingGpuWork(),"Geometry owners completed before the accepted tail");
        require_counts(2,0,0,0);present();result(0,2,256);require_counts(2,0,0,0);
        // B is absent from this fully fenced frame and may now become idle.
        begin(1201);sample(0);end(1201);present();result(1,1,256);
        require_counts(2,0,enabled?1:0,enabled?1:0);
        // C refills B's retired allocation, while the later offscreen D must
        // allocate independently. C is still referenced by an unsubmitted draw.
        begin(1202);sample(2);
        std::weak_ptr<const std::vector<uint8_t>> cpu_owner=bank.at(ids[3]);
        sample(3);bank.erase(ids[3]);Require(cpu_owner.expired(),"Vertex upload retained a borrowed CPU snapshot");
        end(1202);Require(scene.HasPendingGpuWork(),"Vertex replacement unexpectedly fenced the pending tail");
        require_counts(enabled?3:4,enabled?1:0,enabled?1:0,0);
        present();result(2,2,256);require_counts(enabled?3:4,enabled?1:0,enabled?2:0,enabled?1:0);
        // E can take retired A. Revisit still-current C afterwards: its original
        // BDA bytes must survive the different immutable E generation unchanged.
        begin(1203);sample(4);sample(2);end(1203);
        present();result(3,2,512);require_counts(enabled?3:5,enabled?2:0,enabled?3:0,enabled?1:0);
        Require(scene.report().at("vertex_cache").at("creations")==5,
            "Physical reuse changed logical immutable vertex cache creations");
        begin(1204);sample(2);
        const auto before_failure=stats();
        Reject([&]{sample_key(std::string(64,'0'));},"Missing later vertex payload was accepted");
        Require(scene.HasPendingGpuWork()&&stats()==before_failure,
            "Failed vertex preparation released an accepted owner or reused an unproven generation");
        end(1204);scene.FlushGpuWork();result(4,1,256);
        require_counts(enabled?3:5,enabled?2:0,enabled?3:0,enabled?1:0);
        // A partial drain never returns cache owners to this pool. Full EDRAM
        // readback proves every actual covered pixel and every untouched word.
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
        const auto mapped=readback->map();Require(mapped!=nullptr,"Vertex fixture readback mapping failed");
        auto& buffer=static_cast<VulkanBuffer&>(*readback);
        Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Vertex fixture readback invalidation failed");
        std::vector<uint8_t> packed(2048*5120);std::memcpy(packed.data(),mapped,packed.size());readback->unmap();
        for(size_t at=0;at<packed.size();at+=4) {
            uint32_t actual;std::memcpy(&actual,packed.data()+at,4);
            Require(actual==(at<256*4?6u:0u),"Retired vertex reuse changed actual packed EDRAM bytes");
        }
        // Leave an actual accepted BDA-consuming tail at destruction. It must
        // retain its cached C owner until Scene's normal destructor fence.
        sample(2);Require(scene.HasPendingGpuWork(),"Retired vertex shutdown tail fenced prematurely");
        require_counts(enabled?3:5,enabled?2:0,enabled?3:0,enabled?1:0);
        return std::pair{std::move(packed),samples};
    };
    for(bool prefixes:{false,true})Require(run(false,prefixes)==run(true,prefixes),
        "Paired retired vertex buffers changed real GPU geometry/query/EDRAM output");
    std::cout<<"PASS: fenced exact vertex allocation reuse, real BDA geometry replacement, active tail/failure isolation, independent logical generations, exact queries and full10MiB EDRAM parity\n";
}

void CheckDrawTimestamps(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig config,Json draw) {
    Require(!config.gpu_draw_timestamps,"Draw timestamps changed their default");
    config.defer_native_queries=config.batch_transfers=config.unified_gpu_queue=true;
    config.async_gpu_submissions=true;config.max_draw_batch_size=32;
    const auto run=[&](bool enabled) {
        config.gpu_draw_timestamps=enabled;
        Require(!config.gpu_batch_attribution&&!config.gpu_timestamps,
            "Draw timestamps accidentally depend on batch timestamps");
        SceneExecutor scene(config,&device,&queue);uint64_t event=0;
        scene.InitializeEdram(std::vector<uint8_t>(2048*5120));
        const uint32_t capacity=config.async_submission_capacity;
        const std::array<uint32_t,3> frames{16,17,32};
        const std::array<uint32_t,3> draws{capacity+5,3,7};
        Json counts=Json::array();
        for(size_t phase=0;phase<frames.size();++phase) {
            scene.Execute({{"type","query_begin"},{"frame",frames[phase]},{"event",++event},
                {"query_generation",1000+phase},{"query_handle",0x1000},{"report_address",0x2020}});
            for(uint32_t i=0;i<draws[phase];++i) {
                draw["frame"]=frames[phase];draw["event"]=++event;scene.Execute(draw);
            }
            scene.Execute({{"type","query_end"},{"frame",frames[phase]},{"event",++event},
                {"query_generation",1000+phase},{"query_handle",0x1000},{"report_address",0x2000}});
            if(phase==0&&enabled) {
                const auto pending=scene.GpuDrawTimingReport();
                if(pending.value("supported",false))Require(
                    pending.at("accepted")==draws[0]&&pending.at("completed").get<uint64_t>()<draws[0]&&
                    pending.at("pending")==true,"Timestamp pairs escaped their unsubmitted tail");
            }
            scene.FlushGpuWork();const auto completed=scene.TakeCompletedQueries();
            Require(completed.size()==1&&completed[0].generation==1000+phase&&
                completed[0].frame==frames[phase]&&completed[0].result.draws==draws[phase]&&
                completed[0].result.samples==uint64_t(draws[phase])*256,
                "Draw timestamps changed exact fenced query counts or identity");
            counts.push_back(completed[0].result.samples);
        }
        const auto timing=scene.GpuDrawTimingReport(true);
        if(enabled)std::cout<<"Draw timing fixture: "<<scene.GpuDrawTimingReport(false).dump()<<'\n';
        if(enabled&&timing.value("supported",false)) {
            Require(timing.at("disabled")==false&&timing.at("pending")==false&&
                timing.at("accepted")==capacity+12&&timing.at("completed")==capacity+12,
                "Sampled draw timing count/reuse/retirement differs");
            Require(timing.at("available")==capacity+12&&timing.at("unavailable")==0,
                "Fenced GPU timestamp pairs are unavailable");
            Require(timing.at("pair_count")==1&&timing.at("retained_frames")==2&&
                timing.at("frames")[0].at("frame")==16&&timing.at("frames")[1].at("frame")==32,
                "Independent pair registry or frame sampling differs");
        } else if(!enabled)Require(timing==Json{{"enabled",false}},"Disabled profiler allocated diagnostics");
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
        const VkBufferCopy region{0,0,2048*5120};
        vkCmdCopyBuffer(command,exported.edram.buffer,static_cast<VulkanBuffer&>(*readback).vk,1,&region);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&host,0,nullptr,0,nullptr);
        commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
        const auto data=readback->map();auto& buffer=static_cast<VulkanBuffer&>(*readback);
        Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Cannot invalidate timing fixture readback");
        std::vector<uint8_t> bytes(2048*5120);std::memcpy(bytes.data(),data,bytes.size());readback->unmap();
        const auto stats=scene.report().at("gpu_work_queue");
        return std::tuple{counts,bytes,stats};
    };
    const auto off=run(false),on=run(true);
    Require(std::get<0>(off)==std::get<0>(on)&&std::get<1>(off)==std::get<1>(on)&&
        std::get<2>(off)==std::get<2>(on),"Timestamps changed GPU output or queue submission/fence schedule");
}
// Use the existing actual Scene query draw to compare only the fence window.
// Every physical query/DrawSlot ring wraps more than twice; closed, empty and
// active logical queries retain exact identities across four generations.
void CheckFourFenceScene(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig base,Json draw) {
    const uint32_t capacity=base.async_submission_capacity;
    struct Result {Json queries;std::vector<uint8_t> packed;uint64_t waits=0;};
    const auto run=[&](uint32_t fences,bool prefix) {
        auto config=base;config.defer_native_queries=config.batch_transfers=config.unified_gpu_queue=true;
        config.async_gpu_submissions=true;config.max_draw_batch_size=16;
        config.max_inflight_batches=fences;config.gpu_timestamps=true;
        config.gpu_batch_attribution=false;config.gpu_draw_timestamps=false;
        config.retire_completed_query_prefixes=prefix;
        SceneExecutor scene(config,&device,&queue);
        scene.InitializeEdram(std::vector<uint8_t>(2048*5120));
        uint64_t event=0;Json expected=Json::array();std::vector<CompletedNativeQuery> completed;
        const auto begin=[&](uint64_t generation) {
            scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2020}});
        };
        const auto end=[&](uint64_t generation,uint64_t draws) {
            const auto ended=++event;
            scene.Execute({{"type","query_end"},{"frame",7},{"event",ended},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2000}});
            expected.push_back({generation,ended,draws*256,draws});
        };
        const auto record=[&] {draw["frame"]=7;draw["event"]=++event;scene.Execute(draw);};
        const auto take=[&] {
            auto ready=scene.TakeCompletedQueries();completed.insert(completed.end(),ready.begin(),ready.end());
        };
        for(uint32_t group=0;group<capacity/8+2;++group) {
            begin(1000+group);for(uint32_t i=0;i<16;++i)record();end(1000+group,16);
            if(group==1) {begin(5000);end(5000,0);}
            if(group==2) {
                const auto& stats=scene.report().at("gpu_work_queue");
                Require(stats.at("submissions")==3&&stats.at("inflight_batch_limit")==fences,
                    "Fence window changed16draw submission target");
                Require(stats.at("waits")==uint64_t(fences==2)&&
                    stats.at("maximum_inflight_batches")==std::min(fences,3u),
                    "Three independently owned batches did not respect selected fence window");
                if(!prefix)Reject([&]{scene.TakeCompletedQueries();},
                    "Four-fence legacy query publication escaped full completion boundary");
            }
            if(prefix)take();
        }
        // An active query may include both retired and still-owned physical slots.
        begin(6000);for(unsigned i=0;i<5;++i)record();
        const auto active=scene.ActiveQueryMetadata();
        Require(active&&active->draws==5&&active->pending_draws>0,
            "Four-fence physical slot reuse lost active query ownership");
        end(6000,5);Require(scene.HasPendingGpuWork(),"Four-fence fixture lost its final accepted tail");
        scene.FlushGpuWork();take();
        Require(!scene.HasPendingGpuWork()&&!scene.DeferredQueryCount(),
            "Four-fence final completion left GPU/query owners pending");
        Json observed=Json::array();
        for(const auto& q:completed) {
            Require(q.frame==7&&q.result.handle==0x1000&&q.result.begin_address==0x2020&&q.result.end_address==0x2000,
                "Four-fence query publication changed typed metadata");
            observed.push_back({q.generation,q.end_event,q.result.samples,q.result.draws});
        }
        Require(observed==expected,"Four-fence wrapped physical queries changed samples or publication order");
        const auto& stats=scene.report().at("gpu_work_queue");
        Require(stats.at("maximum_inflight_batches")==fences&&stats.at("capacity")==capacity&&
            stats.at("async_batch_lists")==capacity/2&&stats.at("maximum_lists").get<uint64_t>()<=capacity&&
            stats.at("maximum_scratch_bytes").get<uint64_t>()<=NativeGpuWorkQueue::kMaxScratchBytes&&
            stats.at("accepted_work")==stats.at("completed_work"),
            "Four-fence schedule widened ownership/staging bounds or retired unrelated work");
        const uint64_t waits=stats.at("waits");
        // All10MiB packed bytes must match the two-fence path, including untouched data.
        const auto state=scene.ExportState();auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(2048*5120));
        auto commands=queue.createCommandList();auto fence= device.createCommandFence();commands->begin();
        const auto command=static_cast<VulkanCommandList&>(*commands).vk;
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.buffer=state.edram.buffer;barrier.size=VK_WHOLE_SIZE;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
        const VkBufferCopy region{0,0,2048*5120};
        vkCmdCopyBuffer(command,state.edram.buffer,static_cast<VulkanBuffer&>(*readback).vk,1,&region);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
        const auto mapped=readback->map();auto& buffer=static_cast<VulkanBuffer&>(*readback);
        Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Four-fence Scene readback invalidation failed");
        std::vector<uint8_t> packed(2048*5120);std::memcpy(packed.data(),mapped,packed.size());readback->unmap();
        // Destructor must fence a new genuinely accepted tail in both windows.
        record();Require(scene.HasPendingGpuWork(),"Four-fence shutdown case completed its tail early");
        return Result{std::move(observed),std::move(packed),waits};
    };
    const auto baseline=run(2,false);
    for(bool prefix:{false,true}) {
        const auto candidate=run(4,prefix);
        Require(candidate.queries==baseline.queries&&candidate.packed==baseline.packed,
            "Four-fence Scene query or full packed-memory output differs");
    }
    std::cout<<"PASS: two/fourfence16draw parity, >2physicalring wraps, exact actual query counts, zero/active identities, full10MiBpacked readback and accepted shutdown tail\n";
}

void CheckCompletedQueryPrefixes(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig config,Json draw) {
    Require(!config.retire_completed_query_prefixes,"Query prefix experiment changed its default");
    config.defer_native_queries=config.batch_transfers=config.unified_gpu_queue=true;
    config.async_gpu_submissions=true;config.max_draw_batch_size=2;
    const auto run=[&](bool prefix,bool poll) {
        config.retire_completed_query_prefixes=prefix;
        SceneExecutor scene(config,&device,&queue);uint64_t event=0;
        std::vector<CompletedNativeQuery> results;
        auto begin=[&](uint64_t generation) {
            scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2020}});
        };
        auto end=[&](uint64_t generation) {
            scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2000}});
        };
        auto record=[&] {draw["event"]=++event;scene.Execute(draw);};
        auto take=[&] {
            auto part=scene.TakeCompletedQueries();results.insert(results.end(),part.begin(),part.end());
            return part.size();
        };
        begin(901);record();end(901); // First actual sample count: 256.
        record();                  // Unqueried draw completes the first batch.
        begin(902);end(902);        // Actual zero, ordered behind that draw.
        begin(903);record();end(903); // A newer queried draw remains unsubmitted.
        Require(scene.HasPendingDraws()&&scene.DeferredQueryCount()==3,
            "Prefix fixture lost pending draw/query identities");
        Require(scene.report().at("gpu_work_queue").at("submissions")==1&&
            scene.report().at("gpu_work_queue").at("accepted_work")==3,
            "Prefix fixture did not isolate an unsubmitted newer draw");
        if(prefix) {
            if(poll) {
                // External witness fence proves the first batch is complete,
                // without changing Scene's bookkeeping or submitting its tail.
                auto commands=queue.createCommandList();auto fence=device.createCommandFence();
                commands->begin();commands->end();queue.executeCommandLists(commands.get(),fence.get());
                queue.waitForCommandFence(fence.get());
                scene.PollCompletedQueries();
                Require(scene.report().at("gpu_work_queue").at("waits")==0&&
                    scene.report().at("gpu_work_queue").at("poll_retirements")==1,
                    "Nonblocking query polling performed a queue wait or lost its completed fence");
            } else scene.FlushOldestQuery();
            Require(take()==2&&scene.DeferredQueryCount()==1&&scene.HasPendingDraws(),
                "Completed prefix waited on or consumed the newer logical query");
            Require(scene.report().at("gpu_work_queue").at("submissions")==1&&
                scene.report().at("gpu_work_queue").at("completed_work")==2&&
                scene.report().at("gpu_work_queue").at("accepted_work")==3,
                "Prefix pressure submitted or retired the unneeded staged tail");
            const auto active=scene.ActiveQueryMetadata();Require(!active,"Ended tail unexpectedly became active");
            Require(scene.TakeCompletedQueries().empty(),"Unsubmitted queried tail exposed samples");
            scene.PollCompletedQueries();
            Require(scene.HasPendingGpuWork()&&scene.TakeCompletedQueries().empty(),
                "Repeated polling submitted or released a newer tail");
        } else Reject([&]{scene.TakeCompletedQueries();},"Legacy default bypassed its full-drain contract");
        // Same-address metadata reuses freed closed records while the physical
        // queried tail is still owned. Its empty end shares the tail watermark.
        begin(904);end(904);
        scene.FlushGpuWork();take();
        Require(!scene.HasPendingGpuWork()&&results.size()==4&&scene.DeferredQueryCount()==0,
            "Final drain lost a prefix identity or pending resources");
        Json oracle=Json::array();
        for(size_t i=0;i<results.size();++i) {
            const auto& result=results[i];const uint64_t draws=(i==0||i==2)?1:0;
            Require(result.frame==7&&result.generation==901+i&&result.result.handle==0x1000&&
                result.result.begin_address==0x2020&&result.result.end_address==0x2000&&
                result.result.samples==draws*256&&result.result.draws==draws,
                "Prefix changed actual GPU samples, typed identity or FIFO order");
            oracle.push_back({result.generation,result.end_event,result.result.samples,result.result.draws});
        }
        // Leave a real accepted tail for destructor fencing in both modes.
        record();Require(scene.HasPendingGpuWork(),"Prefix shutdown fixture fenced early");
        return oracle;
    };
    const auto reference=run(false,false);
    Require(run(true,false)==reference&&run(true,true)==reference,
        "Prefix polling/pressure output differs from full-drain GPU oracle");

    // Transfer-only predecessor has no occlusion slot but must still hold zero.
    // The witness must not advance this query because its transfer is unsubmitted.
    config.retire_completed_query_prefixes=true;
    SceneExecutor transfer(config,&device,&queue);
    const Json target{{"depth",false},{"edram_base",8},{"format",5},{"format_info",5},
        {"surface_info",80},{"width",1},{"height",1}};
    transfer.Execute({{"type","clearrect"},{"frame",7},{"event",1},{"flags",1},
        {"rectangle",{0,0,1,1}},{"color",{1.f,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
        {"surfaces",{{"6",target}}}});
    transfer.Execute({{"type","query_begin"},{"frame",7},{"event",2},
        {"query_generation",910},{"query_handle",0x1000},{"report_address",0x2020}});
    transfer.Execute({{"type","query_end"},{"frame",7},{"event",3},
        {"query_generation",910},{"query_handle",0x1000},{"report_address",0x2000}});
    const auto staged=transfer.ObserveQueryWork(7,910,0x2000);
    Require(staged.stage==SceneExecutor::QueryWorkStage::Staged&&staged.required>staged.submitted&&
        staged.required<=staged.accepted,"Census failed to observe unsubmitted transfer predecessor");
    Require(transfer.ObserveQueryWork(8,910,0x2000).stage==SceneExecutor::QueryWorkStage::Unknown&&
        transfer.ObserveQueryWork(7,911,0x2000).stage==SceneExecutor::QueryWorkStage::Unknown&&
        transfer.ObserveQueryWork(7,910,0x2040).stage==SceneExecutor::QueryWorkStage::Unknown,
        "Census accepted mismatched query identity");
    transfer.PollCompletedQueries();
    Require(transfer.HasPendingGpuWork()&&!transfer.HasPendingDraws()&&transfer.TakeCompletedQueries().empty(),
        "Zero query passed its unsubmitted transfer-only predecessor");
    transfer.FlushOldestQuery();
    Require(transfer.ObserveQueryWork(7,910,0x2000).stage==SceneExecutor::QueryWorkStage::Completed,
        "Census missed actual completed transfer predecessor");
    const auto zero=transfer.TakeCompletedQueries();
    Require(transfer.ObserveQueryWork(7,910,0x2000).stage==SceneExecutor::QueryWorkStage::Unknown,
        "Census resurrected a retired query");
    Require(zero.size()==1&&zero[0].generation==910&&!zero[0].result.samples&&!zero[0].result.draws,
        "Transfer-only query failed to publish exact zero after its actual fence");
    if(config.multi_draw_command_buffers) {
        auto grouped=config;grouped.max_draw_batch_size=16;
        SceneExecutor scene(grouped,&device,&queue);uint64_t event=0;
        const auto begin=[&](uint64_t g) {scene.Execute({{"type","query_begin"},{"frame",17},{"event",++event},
            {"query_generation",g},{"query_handle",0x3000},{"report_address",0x4020}});};
        const auto end=[&](uint64_t g) {scene.Execute({{"type","query_end"},{"frame",17},{"event",++event},
            {"query_generation",g},{"query_handle",0x3000},{"report_address",0x4000}});};
        const auto record=[&] {auto op=draw;op["frame"]=17;op["event"]=++event;scene.Execute(op);};
        begin(920);record();end(920);
        Require(scene.ObserveQueryWork(17,920,0x4000).stage==SceneExecutor::QueryWorkStage::Staged,
            "Census failed on a closed query in an open physical draw group");
        scene.SubmitGpuWork();
        Require(scene.ObserveQueryWork(17,920,0x4000).stage==SceneExecutor::QueryWorkStage::Submitted,
            "Census failed to distinguish submission from actual completion");
        begin(921);record(); // A newer group stays open and unsubmitted.
        auto witness=queue.createCommandList();auto fence=device.createCommandFence();
        witness->begin();witness->end();queue.executeCommandLists(witness.get(),fence.get());
        queue.waitForCommandFence(fence.get());
        const auto before=scene.ObserveAcceptedWork();
        scene.PollCompletedQueries();
        const auto after=scene.ObserveAcceptedWork();
        Require(scene.ObserveQueryWork(17,920,0x4000).stage==SceneExecutor::QueryWorkStage::Completed,
            "Census failed while preserving newer open draw group");
        auto first=scene.TakeCompletedQueries();
        Require(before.available&&after.available&&before.accepted==after.accepted&&
            after.completed==1&&after.accepted==2&&scene.HasPendingDraws(),
            "Query poll submitted or recycled the open newer draw group");
        Require(first.size()==1&&first[0].generation==920&&first[0].result.samples==256,
            "Query poll lost the actual older fenced sample");
        scene.PollCompletedQueries();Require(scene.TakeCompletedQueries().empty(),
            "Repeated poll exposed the unsubmitted active query");
        record();end(921);scene.FlushGpuWork();
        auto second=scene.TakeCompletedQueries();
        Require(second.size()==1&&second[0].generation==921&&second[0].result.samples==512&&
            !scene.HasPendingGpuWork(),"Open query tail lost its real samples or owners");
        Require(scene.report().at("draw_command_groups").at("maximum_group_draws")==2,
            "Nonblocking poll split a newer physical command group");
        std::cout<<"PASS: submitted prefix polling preserves open newer physical group and256/512 real samples\n";
    }
    std::cout<<"PASS: completed query prefix off/on real GPU parity, nonblocking poll, staged-tail pressure, "
        "same-address record reuse, zero transfer predecessor and pending shutdown\n";
}

// A cache-only CP wait invokes SubmitGpuWork at this exact seam. This case
// compares that progress action with the original full completion using real
// clear/resolve/alias draws, repeated physical addresses and CPU readback.
void CheckCacheCoherenceProgress(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig config,Json draw,const std::vector<uint8_t>& source_state,
    const std::string& alias_pixel) {
    config.defer_native_queries=config.batch_transfers=config.unified_gpu_queue=true;
    config.async_gpu_submissions=true;config.max_draw_batch_size=64;
    config.retire_completed_query_prefixes=false;
    const std::string state_id=draw.at("device_state");
    OwnedPayload::Snapshot current_state;
    config.payload={};config.payload_snapshot=[&](const std::string& key) {
        Require(key==state_id&&bool(current_state),"Coherence progress lost immutable state");
        return current_state;
    };
    const Json fixed{{"depth",false},{"edram_base",8},{"format",5},{"format_info",5},
        {"surface_info",80},{"width",1},{"height",1}};
    const std::array<uint32_t,6> fetch{2u|(1u<<22),0x2000u|38u,0,0,0,1u<<9};
    const Json identity{{"base","00002000"},{"format",38},{"width",1},{"height",1}};
    const auto run=[&](bool submit_only,bool attribution) {
        auto measured=config;measured.gpu_timestamps=measured.gpu_batch_attribution=attribution;
        SceneExecutor scene(measured,&device,&queue);uint64_t event=0;
        std::vector<uint8_t> mutable_source=source_state;
        auto progress=[&] {
            if(submit_only)scene.SubmitGpuWork();else scene.FlushGpuWork();
        };
        auto resolve=[&](float red) {
            scene.Execute({{"type","clearrect"},{"frame",7},{"event",++event},{"flags",1},
                {"rectangle",{0,0,1,1}},{"color",{red,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
                {"surfaces",{{"6",fixed}}}});
            scene.Execute({{"type","resolve"},{"frame",7},{"event",++event},{"level",0},{"flags",0},
                {"source_slot",0},{"source_rectangle",{0,0,1,1}},{"destination_rectangle",{0,0,1,1}},
                {"destination",identity},{"destination_fetch",fetch},{"exponent_bias",0},
                {"copy_dest_swap",false},{"surfaces",{{"6",fixed}}}});
        };
        for(unsigned pass=0;pass<3;++pass) {
            resolve(pass==1?0.f:1.f);
            progress(); // Cache visibility after older transfers, before texture sampling.
            if(submit_only&&pass==0)
                Require(scene.report().at("gpu_work_queue").at("waits")==0,
                    "Coherence progress synchronously waited its first submitted transfer");
            scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",920+pass},{"query_handle",0x1000},{"report_address",0x2020}});
            mutable_source=source_state;
            current_state=std::make_shared<const std::vector<uint8_t>>(mutable_source);
            std::weak_ptr<const std::vector<uint8_t>> old_state=current_state;
            draw["event"]=++event;draw["pixel_shader"]=alias_pixel;
            draw["textures"]=Json::array({{{"slot",0},{"source","gpu_alias_or_previous_frame"},
                {"identity",identity},{"fetch",fetch}}});
            scene.Execute(draw);
            current_state.reset();std::fill(mutable_source.begin(),mutable_source.end(),0xcd);
            Require(old_state.expired(),"Draw retained a borrowed CPU state instead of its upload");
            scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},
                {"query_generation",920+pass},{"query_handle",0x1000},{"report_address",0x2000}});
            progress(); // The old alias image must survive any still-pending draw.
        }
        // This zero-draw query is ordered behind a later transfer-only write.
        resolve(.5f);
        scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
            {"query_generation",923},{"query_handle",0x1000},{"report_address",0x2020}});
        scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},
            {"query_generation",923},{"query_handle",0x1000},{"report_address",0x2000}});
        progress();
        // Explicit readback still performs the full completion boundary and
        // must observe the newest resolve, not a retained earlier alias image.
        const auto bytes=scene.ReadPhysical(0x2000,16);
        const auto results=scene.TakeCompletedQueries();
        Require(!scene.HasPendingGpuWork()&&results.size()==4,
            "Coherence progress/readback lost accepted work or deferred results");
        Json oracle=Json::array();
        for(unsigned i=0;i<4;++i) {
            const uint64_t samples=i==0||i==2?256:0;
            Require(results[i].generation==920+i&&results[i].result.samples==samples&&
                results[i].result.draws==uint64_t(i<3),
                "Coherence progress changed exact alias visibility or zero-query ordering");
            oracle.push_back({results[i].generation,results[i].end_event,
                results[i].result.samples,results[i].result.draws});
        }
        std::array<float,4> color{};std::memcpy(color.data(),bytes.data(),16);
        const float expected=std::round(.5f*(32767.f/32.f))/32767.f*32.f;
        Require(std::abs(color[0]-expected)<.00001f&&color[1]==0&&color[2]==0,
            "Coherence readback did not publish the latest exact transfer bytes");
        if(attribution) {
            const auto profile=scene.GpuBatchAttributionReport(true);
            Require(profile.at("enabled")==true&&profile.at("batches")==scene.report().at("gpu_work_queue").at("submissions"),
                "GPU attribution omitted or invented a retired submission");
            uint64_t draws=0,queried_draws=0;bool clear=false,resolve_kind=false,alias=false;
            for(const auto& batch:profile.at("recent_retired_batches")) {
                draws+=batch.at("draw_lists").get<uint64_t>();
                for(const auto& component:batch.at("composition")) {
                    const std::string kind=component.at("kind");
                    clear|=kind=="clear";resolve_kind|=kind=="resolve";alias|=kind=="alias_load";
                    if(kind=="draw"&&component.at("queried").get<bool>())queried_draws+=component.at("lists").get<uint64_t>();
                }
            }
            Require(draws==3&&queried_draws==3&&clear&&resolve_kind&&alias&&profile.at("shader_pairs").size()==1,
                "GPU attribution lost shader/query/transfer metadata or preparing alias labels");
            Require(profile.at("shader_pairs").at(0).at("pixel_shader")==alias_pixel,
                "GPU attribution did not own the exact shader pair identity");
        } else Require(!scene.report().contains("gpu_batch_attribution"),"Disabled attribution changed report presence");
        // Normal close must retain and fence an accepted transfer-only tail.
        resolve(.25f);scene.SubmitGpuWork();
        return std::pair{oracle,bytes};
    };
    const auto reference=run(false,false);
    Require(reference==run(true,false),"Submit/poll coherence output differs from full completion");
    Require(reference==run(false,true)&&reference==run(true,true),"Batch attribution changed exact alias/query/readback outputs");
    std::cout<<"PASS: cache coherence progress/full-fence alias generations, CPU state reuse, "
        "512 actual samples, zero transfer query, exact physical readback and shutdown\n";
}

// A real interlocked RMW observes every prior draw's packed EDRAM write.
// Separate byte equality and actual query results cover both resource and
// publication ordering; compute and copy phases require the full fallback.
void CheckPreciseFsiBarriers(RenderDevice& device,RenderCommandQueue& queue,SceneExecutorConfig config) {
    constexpr uint32_t edram_bytes=10u*1024*1024,repeats=7,phases=6;
    const std::string state_id(64,'a'),vs_id(64,'b'),ps_id(64,'c');
    std::vector<uint8_t> state(0x2a80);
    auto word=[&](size_t offset,uint32_t value) {
        for(unsigned i=0;i<4;++i)state.at(offset+i)=uint8_t(value>>(24-i*8));
    };
    auto number=[&](size_t offset,float value){word(offset,std::bit_cast<uint32_t>(value));};
    word(0x2880,80);word(0x28bc,16|(16<<16));word(0x28c4,0x80000000);word(0x28c8,16|(16<<16));
    word(0x2944,1<<19);word(0x294c,0x43f);
    for(unsigned offset:{0x2938,0x2958,0x295c,0x2960})word(offset,0x10001);
    number(0x2908,8);number(0x290c,8);number(0x2910,-8);number(0x2914,8);number(0x2918,1);
    OwnedPayload::Snapshot current_state;
    config.payload={};config.shader_snapshot={};config.observe_pipeline={};
    config.payload_snapshot=[&](const std::string& key) {
        Require(key==state_id&&bool(current_state),"Barrier fixture lost immutable state");return current_state;
    };
    config.shader=[&](const std::string& key) {
        Require(key==vs_id,"Unexpected barrier fixture VS");return ShaderBytes(fullscreen_spv);
    };
    config.fsi_shader=[&](const std::string& key) {
        Require(key==ps_id,"Unexpected barrier fixture PS");return ShaderBytes(fsi_barrier_test_spv);
    };
    config.fsi_depth_shader=[] {return ShaderBytes(fsi_barrier_test_spv);};
    config.batch_transfers=config.unified_gpu_queue=config.defer_native_queries=true;
    config.coalesce_draw_barriers=config.skip_readonly_sample_barriers=false;
    config.max_draw_batch_size=config.async_gpu_submissions?2:64;
    auto read=[&](const NativeMemoryState& source) {
        auto buffer=device.createBuffer(RenderBufferDesc::ReadbackBuffer(edram_bytes));
        auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
        const auto command=static_cast<VulkanCommandList&>(*commands).vk;
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.buffer=source.buffer;barrier.size=VK_WHOLE_SIZE;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,1,&barrier,0,nullptr);
        const VkBufferCopy copy{0,0,edram_bytes};
        auto& native=static_cast<VulkanBuffer&>(*buffer);
        vkCmdCopyBuffer(command,source.buffer,native.vk,1,&copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&host,0,nullptr,0,nullptr);
        commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
        const auto* bytes=static_cast<const uint8_t*>(buffer->map());
        Require(vmaInvalidateAllocation(native.device->allocator,native.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Barrier fixture readback invalidate failed");
        std::vector<uint8_t> result(bytes,bytes+edram_bytes);buffer->unmap();return result;
    };
    const bool test_attachmentless=config.attachmentless_fsi;
    auto run=[&](bool precise,bool attachmentless=false) {
        config.precise_fsi_barriers=precise;config.attachmentless_fsi=attachmentless;
        SceneExecutor renderer(config,&device,&queue);
        std::vector<uint8_t> expected(edram_bytes);renderer.InitializeEdram(expected);
        uint64_t event=0;uint32_t accumulated=0;
        for(uint32_t phase=0;phase<phases;++phase) {
            if(phase==1) {
                const Json unused{{"depth",false},{"edram_base",2000},{"format",0},{"format_info",0},
                    {"surface_info",80},{"width",1},{"height",1}};
                renderer.Execute({{"type","clearrect"},{"frame",1},{"event",++event},{"flags",1},
                    {"rectangle",{0,0,1,1}},{"color",{0.f,0.f,0.f,0.f}},{"depth",1.f},{"stencil",0},
                    {"surfaces",{{"6",unused}}}});
            }
            if(phase==2) {
                // Validate before importing the oracle, so the import cannot
                // mask a lost RMW from the first two command sequences.
                Require(read(renderer.ExportState().edram)==expected,
                    "Packed RMW lost a draw before the copy transition");
                renderer.InitializeEdram(expected);
            }
            const bool depth_only=phase>=4;
            const unsigned format=depth_only?phase-4:phase==3?7:phase==2?5:0;
            word(0x28dc,depth_only?0:15);current_state=std::make_shared<const std::vector<uint8_t>>(state);
            const Json surface{{"depth",depth_only},{"edram_base",0},{"format",format},{"format_info",0},
                {"surface_info",80},{"width",16},{"height",16}};
            renderer.Execute({{"type","query_begin"},{"frame",1},{"event",++event},
                {"query_generation",phase+1},{"query_handle",0x1000},{"report_address",0x2020}});
            for(uint32_t repeat=0;repeat<repeats;++repeat)
                renderer.Execute({{"type","draw"},{"frame",1},{"event",++event},{"function","DrawPrimitive"},
                    {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vs_id},
                    {"pixel_shader",depth_only?Json(nullptr):Json(ps_id)},
                    {"surfaces",{{depth_only?"10":"6",surface}}},{"geometry",Json::array()},
                    {"vertex_slots",Json::array()},{"textures",Json::array()}});
            renderer.Execute({{"type","query_end"},{"frame",1},{"event",++event},
                {"query_generation",phase+1},{"query_handle",0x1000},{"report_address",0x2000}});
            accumulated+=repeats;
            for(unsigned pixel=0;pixel<16*16;++pixel)std::memcpy(expected.data()+pixel*4,&accumulated,4);
        }
        Require(renderer.HasPendingGpuWork(),"Barrier fixture drained its last RMW early");
        Reject([&]{renderer.TakeCompletedQueries();},"Barrier fixture published before the full fence");
        const auto actual=read(renderer.ExportState().edram);
        Require(actual==expected,"Precise FSI barriers changed packed RMW or unrelated EDRAM bytes");
        const auto queries=renderer.TakeCompletedQueries();
        Require(queries.size()==phases,"Barrier fixture lost query identities");
        for(uint32_t phase=0;phase<phases;++phase)
            Require(queries[phase].generation==phase+1&&queries[phase].result.draws==repeats&&
                queries[phase].result.samples==16*16*repeats,"Precise FSI barriers changed real query samples");
        if(precise) {
            const auto& stats=renderer.report().at("draw_barriers");
            if(attachmentless) {
                // The initial no-attachment candidate preserves Plume's full
                // tracked EDRAM barrier. The existing precise helper requires
                // an actual dummy image and therefore refuses this shape.
                Require(stats.at("precise_fsi")==0&&
                    stats.at("precise_fallback").get<uint64_t>()>=uint64_t(phases)*repeats,
                    "Attachmentless candidate narrowed the original tracked EDRAM barrier");
            } else Require(stats.at("precise_fsi").get<uint64_t>()>=phases*(repeats-1)&&
                stats.at("precise_fallback").get<uint64_t>()>=3,
                "Precise barrier fixture missed native repeats or compute/copy/initial fallback");
        }
        if(config.discard_dummy_fsi_attachment&&!attachmentless) {
            const auto& dummy=renderer.report().at("dummy_fsi_render_pass");
            Require(dummy.at("installs").get<uint64_t>()>0&&dummy.at("refusals")==0&&
                dummy.at("selected_draws")==uint64_t(phases)*repeats&&
                dummy.at("queried_draws")==uint64_t(phases)*repeats&&
                dummy.at("no_pixel_shader_draws").get<uint64_t>()>0,
                "Dummy pass RMW/query/no-PS coverage is vacuous");
            std::cout<<"DUMMY_FSI_RMW_COVERAGE "<<dummy.dump()<<'\n';
        }
        if(attachmentless) {
            const auto& candidate=renderer.report().at("attachmentless_fsi_render_pass");
            Require(candidate.at("enabled")==true&&candidate.at("supported")==true&&candidate.at("active")==true&&
                candidate.at("refused")==false&&candidate.at("installs").get<uint64_t>()>0&&candidate.at("refusals")==0&&
                candidate.at("selected_draws")==uint64_t(phases)*repeats&&
                candidate.at("queried_draws")==uint64_t(phases)*repeats&&
                candidate.at("no_pixel_shader_draws")==uint64_t(2)*repeats&&
                candidate.at("barrier_draws").get<uint64_t>()>0&&
                renderer.report().at("fsi_raster_attachment").at("creations")==0,
                "Attachmentless RMW material/depth/no-PS/query/barrier coverage is vacuous");
            Require(renderer.report().at("dummy_fsi_render_pass").at("installs")==0,
                "Attachmentless RMW accidentally created a dummy color attachment");
            std::cout<<"ATTACHMENTLESS_FSI_RMW_COVERAGE "<<candidate.dump()<<'\n';
        }
        return actual;
    };
    const auto baseline=run(false);
    Require(baseline==run(true),"Full versus precise FSI barrier packed bytes differ");
    if(test_attachmentless)Require(baseline==run(false,true)&&baseline==run(true,true),
        "Attachmentless full/precise barriers changed actual packed RMW/depth/no-PS outputs");
    std::cout<<"PASS: precise FSI packed interlocked RMW, compute/copy fallback, color/depth variants, full10MiB guards and10752 actual samples per mode\n";
}

void CheckRetainedFsiExtent(RenderDevice& device,RenderCommandQueue& queue,SceneExecutorConfig config) {
    struct Case {
        uint32_t width,height;
        int32_t viewport_x=0,viewport_y=0;
        std::array<int32_t,4> scissor{0,0,64,64};
        bool depth_only=false;
    };
    // Odd framebuffer/scissor boundaries exercise helper invocations at the
    // old renderArea edges. Wide/tall alternation exceeds the32MiB union cap
    // without allocating an oversized attachment in either configuration.
    const std::array cases{
        Case{32,32},Case{17,13},Case{24,19,0,0,{3,5,29,31}},
        Case{40,29,5,7},Case{9,7,0,0,{0,0,64,64},true},
        Case{4096,16},Case{16,4096},Case{15,11},Case{33,25},
        Case{37,35,0,0,{20,20,10,10}},Case{8,9}};
    constexpr size_t edram_bytes=10u*1024*1024;
    constexpr uint32_t repeats=3;
    constexpr uint32_t sentinel=0xcdcdcdcdu;
    const std::string state_id(64,'a'),vs_id(64,'b'),ps_id(64,'c');
    std::vector<uint8_t> state(0x2a80),seed(edram_bytes,0xcd);
    auto word=[&](size_t offset,uint32_t value) {
        for(unsigned i=0;i<4;++i)state.at(offset+i)=uint8_t(value>>(24-i*8));
    };
    auto number=[&](size_t offset,float value){word(offset,std::bit_cast<uint32_t>(value));};
    word(0x2880,80);word(0x28c4,0x80000000);word(0x28c8,64|(64<<16));
    word(0x2944,1<<19);word(0x294c,0x43f);
    for(unsigned offset:{0x2938,0x2958,0x295c,0x2960})word(offset,0x10001);
    number(0x2908,16);number(0x2910,-16);number(0x2918,1);
    OwnedPayload::Snapshot current_state;
    config.payload={};config.shader_snapshot={};config.detailed_reports=false;
    config.payload_snapshot=[&](const std::string& key) {
        Require(key==state_id&&bool(current_state),"FSI extent fixture lost current state");return current_state;
    };
    config.shader=[&](const std::string& key) {
        Require(key==vs_id,"FSI extent fixture used another vertex shader");return ShaderBytes(fullscreen_spv);
    };
    config.fsi_shader=[&](const std::string& key) {
        Require(key==ps_id,"FSI extent fixture used another pixel shader");return ShaderBytes(fsi_extent_test_spv);
    };
    config.fsi_depth_shader=[] {return ShaderBytes(fsi_extent_test_spv);};
    config.batch_transfers=true;config.unified_gpu_queue=true;config.defer_native_queries=true;
    // Under async mode, every third draw follows an actual submitted pair.
    // Shrinks may retain that attachment; growth must drain before replacement.
    config.max_draw_batch_size=config.async_gpu_submissions?2:64;
    struct Result {std::vector<uint8_t> edram;std::vector<uint64_t> samples;uint64_t creations,reuses;};
    const bool test_discard=config.discard_dummy_fsi_attachment,test_attachmentless=config.attachmentless_fsi;
    const auto run=[&](bool retained,bool discard_dummy=false,bool attachmentless=false,
                      uint32_t fail_create=0,bool simulate_unsupported_samples=false) {
        config.retain_fsi_raster_extent=retained;
        config.discard_dummy_fsi_attachment=discard_dummy;config.attachmentless_fsi=attachmentless;
        uint32_t attempted_creates=0;
        config.test_attachmentless_fsi_before_create={};
        if(fail_create)config.test_attachmentless_fsi_before_create=[&] {
            if(++attempted_creates==fail_create)throw std::runtime_error("Injected unaccepted attachmentless framebuffer allocation refusal");
        };
        std::unique_ptr<SceneExecutor> owner;
        if(simulate_unsupported_samples) {
            // Simulate the constructor's capability preflight only. Restore
            // actual physical properties before recording any Vulkan commands.
            auto& counts=static_cast<VulkanDevice&>(device).physicalDeviceProperties.limits.framebufferNoAttachmentsSampleCounts;
            struct Restore {VkSampleCountFlags& counts;VkSampleCountFlags old;~Restore(){counts=old;}} restore{counts,counts};
            counts&=~VkSampleCountFlags(VK_SAMPLE_COUNT_1_BIT);
            owner=std::make_unique<SceneExecutor>(config,&device,&queue);
        } else owner=std::make_unique<SceneExecutor>(config,&device,&queue);
        auto& renderer=*owner;renderer.InitializeEdram(seed);
        uint64_t event=0;
        for(size_t pass=0;pass<cases.size();++pass) {
            const auto& c=cases[pass];word(0x1780,uint32_t(pass));
            word(0x28dc,c.depth_only?0:15);
            word(0x28b8,uint32_t(c.scissor[0])|(uint32_t(c.scissor[1])<<16));
            word(0x28bc,uint32_t(c.scissor[2])|(uint32_t(c.scissor[3])<<16));
            number(0x290c,float(c.viewport_x+16));number(0x2914,float(c.viewport_y+16));
            current_state=std::make_shared<const std::vector<uint8_t>>(state);
            const Json surface{{"depth",c.depth_only},{"edram_base",0},{"format",0},{"format_info",0},
                {"surface_info",80},{"width",c.width},{"height",c.height}};
            renderer.Execute({{"type","query_begin"},{"frame",1},{"event",++event},
                {"query_generation",pass+1},{"query_handle",0x1000},{"report_address",0x2020}});
            for(uint32_t repeat=0;repeat<repeats;++repeat) {
                renderer.Execute({{"type","draw"},{"frame",1},{"event",++event},{"function","DrawPrimitive"},
                    {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vs_id},
                    {"pixel_shader",c.depth_only?Json(nullptr):Json(ps_id)},
                    {"surfaces",{{c.depth_only?"10":"6",surface}}},{"geometry",Json::array()},
                    {"vertex_slots",Json::array()},{"textures",Json::array()}});
            }
            renderer.Execute({{"type","query_end"},{"frame",1},{"event",++event},
                {"query_generation",pass+1},{"query_handle",0x1000},{"report_address",0x2000}});
            current_state.reset();
            if(retained&&pass==6) {
                const auto& dimensions=renderer.report().at("fsi_raster_attachment");
                Require(dimensions.at("width")==16&&dimensions.at("height")==4096,
                    "FSI retained extent exceeded its32MiB union cap instead of exact-size fallback");
            }
        }
        // Full packed storage comparison also checks all bytes outside the
        // viewport/scissor and the helper shader's per-pass diagnostic regions.
        const auto exported=renderer.ExportState();
        Require(exported.edram.capacity==edram_bytes&&
            exported.edram.ranges==NativeStateRanges{{0,uint32_t(edram_bytes)}},
            "FSI extent fixture changed initialized EDRAM ownership");
        const auto queries=renderer.TakeCompletedQueries();
        Require(queries.size()==cases.size(),"FSI extent transitions lost query results");
        Result result;
        for(size_t pass=0;pass<cases.size();++pass) {
            const auto& c=cases[pass];
            const int32_t x0=std::max({0,c.viewport_x,c.scissor[0]}),y0=std::max({0,c.viewport_y,c.scissor[1]});
            const int32_t x1=std::min({int32_t(c.width),c.viewport_x+32,c.scissor[2]}),
                y1=std::min({int32_t(c.height),c.viewport_y+32,c.scissor[3]});
            const auto expected=uint64_t(std::max(0,x1-x0))*uint64_t(std::max(0,y1-y0))*repeats;
            Require(queries[pass].generation==pass+1&&queries[pass].result.samples==expected&&
                queries[pass].result.draws==(expected?repeats:0),
                "FSI framebuffer extent changed actual viewport/scissor query coverage");
            result.samples.push_back(queries[pass].result.samples);
        }
        auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(edram_bytes));
        auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
        const auto command=static_cast<VulkanCommandList&>(*commands).vk;
        VkBufferMemoryBarrier source{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        source.buffer=exported.edram.buffer;source.size=VK_WHOLE_SIZE;
        source.srcQueueFamilyIndex=source.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        source.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;source.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,1,&source,0,nullptr);
        const VkBufferCopy copy{0,0,edram_bytes};
        auto& buffer=static_cast<VulkanBuffer&>(*readback);
        vkCmdCopyBuffer(command,exported.edram.buffer,buffer.vk,1,&copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&host,0,nullptr,0,nullptr);
        commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
        const auto* data=static_cast<const uint8_t*>(readback->map());
        Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "FSI extent readback invalidate failed");
        result.edram.assign(data,data+edram_bytes);readback->unmap();
        const auto& stats=renderer.report().at("fsi_raster_attachment");
        result.creations=stats.at("creations");result.reuses=stats.at("larger_extent_reuses");
        if(discard_dummy&&!attachmentless) {
            const auto& dummy=renderer.report().at("dummy_fsi_render_pass");
            uint64_t expected_selected=0;
            for(const auto& query:queries)expected_selected+=query.result.draws;
            Require(dummy.at("installs").get<uint64_t>()>0&&dummy.at("refusals")==0&&
                dummy.at("selected_draws")==expected_selected&&dummy.at("queried_draws")==expected_selected&&
                dummy.at("no_pixel_shader_draws").get<uint64_t>()>0&&
                dummy.at("discarded_draws").get<uint64_t>()>0&&(!retained||result.reuses>0),
                "Dummy discard extent coverage missed actual pass/no-PS/discard/query/retained cases");
            std::cout<<"DUMMY_FSI_EXTENT_COVERAGE "<<dummy.dump()<<'\n';
        }
        if(attachmentless) {
            const auto& candidate=renderer.report().at("attachmentless_fsi_render_pass");
            const auto& dummy=renderer.report().at("dummy_fsi_render_pass");
            uint64_t expected_selected=0;for(const auto& query:queries)expected_selected+=query.result.draws;
            Require(candidate.at("enabled")==true&&candidate.at("queried_draws")==candidate.at("selected_draws")&&
                candidate.at("selected_draws").get<uint64_t>()<=expected_selected,
                "Attachmentless extent changed accepted/query selection bounds");
            if(!fail_create&&!simulate_unsupported_samples) {
                Require(candidate.at("supported")==true&&candidate.at("active")==true&&candidate.at("refused")==false&&
                    candidate.at("installs").get<uint64_t>()>0&&candidate.at("refusals")==0&&
                    candidate.at("selected_draws")==expected_selected&&candidate.at("no_pixel_shader_draws").get<uint64_t>()>0&&
                    candidate.at("discarded_draws").get<uint64_t>()>0&&candidate.at("barrier_draws").get<uint64_t>()>0&&
                    result.creations==0&&dummy.at("installs")==0&&(!retained||result.reuses>0),
                    "Attachmentless extent resize/no-PS/discard/barrier/retained coverage is vacuous");
            } else {
                Require(candidate.at("active")==false&&candidate.at("refused")==true&&candidate.at("refusals")==1&&
                    result.creations>0&&dummy.at("installs").get<uint64_t>()>0&&
                    candidate.at("selected_draws").get<uint64_t>()+dummy.at("selected_draws").get<uint64_t>()==expected_selected,
                    "Attachmentless preaccept refusal failed to keep exact ordinary Native pass ownership");
                if(fail_create)Require(attempted_creates==fail_create&&candidate.at("attempts")==fail_create&&
                    candidate.at("installs")==fail_create-1&&candidate.at("supported")==true&&
                    (fail_create==1?candidate.at("selected_draws")==0:candidate.at("selected_draws").get<uint64_t>()>0),
                    "Attachmentless1st/2nd failure did not latch after exactly the accepted allocation prefix");
                else Require(candidate.at("supported")==false&&candidate.at("attempts")==0&&candidate.at("installs")==0&&
                    candidate.at("selected_draws")==0,"Missing sample1 support failed constructor refusal before commands");
            }
            std::cout<<"ATTACHMENTLESS_FSI_EXTENT_COVERAGE "<<Json{{"retained",retained},{"failure_create",fail_create},
                {"simulated_unsupported_samples",simulate_unsupported_samples},{"candidate",candidate},{"dummy",dummy},
                {"image_creations",result.creations},{"expected_selected",expected_selected}}.dump()<<'\n';
        }
        // Assert actual shader writes, all scissored bytes and derivatives on
        // both paths. Exact off/on byte equality below remains the parity oracle.
        constexpr std::array<float,4> derivatives{2.f/32,5.f/32,3.f/32,7.f/32};
        for(size_t pass=0;pass<cases.size();++pass)for(int32_t y=0;y<64;++y)for(int32_t x=0;x<64;++x) {
            const auto& c=cases[pass];
            const bool covered=x>=c.viewport_x&&x<c.viewport_x+32&&y>=c.viewport_y&&y<c.viewport_y+32&&
                x<int32_t(c.width)&&y<int32_t(c.height)&&x>=c.scissor[0]&&x<c.scissor[2]&&
                y>=c.scissor[1]&&y<c.scissor[3];
            for(size_t component=0;component<4;++component) {
                const size_t offset=((pass*64*64+size_t(y)*64+size_t(x))*4+component)*4;
                uint32_t bits;std::memcpy(&bits,result.edram.data()+offset,4);
                const auto value=std::bit_cast<float>(bits);
                Require(covered?(std::isfinite(value)&&std::abs(value-derivatives[component])<.00001f):bits==sentinel,
                    "FSI extent changed derivative output or wrote beyond the original viewport/scissor");
            }
        }
        const auto guard=cases.size()*64*64*4*sizeof(uint32_t);
        Require(std::equal(result.edram.begin()+guard,result.edram.end(),seed.begin()+guard),
            "FSI extent fixture modified unrelated packed EDRAM bytes");
        // Destructor must also retire a final draw that still owns this image.
        current_state=std::make_shared<const std::vector<uint8_t>>(state);
        renderer.Execute({{"type","draw"},{"frame",1},{"event",++event},{"function","DrawPrimitive"},
            {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vs_id},{"pixel_shader",ps_id},
            {"surfaces",{{"6",{{"depth",false},{"edram_base",0},{"format",0},{"format_info",0},
                {"surface_info",80},{"width",8},{"height",9}}}}},{"geometry",Json::array()},
            {"vertex_slots",Json::array()},{"textures",Json::array()}});
        Require(renderer.HasPendingGpuWork(),"FSI extent shutdown fixture lost its pending raster work");
        current_state.reset();return result;
    };
    const auto exact=run(false),retained=run(true);
    if(test_discard) {
        const auto exact_discard=run(false,true),retained_discard=run(true,true);
        Require(exact.edram==exact_discard.edram&&exact.samples==exact_discard.samples&&
            retained.edram==retained_discard.edram&&retained.samples==retained_discard.samples,
            "Dummy LOAD/STORE discard changed real full EDRAM/derivatives/query/extent output");
    }
    if(test_attachmentless) {
        const auto exact_empty=run(false,false,true),retained_empty=run(true,false,true);
        Require(exact.edram==exact_empty.edram&&exact.samples==exact_empty.samples&&
            retained.edram==retained_empty.edram&&retained.samples==retained_empty.samples,
            "Attachmentless exact/retained framebuffers changed full EDRAM/helper derivatives/query coverage");
        for(bool keep:{false,true}) {
            const auto& reference=keep?retained:exact;
            for(uint32_t failed:{1u,2u}) {
                const auto fallback=run(keep,true,true,failed);
                Require(reference.edram==fallback.edram&&reference.samples==fallback.samples,
                    "Unaccepted attachmentless allocation failure changed ordinary Native query/full EDRAM output");
            }
            const auto unsupported=run(keep,true,true,0,true);
            Require(reference.edram==unsupported.edram&&reference.samples==unsupported.samples,
                "Simulated attachmentless sample1 refusal changed ordinary Native query/full EDRAM output");
        }
        std::cout<<"PASS: attachmentless original/retained no-color extents, no-PS/discard/empty real queries, derivatives, full10MiB guards,1st/2nd allocation and missing sample1 preflight refusal parity\n";
    }
    Require(exact.edram==retained.edram&&exact.samples==retained.samples,
        "Retained FSI raster extent changed packed EDRAM bits or query samples");
    Require(retained.creations<exact.creations&&retained.reuses>0,
        "FSI retained extent fixture failed to exercise actual framebuffer reuse");
    std::cout<<"PASS: FSI exact/retained extents, eleven shrink/grow/odd/scissor/depth-only/empty/budget cases, "
        <<"actual query counts and10MiB packed derivative/guard parity; attachment creations "
        <<exact.creations<<" -> "<<retained.creations<<'\n';
}
// Sample all three image dimensions and their separate samplers at sparse,
// high slots. The third draw exceeds one fixed write chunk and overwrites the
// same destination repeatedly; its final red image must win. Other fixtures
// exercise actual set4 EDRAM writes, alias retirement and transfer pressure.
void CheckDescriptorBatches(RenderDevice& device,RenderCommandQueue& queue,
                            SceneExecutorConfig base,const std::vector<uint8_t>& state) {
    const std::string state_id(64,'3'),vs_id(64,'1'),ps_id(64,'2');
    std::unordered_map<std::string,OwnedPayload::Snapshot> inputs;
    inputs[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
    const std::array<uint32_t,3> slots{31,17,11};
    std::array<std::array<uint32_t,6>,3> fetches;
    for(unsigned i=0;i<3;++i)fetches[i]={2u|(1u<<22),(0x100000u+i*0x20000u)|38u,
        i==2?5u<<26:0u,(1u<<4)|(2u<<7)|(3u<<10),0,(i+1)<<9};
    auto texture=[&](unsigned which,bool black) {
        const auto layout=DescribeTexture(fetches[which]);
        size_t bytes=0;
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
    base.payload={};base.shader_snapshot={};base.observe_pipeline={};
    base.payload_snapshot=[&](const std::string& key){return inputs.at(key);};
    base.shader=[&](const std::string& key) {
        if(key==vs_id)return ShaderBytes(fullscreen_spv);
        Require(key==ps_id,"Descriptor fixture requested unexpected shader");
        return ShaderBytes(descriptor_batch_test_spv);
    };
    base.fsi_shader=[&](const std::string& key) {
        Require(key==ps_id,"Descriptor fixture requested unexpected FSI shader");
        return ShaderBytes(descriptor_batch_test_spv);
    };
    base.batch_transfers=base.unified_gpu_queue=base.defer_native_queries=true;
    base.max_draw_batch_size=16;
    for(bool fsi:{false,true})for(bool batched:{false,true}) {
        auto config=base;config.native_fsi=fsi;config.batch_draw_descriptors=batched;
        config.cache_draw_descriptors=false; // Preserve the original exact write-count oracle.
        if(!fsi){config.async_gpu_submissions=false;config.attachmentless_fsi=false;}
        SceneExecutor renderer(config,&device,&queue);uint64_t event=0;
        const Json surface{{"depth",false},{"edram_base",0},{"format",0},{"format_info",0},
            {"surface_info",80},{"width",16},{"height",16}};
        auto draw=[&](const Json& textures) {
            renderer.Execute({{"type","draw"},{"frame",7},{"event",++event},{"function","DrawPrimitive"},
                {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vs_id},
                {"pixel_shader",ps_id},{"surfaces",{{"6",surface}}},{"geometry",Json::array()},
                {"vertex_slots",Json::array()},{"textures",textures}});
        };
        renderer.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
            {"query_generation",10000},{"query_handle",0x1000},{"report_address",0x2020}});
        draw(Json::array({red,green,blue}));draw(Json::array({black,green,blue}));
        Json repeated=Json::array();
        for(unsigned i=0;i<35;++i)repeated.push_back(i&1?black:red);
        repeated.push_back(green);repeated.push_back(blue);draw(repeated);
        if(fsi)Require(renderer.HasPendingDraws(),"Descriptor preparation fenced its current draw");
        renderer.Execute({{"type","query_end"},{"frame",7},{"event",++event},
            {"query_generation",10000},{"query_handle",0x1000},{"report_address",0x2000}});
        renderer.FlushGpuWork();const auto results=renderer.TakeCompletedQueries();
        Require(results.size()==1&&results[0].generation==10000&&results[0].result.draws==3&&
            results[0].result.samples==512,"Descriptor batching changed sparse dimensional sampling or duplicate order");
        if(config.reset_draw_command_pool||config.time_draw_recording) {
            const auto& recording=renderer.report().at("draw_recording");
            Require(recording.at("begins")==3&&recording.at("ends")==3&&
                recording.at("pool_resets")==uint64_t(config.reset_draw_command_pool?3:0)&&
                recording.at("timings_enabled")==config.time_draw_recording,
                "Private draw pool reset or command timing counters lost an accepted buffer");
            for(const auto* field:{"begin_ms","pool_reset_ms","end_ms","enqueue_submit_ms","enqueue_wait_ms","enqueue_host_ms"}) {
                const auto value=recording.at(field).get<double>();
                Require(std::isfinite(value)&&value>=0&&(config.time_draw_recording||value==0),
                    "Draw recording timing enabled unexpectedly or produced an invalid duration");
            }
        }
        if(batched) {
            const auto& stats=renderer.report().at("draw_descriptors");
            Require(stats.at("draws")==3&&stats.at("writes")==86&&stats.at("update_calls")==4&&
                stats.at("bind_calls")==3,"Descriptor batching did not combine writes/binds at expected bounds");
        }
        // Destruction must fence this pending resource change before releasing
        // its descriptors, sampler/views and every previously accepted owner.
        draw(Json::array({black,green,blue}));
        if(fsi)Require(renderer.HasPendingDraws(),"Descriptor shutdown fixture drained too soon");
    }
    std::cout<<"PASS: descriptor off/on four/five sets, 2D31/3D17/cube11 samplers, changed images, "
        "37-entry duplicate array, exact 512 samples per mode and pending shutdown\n";
}
}
int main(int argc,char** argv) try {
    uint32_t inflight_batches=2;
    bool coalesced_ranges=false,reuse_defined_ranges=false,canonical_stores=false;
    bool group_test=false,group_fault=false,discard_dummy=false,compact_version_pages=false,attachmentless=false,cache_descriptors=false;
    bool draw_timestamps=false,asynchronous=false,recycle_images=false,coalesced_barriers=false,skip_read_barriers=false,precise_barriers=false,batch_descriptors=false,reset_draw_pool=false,time_draw_recording=false;uint32_t query_capacity=64,submission_capacity=64;
    for(int i=1;i<argc;++i) {
        const std::string_view arg=argv[i];
        if(arg=="--multi-draw-command-buffers")group_test=true;
        else if(arg=="--multi-draw-follower-failure")group_fault=true;
        else if(arg=="--discard-dummy-fsi-attachment")discard_dummy=true;
        else if(arg=="--attachmentless-fsi")attachmentless=true;
        else if(arg=="--compact-cpu-texture-version-pages")compact_version_pages=true;
        else if(arg=="--canonical-resolve-stores")canonical_stores=true;
        else if(arg=="--reuse-defined-ranges")reuse_defined_ranges=true;
        else if(arg=="--coalesced-texture-ranges")coalesced_ranges=true;
        else if(arg=="--gpu-draw-timestamps")draw_timestamps=true;
        else if(arg=="--async-gpu-submissions")asynchronous=true;
        else if(arg=="--four-inflight-batches")inflight_batches=4;
        else if(arg=="--recycle-resolve-images")recycle_images=true;
        else if(arg=="--coalesce-draw-barriers")coalesced_barriers=true;
        else if(arg=="--skip-readonly-sample-barriers")skip_read_barriers=true;
        else if(arg=="--precise-fsi-barriers")precise_barriers=true;
        else if(arg=="--reset-draw-command-pool")reset_draw_pool=true;
        else if(arg=="--time-draw-recording")time_draw_recording=true;
        else if(arg=="--batch-draw-descriptors")batch_descriptors=true;
        else if(arg=="--cache-draw-descriptors")cache_descriptors=true;
        else if(arg=="--logical-query-capacity=128")query_capacity=128;
        else if(arg=="--async-submission-capacity=128")submission_capacity=128;
        else if(arg=="--async-submission-capacity=64")submission_capacity=64;
        else if(arg=="--logical-query-capacity=64")query_capacity=64;
        else throw std::runtime_error("Unknown scene query fixture argument");
    }
    FsiDevice fsi;auto& device=fsi.device();
    {
        // CPU preflight uses explicit synthetic limits, never substituted
        // device capabilities for commands. Even supported multi-sample bits
        // must refuse host samples other than the unchanged single-sample path.
        VkPhysicalDeviceLimits limits{};limits.maxFramebufferWidth=limits.maxFramebufferHeight=8192;
        limits.maxFramebufferLayers=1;limits.framebufferNoAttachmentsSampleCounts=
            VK_SAMPLE_COUNT_1_BIT|VK_SAMPLE_COUNT_2_BIT|VK_SAMPLE_COUNT_4_BIT;
        Require(NativeAttachmentlessFsiEligible(true,32,32,limits)&&
            !NativeAttachmentlessFsiEligible(false,32,32,limits)&&
            !NativeAttachmentlessFsiEligible(true,0,32,limits)&&
            !NativeAttachmentlessFsiEligible(true,8193,32,limits)&&
            !NativeAttachmentlessFsiEligible(true,32,32,limits,VK_SAMPLE_COUNT_2_BIT)&&
            !NativeAttachmentlessFsiEligible(true,32,32,limits,VK_SAMPLE_COUNT_4_BIT),
            "Attachmentless preflight changed native/extent/single-sample bounds");
        limits.framebufferNoAttachmentsSampleCounts=VK_SAMPLE_COUNT_2_BIT;
        Require(!NativeAttachmentlessFsiEligible(true,32,32,limits),"Missing host sample1 capability was accepted");
        limits.framebufferNoAttachmentsSampleCounts=VK_SAMPLE_COUNT_1_BIT;limits.maxFramebufferLayers=0;
        Require(!NativeAttachmentlessFsiEligible(true,32,32,limits),"Missing framebuffer layers were accepted");
    }
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    std::vector<uint8_t> state(0x2a80);
    auto word=[&](size_t offset,uint32_t value) {
        for(unsigned i=0;i<4;++i)state.at(offset+i)=uint8_t(value>>(24-i*8));
    };
    auto number=[&](size_t offset,float value){word(offset,std::bit_cast<uint32_t>(value));};
    word(0x2880,80);word(0x28bc,16|(16<<16));word(0x28c4,0x80000000);word(0x28c8,16|(16<<16));
    word(0x28dc,15);word(0x2944,1<<19);word(0x294c,0x43f);
    for(unsigned offset:{0x2938,0x2958,0x295c,0x2960})word(offset,0x10001);
    number(0x2908,8);number(0x290c,8);number(0x2910,-8);number(0x2914,8);number(0x2918,1);
    const std::string vs_id(64,'1'),ps_id(64,'2'),state_id(64,'3'),header_id(64,'4'),indices_id(64,'5'),alias_ps_id(64,'6'),pair_ps_id(64,'7');
    std::unordered_map<std::string,OwnedPayload::Snapshot> snapshots;
    snapshots[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
    size_t legacy_payload_calls=0;
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    Require(!config.discard_dummy_fsi_attachment,"Dummy attachment discard default changed");
    config.discard_dummy_fsi_attachment=discard_dummy;
    Require(!config.attachmentless_fsi,"Attachmentless FSI default changed");
    config.attachmentless_fsi=attachmentless;
    Require(!config.compact_cpu_texture_version_pages,"Texture page union default changed");
    config.compact_cpu_texture_version_pages=compact_version_pages;
    config.coalesced_texture_ranges=coalesced_ranges;
    config.reuse_defined_ranges=reuse_defined_ranges;
    config.canonical_resolve_stores=canonical_stores;
    Require(!config.reset_draw_command_pool&&!config.time_draw_recording,"Draw recording experiment changed defaults");
    config.reset_draw_command_pool=reset_draw_pool;config.time_draw_recording=time_draw_recording;
    Require(!config.batch_draw_descriptors,"Descriptor batching default changed");
    config.batch_draw_descriptors=batch_descriptors;
    Require(!config.cache_draw_descriptors,"Descriptor content cache default changed");
    config.cache_draw_descriptors=cache_descriptors;
    Require(!config.coalesce_draw_barriers,"Draw barrier experiment changed its default");
    config.coalesce_draw_barriers=coalesced_barriers;
    Require(!config.skip_readonly_sample_barriers,"Sample barrier experiment changed its default");
    config.skip_readonly_sample_barriers=skip_read_barriers;
    Require(!config.precise_fsi_barriers,"Precise FSI experiment changed its default");
    config.precise_fsi_barriers=precise_barriers;
    Require(config.max_deferred_queries==64,"Default logical query capacity changed");
    for(uint32_t invalid:{0u,129u}) {
        auto bad=config;bad.max_deferred_queries=invalid;bool rejected=false;
        try {SceneExecutor invalid_renderer(bad,&device,queue.get());}
        catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"Invalid logical query capacity accepted");
    }
    Require(config.max_inflight_batches==2,"Default fence window changed");
    for(uint32_t invalid:{0u,1u,3u,5u,UINT32_MAX}) {
        auto bad=config;bad.max_inflight_batches=invalid;bool rejected=false;
        try {SceneExecutor invalid_renderer(bad,&device,queue.get());}
        catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"Invalid fence window accepted");
    }
    Require(config.async_submission_capacity==64,"Default submission capacity changed");
    for(uint32_t invalid:{0u,32u,65u,127u,129u,UINT32_MAX}) {
        auto bad=config;bad.async_submission_capacity=invalid;bool rejected=false;
        try {SceneExecutor invalid_renderer(bad,&device,queue.get());}
        catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"Invalid submission capacity accepted");
    }
    // The legacy non-unified list array is still bounded to 64; opting in to
    // larger physical ownership must not admit a larger draw batch target.
    for(uint32_t invalid:{65u,128u}) {
        auto bad=config;bad.async_submission_capacity=128;bad.max_draw_batch_size=invalid;
        Reject([&]{SceneExecutor invalid_renderer(bad,&device,queue.get());},
            "Physical capacity widened the legacy draw batch bound");
    }
    Require(!config.recycle_resolve_images,"Resolve image recycling default changed");
    config.recycle_resolve_images=recycle_images;
    config.max_deferred_queries=query_capacity;
    config.async_submission_capacity=submission_capacity;
    config.max_inflight_batches=inflight_batches;
    const uint32_t physical_capacity=asynchronous?submission_capacity:64;
    const uint32_t half=physical_capacity/2;
    config.max_draw_batch_size=64;config.defer_native_queries=true;
    config.payload=[&](const std::string& hash) {
        ++legacy_payload_calls;Require(hash==state_id,"Unexpected fixture payload");return state;
    };
    config.payload_snapshot=[&](const std::string& hash) {
        const auto found=snapshots.find(hash);
        Require(found!=snapshots.end(),"Unexpected fixture snapshot");return found->second;
    };
    config.shader=[&](const std::string& hash) {
        Require(hash==vs_id,"Unexpected fixture vertex shader");return ShaderBytes(fullscreen_spv);
    };
    config.fsi_shader=[&](const std::string& hash) {
        if(hash==alias_ps_id)return ShaderBytes(alias_query_test_spv);
        if(hash==pair_ps_id)return ShaderBytes(alias_pair_query_test_spv);
        Require(hash==ps_id,"Unexpected fixture pixel shader");return ShaderBytes(query_test_spv);
    };
    if(!group_test&&!group_fault) {
        auto extent_config=config;extent_config.async_gpu_submissions=asynchronous;
        CheckDescriptorBatches(device,*queue,extent_config,state);
        if(cache_descriptors)CheckDescriptorContentCache(device,*queue,extent_config,state);
        CheckPreciseFsiBarriers(device,*queue,extent_config);
        CheckRetainedFsiExtent(device,*queue,std::move(extent_config));
    }
    SceneExecutor renderer(config,&device,queue.get());
    uint64_t event=0;
    const Json surface{{"depth",false},{"edram_base",0},{"format",0},{"format_info",0},
        {"surface_info",80},{"width",16},{"height",16}};
    auto draw_operation=[&] {
        return Json{{"type","draw"},{"frame",7},{"event",++event},{"function","DrawPrimitive"},
            {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vs_id},
            {"pixel_shader",ps_id},{"surfaces",{{"6",surface}}},{"geometry",Json::array()},
            {"vertex_slots",Json::array()},{"textures",Json::array()}};
    };
    if(group_fault) {
        CheckMultiDrawFollowerFailure(device,*queue,config,draw_operation());
        throw std::runtime_error("Poisoned follower fixture returned without fatal Scene destruction");
    }
    if(group_test) {
        CheckMultiDrawCommandBuffers(device,*queue,config,draw_operation(),state,alias_ps_id);
        auto grouped=config;grouped.multi_draw_command_buffers=true;
        grouped.async_gpu_submissions=grouped.batch_transfers=grouped.unified_gpu_queue=true;
        // These fixtures have independent actual-sample/packed-byte oracles:
        // BDA old/fresh geometry and caller release, query prefixes/empty ends,
        // queued updates, coherent alias1/0/1, timestamp ring wraps, RMW and resize.
        CheckRetiredVertexBuffers(device,*queue,grouped,draw_operation(),state);
        CheckQueuedCpuWrites(device,*queue,grouped,draw_operation(),state,alias_ps_id);
        CheckCompletedQueryPrefixes(device,*queue,grouped,draw_operation());
        CheckCacheCoherenceProgress(device,*queue,grouped,draw_operation(),state,alias_ps_id);
        CheckDrawTimestamps(device,*queue,grouped,draw_operation());
        CheckPreciseFsiBarriers(device,*queue,grouped);
        CheckRetainedFsiExtent(device,*queue,grouped);
        std::cout<<"PASS: multi-draw physical command-buffer integration and independent real GPU ownership/query/history/RMW/extent fixtures\n";
        return 0;
    }
    if(draw_timestamps)CheckDrawTimestamps(device,*queue,config,draw_operation());
    CheckFourFenceScene(device,*queue,config,draw_operation());
    CheckQueuedCpuWrites(device,*queue,config,draw_operation(),state,alias_ps_id);
    CheckRetiredVertexBuffers(device,*queue,config,draw_operation(),state);
    CheckRawFloatCpuTextures(device,*queue,config,draw_operation(),state);
    CheckCompletedQueryPrefixes(device,*queue,config,draw_operation());
    CheckCacheCoherenceProgress(device,*queue,config,draw_operation(),state,alias_ps_id);
    // Both adapters execute the same indexed textured draw, including a
    // cache hit. The live record owns every CPU source; JSON uses the legacy
    // callbacks at their original state/index/miss access points.
    auto typed_adapter_case=[&](bool typed,bool lazy_state=false,bool prewarmed=false,bool prewarm_pressure=false,bool merged_barriers=false) {
        SceneExecutorConfig paired_config=config;
        paired_config.coalesce_draw_barriers=merged_barriers||config.coalesce_draw_barriers;
        paired_config.defer_native_queries=true;paired_config.batch_transfers=true;
        paired_config.unified_gpu_queue=true;paired_config.async_gpu_submissions=asynchronous;
        if(prewarm_pressure){paired_config.max_pipeline_cache_entries=1;paired_config.max_shader_cache_entries=2;}
        std::unordered_map<std::string,OwnedPayload::Snapshot> inputs;
        size_t payload_reads=0,legacy_shader_reads=0,owned_shader_reads=0,observed_pipelines=0;
        paired_config.observe_pipeline=[&](ObservedNativePipeline observation) {
            Require(observation.vertex&&observation.pixel&&ValidNativeFsiPipelineRecipe(observation.recipe),
                "Observed pipeline lost immutable source owners or exact key");
            ++observed_pipelines;
        };
        const auto resource=[&](char tag,std::vector<uint8_t> bytes) {
            const std::string hash(64,tag);
            auto owner=std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
            inputs[hash]=owner;return CanonicalResourceRef{CanonicalDigest(hash),std::move(owner)};
        };
        TypedLiveDraw input;input.frame=7;input.event=++event;
        input.function=DrawFunction::DrawIndexedPrimitive;input.args={0,4,0,3,0,0,0,0};
        auto state_bytes=state;if(lazy_state)state_bytes.resize(LazyDeviceState::kBytes);
        input.device_state=resource('a',std::move(state_bytes));
        input.vertex_shader=resource('b',{0x11});input.pixel_shader=resource('c',{0x22});
        input.surfaces[0]=TypedSurface{0,16,16,80,0,0,0,false};
        input.vertex_fetch_slots={0};
        input.geometry.push_back(TypedGeometry{14,0x7000,64,resource('d',std::vector<uint8_t>(64)),false,0,0});
        input.geometry.push_back(TypedGeometry{30,0,4,resource('e',{0x20,0,0,0})});
        input.geometry.push_back(TypedGeometry{31,0,6,resource('f',{0,0,0,1,0,2})});
        std::vector<uint8_t> texture(4096);const std::array<float,4> red{1,0,0,1};
        std::memcpy(texture.data(),red.data(),sizeof(red));
        TypedTexture binding;binding.slot=0;binding.fetch={2u|(1u<<22),0x6000u|38u,0,0,0,1u<<9};
        binding.base_payload=resource('9',std::move(texture));input.textures.push_back(std::move(binding));
        std::weak_ptr<const std::vector<uint8_t>> state_owner=input.device_state.owner;
        if(lazy_state) {
            input.lazy_device_state=std::make_shared<const LazyDeviceState>(input.device_state.owner);
            input.device_state={};
        }
        std::weak_ptr<const std::vector<uint8_t>> index_owner=input.geometry[2].payload.owner;
        paired_config.payload_snapshot=[&](const std::string& hash) {
            ++payload_reads;return inputs.at(hash);
        };
        paired_config.shader=[&](const std::string& hash) {
            ++legacy_shader_reads;Require(hash==std::string(64,'b'),"Paired fixture lost VS identity");
            return ShaderBytes(fullscreen_spv);
        };
        paired_config.fsi_shader=[&](const std::string& hash) {
            ++legacy_shader_reads;Require(hash==std::string(64,'c'),"Paired fixture lost PS identity");
            return ShaderBytes(alias_query_test_spv);
        };
        paired_config.shader_snapshot=[&](const CanonicalResourceRef& source,bool fsi) {
            ++owned_shader_reads;
            Require(source.owner&&source.hash.View()==std::string(64,fsi?'c':'b')&&
                source.owner->at(0)==(fsi?0x22:0x11),"Paired shader lost immutable source-container owner");
            return fsi?ShaderBytes(alias_query_test_spv):ShaderBytes(fullscreen_spv);
        };
        {
            SceneExecutor paired(paired_config,&device,queue.get());
            if(prewarmed) {
                const NativeFsiPipelineRecipe recipe{std::string(64,'b'),std::string(64,'c'),
                    {uint32_t(RenderCullMode::NONE),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
                        uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
                paired.PrewarmFsiPipeline(recipe,fullscreen_spv,alias_query_test_spv);
                if(prewarm_pressure) {
                    auto alternate=recipe;alternate.pixel=std::string(64,'8');
                    paired.PrewarmFsiPipeline(alternate,fullscreen_spv,query_test_spv);
                    paired.PrewarmFsiPipeline(recipe,fullscreen_spv,alias_query_test_spv);
                }
                paired.PrewarmFsiPipeline(recipe,fullscreen_spv,alias_query_test_spv);
                Require(!paired.mutation_serial()&&!paired.HasPendingGpuWork()&&!paired.DeferredQueryCount()&&
                    paired.report().at("pipelines_created")==unsigned(prewarm_pressure?3:1)&&payload_reads==0&&owned_shader_reads==0&&legacy_shader_reads==0,
                    "Startup preparation changed GPU history, called frame providers, or duplicated a pipeline");
                // Initial real EDRAM import remains legal after preparation.
                paired.InitializeEdram(std::vector<uint8_t>(10u*1024*1024));
            }
            paired.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",9},{"query_handle",0x1000},{"report_address",0x2020}});
            for(unsigned repeat=0;repeat<2;++repeat) {
                input.event=++event;
                if(typed)paired.Execute(input);else paired.Execute(ToJson(input));
            }
            Require(typed?(payload_reads==0&&owned_shader_reads==(prewarmed?0u:2u)&&legacy_shader_reads==0):
                (payload_reads==8&&owned_shader_reads==0&&legacy_shader_reads==2),
                "Typed/cache-hit adaptation changed the original provider read sites");
            Require(observed_pipelines==(typed&&!prewarmed?1u:0u),"Observed recipe wasn't unique or warm cache missed");
            if(prewarmed)Require(paired.report().at("pipelines_created")==unsigned(prewarm_pressure?3:1),"First actual draw recreated its prepared pipeline");
            Require(!lazy_state||!input.lazy_device_state->HasDigest(),
                "Typed GPU state consumption eagerly computed a canonical digest");
            const auto serial=paired.mutation_serial();
            auto malformed=input;malformed.vertex_fetch_slots={96};malformed.event=++event;
            std::string error;
            try {if(typed)paired.Execute(malformed);else paired.Execute(ToJson(malformed));}
            catch(const std::exception& failure){error=failure.what();}
            Require(error=="Unsupported vertex fetch slot"&&paired.mutation_serial()==serial,
                "Malformed typed/JSON vertex requirement changed rejection or GPU mutation");
            malformed=TypedLiveDraw{};
            if(typed) {
                auto unassigned=input;unassigned.device_state.hash=CanonicalDigest{};
                if(unassigned.lazy_device_state) {
                    unassigned.device_state.owner=unassigned.lazy_device_state->Owner();
                    unassigned.lazy_device_state.reset();
                }
                bool rejected=false;
                try {paired.Execute(unassigned);}
                catch(const std::invalid_argument&) {rejected=true;}
                Require(rejected&&paired.mutation_serial()==serial,
                    "Typed required digest bypassed validation through its owned payload");
            }
            paired.Execute({{"type","query_end"},{"frame",7},{"event",++event},
                {"query_generation",9},{"query_handle",0x1000},{"report_address",0x2000}});
            input=TypedLiveDraw{};inputs.clear();
            Require(state_owner.expired()&&!index_owner.expired()&&paired.HasPendingGpuWork(),
                "Typed draw retained transient CPU state or dropped pending immutable indices");
            paired.FlushGpuWork();const auto results=paired.TakeCompletedQueries();
            Require(results.size()==1&&results[0].generation==9&&results[0].result.draws==2&&
                results[0].result.samples==512,"Typed and JSON indexed textured draws differ on the GPU");
        }
        Require(index_owner.expired(),"Paired executor retained its immutable index owner after shutdown");
    };
    typed_adapter_case(false);typed_adapter_case(true);typed_adapter_case(true,true);typed_adapter_case(true,false,true);typed_adapter_case(true,false,true,true);
    // Actual indexed sampled draws and512 fenced samples through the opt-in
    // adjacent barrier path, including module/geometry/binding cache hits.
    typed_adapter_case(true,false,false,false,true);
    auto draw=[&]{renderer.Execute(draw_operation());};
    auto begin=[&](uint64_t generation) {
        renderer.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
            {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2020}});
    };
    auto end=[&](uint64_t generation) {
        renderer.Execute({{"type","query_end"},{"frame",7},{"event",++event},
            {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2000}});
        return event;
    };
    auto check=[&](const CompletedNativeQuery& q,uint64_t generation,uint64_t end_event,uint64_t draws) {
        Require(q.frame==7&&q.generation==generation&&q.end_event==end_event&&q.result.handle==0x1000&&
            q.result.begin_address==0x2020&&q.result.end_address==0x2000&&q.result.draws==draws&&
            q.result.samples==draws*256,"Typed query completion lost identity or actual GPU count");
    };
    // Queryless raster still orders a later empty query's availability.
    draw();Require(renderer.HasPendingDraws(),"Fixture did not retain an unqueried native draw");
    begin(10);const auto empty_event=end(10);
    Require(renderer.HasPendingDraws()&&renderer.DeferredQueryCount()==1,"Empty end forced a drain or lost its record");
    Require(!renderer.ActiveQueryMetadata(),"Closed empty query retained active metadata");
    Require(!renderer.report().contains("native_occlusion_results"),"Unfenced empty query appeared completed");
    Reject([&]{renderer.TakeCompletedQueries();},"Empty query passed unrelated pending raster work");
    renderer.FlushPendingDraws();
    Require(renderer.DeferredQueryCount()==1,"Internal flush consumed typed query metadata");
    auto completed=renderer.TakeCompletedQueries();Require(completed.size()==1,"Empty result was lost");
    check(completed[0],10,empty_event,0);
    Require(renderer.TakeCompletedQueries().empty(),"Typed empty result consumed twice");

    begin(11);draw();draw();const auto first_event=end(11);
    begin(12);const auto second_event=end(12);
    begin(13);draw();const auto third_event=end(13);
    begin(14);draw();
    const auto metadata=renderer.ActiveQueryMetadata();
    Require(metadata&&metadata->draws==1&&metadata->pending_draws==1,"Closed queries leaked into active metadata");
    Require(renderer.DeferredQueryCount()==3&&renderer.HasPendingDraws(),"Ended queries did not share a pending batch");
    Reject([&]{renderer.TakeCompletedQueries();},"Multiple query results consumed before full fence");
    renderer.FlushPendingDraws();
    Require(renderer.DeferredQueryCount()==3,"Automatic retirement discarded closed queries");
    completed=renderer.TakeCompletedQueries();Require(completed.size()==3,"Multiple query results lost order");
    check(completed[0],11,first_event,2);check(completed[1],12,second_event,0);check(completed[2],13,third_event,1);
    const auto active=renderer.ActiveQuery();Require(active&&active->samples==256&&active->draws==1,"Active subtotal lost on closed consumption");
    draw();const auto fourth_event=end(14);renderer.FlushPendingDraws();
    completed=renderer.TakeCompletedQueries();Require(completed.size()==1,"Active query end was lost");
    check(completed[0],14,fourth_event,2);

    // The CPU provider may retire immediately after Execute even though the
    // indexed draw is still pending. The bounded index cache retains only its
    // immutable index owner; state and header snapshots remain temporary.
    snapshots[header_id]=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0x20,0,0,0});
    snapshots[indices_id]=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0,0,0,1,0,2});
    std::array<std::weak_ptr<const std::vector<uint8_t>>,3> old_owners{
        snapshots.at(state_id),snapshots.at(header_id),snapshots.at(indices_id)};
    begin(15);auto indexed=draw_operation();indexed["function"]="DrawIndexedPrimitive";
    indexed["geometry"]=Json::array({{{"kind",30},{"sha256",header_id}},{{"kind",31},{"sha256",indices_id}}});
    renderer.Execute(indexed);const auto owned_event=end(15);
    snapshots.clear();
    Require(old_owners[0].expired()&&old_owners[1].expired(),"Draw retained temporary state/header snapshots");
    Require(!old_owners[2].expired(),"Index cache lost its immutable source owner");
    Require(renderer.HasPendingDraws()&&legacy_payload_calls==0,"Borrowed payload draw drained early or used legacy copies");
    // A malformed provider can reuse a digest for changed bytes. It must use
    // a fresh upload while the old cached buffer is still consumed by query15.
    snapshots[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
    snapshots[header_id]=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0x20,0,0,0});
    snapshots[indices_id]=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0,0,0,0,0,1});
    begin(16);indexed["event"]=++event;renderer.Execute(indexed);const auto changed_event=end(16);
    renderer.FlushPendingDraws();completed=renderer.TakeCompletedQueries();
    Require(completed.size()==2,"Provider retirement or changed source lost the pending indexed draws");
    check(completed[0],15,owned_event,1);
    const auto& changed=completed[1];
    Require(changed.frame==7&&changed.generation==16&&changed.end_event==changed_event&&
        changed.result.handle==0x1000&&changed.result.begin_address==0x2020&&changed.result.end_address==0x2000&&
        changed.result.draws==1&&changed.result.samples==0,"Changed index bytes reused stale cached geometry");
    snapshots.clear();

    // A full logical queue of empty ends must remain behind an unrelated
    // pending draw, even when there are more logical records than GPU slots.
    snapshots[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
    draw();
    for(uint64_t generation=1000;generation<1000+query_capacity;++generation){begin(generation);end(generation);}
    Require(renderer.HasPendingGpuWork()&&renderer.DeferredQueryCount()==query_capacity,"Logical pressure forced early unqueried predecessor completion");
    Reject([&]{renderer.TakeCompletedQueries();},"Full empty queue overtook unrelated draw");
    renderer.FlushGpuWork();completed=renderer.TakeCompletedQueries();
    Require(completed.size()==query_capacity,"Full empty queue lost fenced results");
    for(size_t i=0;i<completed.size();++i)Require(completed[i].generation==1000+i&&completed[i].result.samples==0&&completed[i].result.draws==0,"Unqueried predecessor changed empty query identity");
    begin(2000);draw();end(2000);
    for(uint64_t generation=2001;generation<2000+query_capacity;++generation){begin(generation);end(generation);}
    Require(renderer.HasPendingGpuWork()&&renderer.DeferredQueryCount()==query_capacity,"Logical pressure forced early queried predecessor completion");
    Reject([&]{renderer.TakeCompletedQueries();},"Empty queries overtook a queried predecessor");
    renderer.FlushGpuWork();completed=renderer.TakeCompletedQueries();
    Require(completed.size()==query_capacity,"Queried predecessor lost full logical queue");
    for(size_t i=0;i<completed.size();++i)Require(completed[i].generation==2000+i&&completed[i].result.samples==(i?0u:256u)&&completed[i].result.draws==(i?0u:1u),"Queried predecessor sample ownership changed");
    snapshots.clear();
    // Ready empty results still occupy their logical entries until consumed.
    const auto before_query_reports=renderer.report().at("query_report_serializations").get<uint64_t>();
    const auto before_query_history=renderer.report().at("native_occlusion_results").size();
    for(uint64_t generation=100;generation<100+query_capacity;++generation){begin(generation);end(generation);}
    Require(renderer.DeferredQueryCount()==query_capacity,"Ready zero-draw records escaped capacity accounting");
    begin(100+query_capacity);Reject([&]{end(100+query_capacity);},"Ready empty query beyond capacity was accepted");
    Require(renderer.ActiveQueryMetadata().has_value(),"Full queue lost retryable active query");
    completed=renderer.TakeCompletedQueries();Require(completed.size()==query_capacity,"Full ready queue lost records");
    for(size_t i=0;i<completed.size();++i)Require(completed[i].generation==100+i&&completed[i].result.samples==0&&
        completed[i].result.draws==0,"Ready queue identity or real zero changed");
    const Json ready_history=renderer.report();
    Require(ready_history.at("query_report_serializations")==before_query_reports+1&&
        ready_history.at("native_occlusion_results").size()==before_query_history+query_capacity&&
        renderer.report()==ready_history,
        "Completed queries rebuilt diagnostics per result or changed repeated observations");
    const auto retry_event=end(100+query_capacity);completed=renderer.TakeCompletedQueries();
    Require(completed.size()==1,"Empty query retry failed");check(completed[0],100+query_capacity,retry_event,0);
    Require(!renderer.HasPendingDraws()&&!renderer.DeferredQueryCount()&&!renderer.ActiveQueryMetadata(),"Query fixture left accepted work");
    // Explicitly retain the original synchronous default/report contract.
    config.defer_native_queries=false;config.payload_snapshot={};
    SceneExecutor synchronous(config,&device,queue.get());
    synchronous.Execute({{"type","query_begin"},{"frame",8},{"event",1},{"query_handle",0x3000},{"report_address",0x4020}});
    synchronous.Execute({{"type","query_end"},{"frame",8},{"event",2},{"query_handle",0x3000},{"report_address",0x4000}});
    Require(!synchronous.DeferredQueryCount()&&synchronous.TakeCompletedQueries().empty()&&
        synchronous.report().at("native_occlusion_results").at(0).at("samples")==0,"Synchronous query contract changed");
    synchronous.Execute({{"type","query_begin"},{"frame",8},{"event",3},{"query_handle",0x3000},{"report_address",0x4020}});
    auto legacy_draw=draw_operation();legacy_draw["frame"]=8;legacy_draw["event"]=4;synchronous.Execute(legacy_draw);
    synchronous.Execute({{"type","query_end"},{"frame",8},{"event",5},{"query_handle",0x3000},{"report_address",0x4000}});
    Require(legacy_payload_calls==1&&synchronous.report().at("native_occlusion_results").at(1).at("samples")==256,
        "Legacy vector payload callback changed draw results");
    // An exact pipeline absent for one frame must remain reusable. Small hard
    // limits separately exercise module/pipeline eviction while real queried
    // draws are pending, including a new vertex selected before the PS miss.
    auto cache_case=[&](bool pressure) {
        auto cached_config=config;
        cached_config.defer_native_queries=true;cached_config.batch_transfers=true;
        cached_config.unified_gpu_queue=true;cached_config.async_gpu_submissions=asynchronous;
        cached_config.max_pipeline_cache_entries=pressure?1:8;
        cached_config.max_shader_cache_entries=pressure?2:8;
        cached_config.max_shader_cache_bytes=pressure?sizeof(fullscreen_spv)+sizeof(query_test_spv):64u*1024*1024;
        const std::string other_vs(64,'0'),other_ps(64,'9');
        cached_config.shader=[&](const std::string& hash) {
            Require(hash==vs_id||hash==other_vs,"Unexpected cache fixture vertex shader");
            return ShaderBytes(fullscreen_spv);
        };
        cached_config.fsi_shader=[&](const std::string& hash) {
            Require(hash==ps_id||hash==other_ps,"Unexpected cache fixture pixel shader");
            return ShaderBytes(query_test_spv);
        };
        SceneExecutor cached(cached_config,&device,queue.get());
        const std::array<uint32_t,6> front_fetch{2u|(1u<<22),0x1000u|38u,0,0,0,1u<<9};
        const std::array<uint8_t,16> black{};cached.InitializeMemory(0x1000,black);
        const Json& unobserved_diagnostics=cached.report();
        const uint64_t initial_serializations=unobserved_diagnostics.at("diagnostic_serializations");
        for(const auto* key:{"index_cache","texture_binding_cache","draw_batches","texture_updates",
                "draw_resource_reuse","draw_timings","completed_frames","anisotropy_override",
                "pipelines_created","direct_fixed_source_resolves","last_accepted_event","last_completed_event"})
            Require(!unobserved_diagnostics.contains(key),"Fresh diagnostics fabricated an unobserved field");
        auto present=[&](uint32_t frame=7) {
            // Reusing the guest frame ID also verifies internal epoch identity.
            cached.Execute({{"type","present"},{"frame",frame},{"event",++event},
                {"frontbuffer",{{"base","00001000"},{"format",38},{"width",1},{"height",1}}},
                {"frontbuffer_fetch",front_fetch},{"provenance","cache fixture"}});
        };
        cached.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
            {"query_generation",190},{"query_handle",0x1000},{"report_address",0x2020}});
        uint64_t last_cache_draw_event=0;
        for(unsigned pass=0;pass<3;++pass) {
            auto op=draw_operation();last_cache_draw_event=op.at("event");
            if(pass==1){op["pixel_shader"]=other_ps;if(pressure)op["vertex_shader"]=other_vs;}
            cached.Execute(op);
            if(!pressure)present();
        }
        Require(unobserved_diagnostics.at("diagnostic_serializations")==initial_serializations&&
            unobserved_diagnostics.at("draws")==0&&!unobserved_diagnostics.contains("last_accepted_event")&&
            !unobserved_diagnostics.contains("completed_frames")&&!unobserved_diagnostics.contains("draw_timings"),
            "Unobserved draws/frame boundaries serialized or mutated cached diagnostics");
        const auto cache_end=++event;
        cached.Execute({{"type","query_end"},{"frame",7},{"event",cache_end},
            {"query_generation",190},{"query_handle",0x1000},{"report_address",0x2000}});
        cached.FlushGpuWork();const auto results=cached.TakeCompletedQueries();
        Require(results.size()==1,"Persistent cache fixture lost query identity");
        check(results[0],190,cache_end,3);
        const Json cached_report=cached.report();
        Require(cached_report.at("diagnostic_serializations")==initial_serializations+1&&
            cached_report.at("draws")==3&&cached_report.at("last_accepted_event")==cache_end&&
            cached_report.at("last_completed_event")== (pressure?last_cache_draw_event:cache_end)&&cached.report()==cached_report,
            "Lazy observation lost cumulative accepted/completed counters or rebuilt unchanged data");
        if(!pressure)Require(cached_report.at("completed_frames")==Json::array({7,7,7})&&
            cached_report.at("draw_timings").at("draws")==1&&
            cached_report.at("texture_updates").at("frame")==7,
            "Completed-frame snapshots accumulated later frames or lost repeated guest frame IDs");
        const auto& pipelines=cached_report.at("pipeline_cache");
        const auto& shaders=cached_report.at("shader_cache");
        if(pressure) {
            Require(pipelines.at("resident")==1&&pipelines.at("creations")==3&&pipelines.at("evictions")==2&&
                pipelines.at("pressure_drains")==2&&shaders.at("resident")==2&&shaders.at("creations")==6&&
                shaders.at("evictions")==4&&shaders.at("spirv_bytes").get<uint64_t>()<=cached_config.max_shader_cache_bytes,
                "Pending pipeline/module pressure exceeded bounds or lost preparing shaders");
        } else Require(pipelines.at("creations")==2&&pipelines.at("hits")==1&&pipelines.at("evictions")==0&&
            shaders.at("creations")==3&&shaders.at("evictions")==0,
            "A/B/A across completed frames recreated a persistent pipeline or shader");
        const Json same_frame_history=cached.report().at("native_occlusion_results");
        if(!pressure) {
            const auto finished_timings=cached.report().at("draw_timings");
            const auto finished_textures=cached.report().at("texture_updates");
            const auto finished_indices=cached.report().at("index_cache");
            auto later=draw_operation();cached.Execute(later);
            const auto& midframe=cached.report();
            Require(midframe.at("draws")==4&&midframe.at("last_accepted_event")==later.at("event")&&
                midframe.at("draw_timings")==finished_timings&&midframe.at("texture_updates")==finished_textures&&
                midframe.at("index_cache")==finished_indices,
                "Mid-frame observation replaced the previous completed-frame snapshot");
        }
        present();
        Require(cached.report().at("native_occlusion_results")==same_frame_history,
            "Same-frame presentation lost completed diagnostic queries");
        present(8);
        Require(cached.report().at("native_occlusion_results").empty()&&cached.TakeCompletedQueries().empty(),
            "Next frame retained stale diagnostic queries or republished consumed results");
    };
    cache_case(false);cache_case(true);
    auto residency_case=[&](bool retain) {
        auto resident_config=config;
        // Earlier legacy-payload checks deliberately clear config's snapshot
        // callback and its state owner. This fixture owns an independent bank.
        std::unordered_map<std::string,OwnedPayload::Snapshot> resident_snapshots;
        resident_snapshots[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
        resident_config.payload_snapshot=[&](const std::string& hash) {
            const auto found=resident_snapshots.find(hash);
            Require(found!=resident_snapshots.end(),"Unexpected dormant residency snapshot");
            return found->second;
        };
        resident_config.defer_native_queries=true;resident_config.batch_transfers=true;
        resident_config.unified_gpu_queue=true;resident_config.async_gpu_submissions=asynchronous;
        if(!retain) {
            resident_config.max_dormant_texture_bytes=resident_config.max_dormant_vertex_bytes=0;
            resident_config.max_dormant_texture_entries=resident_config.max_dormant_vertex_entries=0;
        }
        const std::array<std::string,2> texture_ids{std::string(64,'a'),std::string(64,'b')};
        const std::array<std::string,2> vertex_ids{std::string(64,'c'),std::string(64,'d')};
        for(unsigned i=0;i<2;++i) {
            std::vector<uint8_t> texture(4096);
            const std::array<float,4> pixel{float(1-i),0,0,1};
            std::memcpy(texture.data(),pixel.data(),sizeof(pixel));
            resident_snapshots[texture_ids[i]]=std::make_shared<const std::vector<uint8_t>>(std::move(texture));
            resident_snapshots[vertex_ids[i]]=std::make_shared<const std::vector<uint8_t>>(64,uint8_t(i));
        }
        SceneExecutor resident(resident_config,&device,queue.get());
        const std::array<uint32_t,6> front{2u|(1u<<22),0x1000u|38u,0,0,0,1u<<9};
        const std::array<uint32_t,6> fetch{2u|(1u<<22),0x6000u|38u,0,0,0,1u<<9};
        const std::array<uint8_t,16> black{};resident.InitializeMemory(0x1000,black);
        auto present=[&] {
            resident.Execute({{"type","present"},{"frame",7},{"event",++event},
                {"frontbuffer",{{"base","00001000"},{"format",38},{"width",1},{"height",1}}},
                {"frontbuffer_fetch",front},{"provenance","dormant residency fixture"}});
        };
        auto sample=[&](unsigned which) {
            auto op=draw_operation();op["pixel_shader"]=alias_ps_id;
            // Fullscreen's vertex program does not consume this stream; its
            // allocation/retirement is checked by counters and the CPU owner
            // fixture. Pixel sampling below checks actual retained GPU images.
            op["vertex_slots"]=Json::array({0});
            op["geometry"]=Json::array({{{"kind",14},{"fetch_slot",0},{"endian",0},
                {"address","00007000"},{"bytes",64},{"sha256",vertex_ids[which]}}});
            op["textures"]=Json::array({{{"slot",0},{"source","cpu_snapshot"},
                {"fetch",fetch},{"base_payload",texture_ids[which]},{"mip_payload",nullptr}}});
            resident.Execute(op);
        };
        resident.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
            {"query_generation",195},{"query_handle",0x1000},{"report_address",0x2020}});
        for(unsigned which:{0u,1u,0u}){sample(which);present();}
        resident.Execute({{"type","query_end"},{"frame",7},{"event",++event},
            {"query_generation",195},{"query_handle",0x1000},{"report_address",0x2000}});
        resident.FlushGpuWork();auto results=resident.TakeCompletedQueries();
        Require(results.size()==1&&results[0].generation==195&&results[0].result.draws==3&&
            results[0].result.samples==512,"Dormant CPU image A/B/A changed real sampled pixels");
        for(const auto* field:{"texture_cache","vertex_cache"}) {
            const auto& stats=resident.report().at(field);
            Require(stats.at("creations")==uint64_t(retain?2:3)&&
                stats.at("dormant_hits")==uint64_t(retain?1:0)&&
                stats.at("evictions")==uint64_t(retain?0:2)&&
                stats.at("allocation_bytes").get<uint64_t>()>0&&
                stats.at("extra_dormant_bytes").get<uint64_t>()<=
                    stats.at("maximum_extra_dormant_bytes").get<uint64_t>(),
                "Dormant allocation budget or A/B/A immutable owner reuse changed");
        }
        // Same fetch and immutable CPU SHA, newer physical overlay version:
        // the cache must miss and sample black, never the retained red image.
        const auto created=resident.report().at("texture_cache").at("creations").get<uint64_t>();
        resident.InitializeMemory(0x6000,black);
        resident.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
            {"query_generation",196},{"query_handle",0x1000},{"report_address",0x2020}});
        sample(0);
        resident.Execute({{"type","query_end"},{"frame",7},{"event",++event},
            {"query_generation",196},{"query_handle",0x1000},{"report_address",0x2000}});
        resident.FlushGpuWork();results=resident.TakeCompletedQueries();
        Require(results.size()==1&&results[0].generation==196&&results[0].result.draws==1&&
            results[0].result.samples==0&&resident.report().at("texture_cache").at("creations")==created+1,
            "Dormant image reuse ignored the changed physical memory version");
        // End with a retained texture hit pending, exercising normal shutdown
        // completion before the cached image/view and vertex owner are freed.
        sample(0);Require(resident.HasPendingGpuWork(),"Dormant resource shutdown fixture fenced early");
    };
    residency_case(true);residency_case(false);
    // Unified mode must order transfer-only work too: a zero-draw query is
    // unavailable until the older clear/store command list actually completes.
    config.defer_native_queries=true;config.batch_transfers=true;config.unified_gpu_queue=true;
    config.async_gpu_submissions=asynchronous;
    SceneExecutor unified(config,&device,queue.get());
    const Json initial_diagnostics=unified.report();
    Require(initial_diagnostics.at("diagnostic_serializations")==1&&
        unified.report()==initial_diagnostics&&unified.report()==initial_diagnostics,
        "Repeated fresh report observations rebuilt diagnostic JSON");
    const Json fixed{{"depth",false},{"edram_base",8},{"format",5},{"format_info",5},
        {"surface_info",80},{"width",1},{"height",1}};
    auto clear=[&](float red) {
        unified.Execute({{"type","clearrect"},{"frame",7},{"event",++event},{"flags",1},
            {"rectangle",{0,0,1,1}},{"color",{red,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
            {"surfaces",{{"6",fixed}}}});
    };
    auto unified_begin=[&](uint64_t generation) {
        unified.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
            {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2020}});
    };
    auto unified_end=[&](uint64_t generation) {
        unified.Execute({{"type","query_end"},{"frame",7},{"event",++event},
            {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2000}});
        return event;
    };
    clear(1);unified_begin(200);const auto transfer_empty_event=unified_end(200);
    Require(!unified.HasPendingDraws()&&unified.HasPendingGpuWork()&&unified.DeferredQueryCount()==1,
        "Transfer-only work did not gate an empty query");
    Reject([&]{unified.TakeCompletedQueries();},"Empty query overtook pending clear/store transfers");
    const Json pending_diagnostics=unified.report();
    Require(pending_diagnostics.at("diagnostic_serializations")==2&&
        pending_diagnostics.at("transfer_batches").at("queued_lists").get<uint64_t>()>0&&
        pending_diagnostics.at("gpu_work_queue").at("waits")==0&&unified.HasPendingGpuWork()&&
        unified.report()==pending_diagnostics,
        "Diagnostic observation fenced work or failed to refresh pending transfer counters");
    unified.FlushGpuWork();completed=unified.TakeCompletedQueries();
    Require(completed.size()==1&&!unified.HasPendingGpuWork(),"Transfer fence lost empty completion");
    check(completed[0],200,transfer_empty_event,0);
    const Json drained_diagnostics=unified.report();
    Require(drained_diagnostics.at("diagnostic_serializations")==3&&
        drained_diagnostics.at("gpu_work_queue").at("waits")==1&&
        unified.report()==drained_diagnostics,
        "Explicit GPU drain did not invalidate the observed diagnostic snapshot");
    // Execute flushes accepted work before rejecting an unknown non-draw
    // operation. Its failure report must observe that real completed fence.
    clear(0);
    const Json before_failure=unified.report();
    Reject([&]{unified.Execute({{"type","invalid-diagnostic-fixture"},{"frame",7},{"event",++event}});},
        "Invalid diagnostic fixture operation was accepted");
    const Json after_failure=unified.report();
    Require(after_failure.at("gpu_work_queue").at("waits").get<uint64_t>()==
            before_failure.at("gpu_work_queue").at("waits").get<uint64_t>()+1&&
        after_failure.at("native_occlusion_results")==before_failure.at("native_occlusion_results")&&
        !unified.HasPendingGpuWork()&&unified.report()==after_failure,
        "Failure observation missed completed work or changed query reporting");

    // More than two physical capacities of alternating lists exercise retirement
    // while logical query ownership spans multiple shared submission fences.
    unified_begin(201);
    for(unsigned i=0;i<physical_capacity+8;++i) {
        clear(float(i));unified.Execute(draw_operation());
    }
    const auto mixed_end=unified_end(201);
    Require(unified.HasPendingGpuWork()&&unified.DeferredQueryCount()==1,
        "Mixed query failed to retain its final GPU work");
    Reject([&]{unified.TakeCompletedQueries();},"Mixed query became available before its final list");
    unified.FlushGpuWork();completed=unified.TakeCompletedQueries();
    Require(completed.size()==1,"Mixed pressure drain lost typed query identity");
    check(completed[0],201,mixed_end,physical_capacity+8);

    // A clear->resolve->draw chain remains pending across operation boundaries.
    // CPU readback is the required fence and sees the exact encoded clear, not
    // a stale image from an earlier transfer packet or draw-slot generation.
    clear(16);
    const std::array<uint32_t,6> fetch{2u|(1u<<22),0x1000u|38u,0,0,0,1u<<9};
    unified.Execute({{"type","resolve"},{"frame",7},{"event",++event},{"level",0},{"flags",0},
        {"source_slot",0},{"source_rectangle",{0,0,1,1}},{"destination_rectangle",{0,0,1,1}},
        {"destination",{{"base","00001000"},{"format",38},{"width",1},{"height",1}}},
        {"destination_fetch",fetch},{"exponent_bias",0},{"copy_dest_swap",false},{"surfaces",{{"6",fixed}}}});
    unified.Execute(draw_operation());
    Require(unified.HasPendingGpuWork()&&unified.HasPendingDraws(),"Mixed resolve chain fenced prematurely");
    const auto copied=unified.ReadPhysical(0x1000,16);
    std::array<float,4> pixel;std::memcpy(pixel.data(),copied.data(),16);
    const float quantized_red=std::round(16.f*(32767.f/32.f))/32767.f*32.f;
    Require(std::abs(pixel[0]-quantized_red)<.00001f&&pixel[1]==0&&pixel[2]==0&&
        !unified.HasPendingGpuWork(),"Readback did not fence exact mixed transfer/draw contents");
    // Canonical GPU aliases now decode into an owned queued image before the
    // consuming draw. Actual fragment discard makes visibility depend on the
    // converted texel rather than merely proving that a command was submitted.
    const std::array<uint32_t,6> alias_fetch{2u|(1u<<22),0x2000u|38u,0,0,0,1u<<9};
    const Json alias_identity{{"base","00002000"},{"format",38},{"width",1},{"height",1}};
    auto alias_draw=[&] {
        auto op=draw_operation();op["pixel_shader"]=alias_ps_id;
        op["textures"]=Json::array({{{"slot",0},{"source","gpu_alias_or_previous_frame"},
            {"identity",alias_identity},{"fetch",alias_fetch}}});
        unified.Execute(op);
    };
    auto refresh_stats=[&] {
        (void)unified.report();
    };
    auto set_alias_red=[&](float value) {
        const std::array<float,4> color{value,0,0,1};std::array<uint8_t,16> encoded{};
        for(size_t i=0;i<color.size();++i) {
            const auto bits=std::bit_cast<uint32_t>(color[i]);
            for(unsigned b=0;b<4;++b)encoded[i*4+b]=uint8_t(bits>>(b*8));
        }
        unified.InitializeMemory(0x2000,encoded);refresh_stats();
    };
    for(unsigned pass=0;pass<3;++pass) {
        // Repeat the positive case after zero to cover both directions of
        // same-address version changes and stale cached-view replacement.
        set_alias_red(pass==1?0.f:1.f);
        const uint64_t queued_before=unified.report().at("transfer_batches").at("queued_lists");
        const uint64_t submits_before=unified.report().at("gpu_work_queue").at("submissions");
        const uint64_t generation=210+pass;unified_begin(generation);
        alias_draw();alias_draw();const auto alias_end=unified_end(generation);
        Require(unified.HasPendingGpuWork()&&unified.HasPendingDraws(),"Alias conversion/draw fenced prematurely");
        Require(unified.report().at("transfer_batches").at("queued_lists")==queued_before+1,
            "Canonical alias was not queued exactly once and reused by the next draw");
        Require(unified.report().at("gpu_work_queue").at("submissions")==submits_before,
            "Canonical2D alias conversion inserted an extra CPU fence");
        Reject([&]{unified.TakeCompletedQueries();},"Alias-dependent query was published before its draw fence");
        unified.FlushGpuWork();completed=unified.TakeCompletedQueries();
        Require(completed.size()==1&&completed[0].frame==7&&completed[0].generation==generation&&
            completed[0].end_event==alias_end&&completed[0].result.handle==0x1000&&
            completed[0].result.begin_address==0x2020&&completed[0].result.end_address==0x2000&&
            completed[0].result.draws==2&&completed[0].result.samples==(pass==1?0u:512u),
            "Queued alias sampling used stale bytes or changed actual query visibility");
        refresh_stats();
        Require(unified.report().at("gpu_work_queue").at("submissions")==submits_before+1,
            "Canonical conversion and both consuming draws failed to share one fence");
    }
    auto generation_case=[&](uint64_t stale_budget,bool partial_fences) {
        auto generation_config=config;generation_config.max_stale_alias_bytes=stale_budget;
        // The pressure case opts in to full provenance while the ordinary and
        // async cases exercise the lightweight live default with identical GPU checks.
        generation_config.detailed_reports=stale_budget<32;
        SceneExecutor generations(generation_config,&device,queue.get());
        auto statistics=[&] {
            (void)generations.report();
        };
        // Allocate the unchanged FSI framebuffer before measuring alias waits.
        generations.Execute(draw_operation());generations.FlushGpuWork();statistics();
        const uint64_t waits_before=generations.report().at("gpu_work_queue").at("waits");
        std::array<uint64_t,3> ends{};
        const bool pair_pressure=stale_budget<32;
        const std::array<uint32_t,6> second_fetch{2u|(1u<<22),0x3000u|38u,0,0,0,1u<<9};
        const Json second_identity{{"base","00003000"},{"format",38},{"width",1},{"height",1}};
        auto sample_alias=[&](bool pair=false) {
            const bool duplicate=skip_read_barriers&&!pair;
            auto op=draw_operation();op["pixel_shader"]=(pair||duplicate)?pair_ps_id:alias_ps_id;
            op["textures"]=Json::array();
            if(pair)op["textures"].push_back({{"slot",0},{"source","gpu_alias_or_previous_frame"},
                {"identity",second_identity},{"fetch",second_fetch}});
            op["textures"].push_back({{"slot",pair?1:0},{"source","gpu_alias_or_previous_frame"},
                {"identity",alias_identity},{"fetch",alias_fetch}});
            if(duplicate)op["textures"].push_back({{"slot",1},{"source","gpu_alias_or_previous_frame"},
                {"identity",alias_identity},{"fetch",alias_fetch}});
            generations.Execute(op);
        };
        if(pair_pressure) {
            // This image is first decoded/pinned in the final draw's slot0.
            // Preparing slot1 then pressures the old B generation budget and
            // fences before the current draw has been admitted to the FIFO.
            generations.Execute({{"type","clearrect"},{"frame",7},{"event",++event},{"flags",1},
                {"rectangle",{0,0,1,1}},{"color",{1.f,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
                {"surfaces",{{"6",fixed}}}});
            generations.Execute({{"type","resolve"},{"frame",7},{"event",++event},{"level",0},{"flags",0},
                {"source_slot",0},{"source_rectangle",{0,0,1,1}},{"destination_rectangle",{0,0,1,1}},
                {"destination",second_identity},{"destination_fetch",second_fetch},{"exponent_bias",0},
                {"copy_dest_swap",false},{"surfaces",{{"6",fixed}}}});
        }
        for(unsigned pass=0;pass<3;++pass) {
            // Only GPU commands change this allocation. The second conversion
            // must not destroy image/view/sampler generation A still used by
            // its queued draw, nor may draw A read the later physical red0.
            const float red=pass==1?0.f:1.f;
            generations.Execute({{"type","clearrect"},{"frame",7},{"event",++event},{"flags",1},
                {"rectangle",{0,0,1,1}},{"color",{red,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
                {"surfaces",{{"6",fixed}}}});
            generations.Execute({{"type","resolve"},{"frame",7},{"event",++event},{"level",0},{"flags",0},
                {"source_slot",0},{"source_rectangle",{0,0,1,1}},{"destination_rectangle",{0,0,1,1}},
                {"destination",alias_identity},{"destination_fetch",alias_fetch},{"exponent_bias",0},
                {"copy_dest_swap",false},{"surfaces",{{"6",fixed}}}});
            const uint64_t generation=400+pass;
            generations.Execute({{"type","query_begin"},{"frame",7},{"event",++event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2020}});
            sample_alias(pair_pressure&&pass==2);ends[pass]=++event;
            generations.Execute({{"type","query_end"},{"frame",7},{"event",event},
                {"query_generation",generation},{"query_handle",0x1000},{"report_address",0x2000}});
            if(partial_fences&&pass<2)
                for(unsigned i=0;i<half-4;++i)generations.Execute(draw_operation());
        }
        statistics();
        const uint64_t conversions=generations.report().at("transfer_batches").at("queued_lists");
        sample_alias();statistics();
        const auto& retained=generations.report().at("alias_generations");
        const uint64_t expected_reuse=1+(skip_read_barriers?(pair_pressure?3u:4u):0u);
        Require(retained.at("created")==(pair_pressure?4:3)&&retained.at("reused")==expected_reuse&&
            generations.report().at("transfer_batches").at("queued_lists")==conversions,
            "Generation ownership changed canonical cache-hit reuse");
        Require(generations.HasPendingGpuWork()&&generations.HasPendingDraws()&&
            generations.DeferredQueryCount()==3,"Alias versions lost pending logical query ownership");
        Reject([&]{generations.TakeCompletedQueries();},"Alias version query escaped its actual consuming fence");
        const uint64_t waits=generations.report().at("gpu_work_queue").at("waits");
        if(partial_fences) {
            Require(waits==waits_before+1&&retained.at("retired")==1&&
                retained.at("stale_generations")==1&&retained.at("stale_bindings")==1&&
                retained.at("stale_bytes")==16,"Partial fence released the later alias generation or retained the completed prefix");
        } else if(stale_budget>=32) {
            Require(waits==waits_before&&retained.at("stale_generations")==2&&
                retained.at("stale_bindings")==2&&retained.at("stale_bytes")==32,
                "GPU-only alias replacement fenced early or lost old image/view/sampler ownership");
        } else {
            Require(waits==waits_before+1&&retained.at("pressure_drains")==1&&
                retained.at("peak_stale_bytes")==16&&retained.at("stale_generations")==0,
                "Stale alias byte budget did not fence before admitting excess ownership");
        }
        // Export is also the coherent fallback boundary: every prior generation
        // must stay valid until it fences, then retire before reference handback.
        const auto before_export=generations.report().at("diagnostic_serializations").get<uint64_t>();
        if(stale_budget<32)generations.ExportState();else generations.FlushGpuWork();
        Require(generations.report().at("diagnostic_serializations")==before_export+1,
            "State export or explicit drain did not refresh diagnostic counters");
        auto results=generations.TakeCompletedQueries();Require(results.size()==3,"Alias generations lost query results");
        for(unsigned pass=0;pass<3;++pass)Require(results[pass].frame==7&&results[pass].generation==400+pass&&
            results[pass].end_event==ends[pass]&&results[pass].result.draws==1&&
            results[pass].result.samples==(pass==1?0u:256u),"GPU1/0/1 alias generations sampled stale or overwritten pixels");
        statistics();const auto& final=generations.report().at("alias_generations");
        Require(!generations.HasPendingGpuWork()&&final.at("stale_generations")==0&&
            final.at("stale_bindings")==0&&final.at("stale_bytes")==0,
            "Fenced alias generation retained stale resources");
        const auto& provenance=generations.report();
        if(generation_config.detailed_reports) {
            Require(provenance.at("texture_alias_reads").is_array()&&
                provenance.at("texture_alias_reads").size()>=3,
                "Detailed live diagnostics omitted actual alias provenance");
            for(const auto& read:provenance.at("texture_alias_reads"))
                Require(read.contains("event")&&read.contains("slot")&&read.contains("identity"),
                    "Detailed live diagnostics changed alias record fields");
        } else {
            for(const auto* field:{"texture_alias_reads","surface_alias_restores","texture_alias_restores",
                    "fixed_source_resolves","resolve_destination_swaps","mixed_cpu_gpu_textures",
                    "memory_copies","frame_outputs"})
                Require(!provenance.contains(field),"Lightweight live diagnostics retained an informational history");
        }
        Require(provenance.at("native_occlusion_results").size()==3&&
            provenance.at("gpu_texture_reads").get<uint64_t>()>=3,
            "Detailed diagnostic mode changed query results or essential counters");
        // Leave both the old consuming draw and its replacement pending for
        // Scene teardown, exercising the same final drain as terminal shutdown.
        sample_alias();
        generations.Execute({{"type","clearrect"},{"frame",7},{"event",++event},{"flags",1},
            {"rectangle",{0,0,1,1}},{"color",{0.f,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
            {"surfaces",{{"6",fixed}}}});
        generations.Execute({{"type","resolve"},{"frame",7},{"event",++event},{"level",0},{"flags",0},
            {"source_slot",0},{"source_rectangle",{0,0,1,1}},{"destination_rectangle",{0,0,1,1}},
            {"destination",alias_identity},{"destination_fetch",alias_fetch},{"exponent_bias",0},
            {"copy_dest_swap",false},{"surfaces",{{"6",fixed}}}});
        sample_alias();Require(generations.HasPendingGpuWork(),"Generation shutdown fixture drained early");
    };
    generation_case(128u*1024*1024,false);
    generation_case(16,false);
    if(asynchronous)generation_case(128u*1024*1024,true);
    if(asynchronous) {
        // Fill every physical draw/query owner, then force multiple ring wraps
        // without transfer lists masking the draw-slot capacity being tested.
        unified_begin(299);
        const uint32_t count=2*physical_capacity+3;
        for(uint32_t i=0;i<count;++i)unified.Execute(draw_operation());
        const auto capacity_end=unified_end(299);
        Require(unified.HasPendingDraws()&&unified.HasPendingGpuWork(),
            "Physical capacity fixture unexpectedly fenced its final draw");
        Reject([&]{unified.TakeCompletedQueries();},"Capacity pressure published an unfinished logical query");
        unified.FlushGpuWork();completed=unified.TakeCompletedQueries();
        Require(completed.size()==1,"Physical draw/query ring lost its logical owner");
        check(completed[0],299,capacity_end,count);
        const auto& capacity_stats=unified.report().at("gpu_work_queue");
        Require(capacity_stats.at("maximum_lists")==physical_capacity,
            "Draw-only workload did not reach the requested physical capacity");
    }
    if(asynchronous) {
        // Closed, empty and active query identities span two real in-flight
        // mixed clear/draw batches. Internal prefix retirement must not expose
        // the first closed result while any later work is still owned.
        unified_begin(300);
        for(unsigned i=0;i<half/2;++i){clear(1);unified.Execute(draw_operation());}
        const auto prefix_end=unified_end(300);
        unified_begin(301);const auto zero_end=unified_end(301);
        unified_begin(302);
        for(unsigned i=0;i<half;++i){clear(2);unified.Execute(draw_operation());}
        const auto active_metadata=unified.ActiveQueryMetadata();
        Require(active_metadata&&active_metadata->draws==half&&active_metadata->pending_draws>0,
            "Partial async fence lost the active query's accepted draws");
        const auto suffix_end=unified_end(302);
        Require(unified.DeferredQueryCount()==3&&unified.HasPendingGpuWork(),
            "Async prefix consumed closed or empty query metadata");
        Reject([&]{unified.TakeCompletedQueries();},"First async fence published queries ahead of a later batch");
        unified.FlushGpuWork();completed=unified.TakeCompletedQueries();
        Require(completed.size()==3,"Async mixed batches lost typed completions");
        check(completed[0],300,prefix_end,half/2);check(completed[1],301,zero_end,0);check(completed[2],302,suffix_end,half);
        refresh_stats();const auto& stats=unified.report().at("gpu_work_queue");
        Require(stats.at("maximum_inflight_batches")==2&&stats.at("maximum_lists").get<uint64_t>()<=physical_capacity&&
            stats.at("capacity")==physical_capacity&&stats.at("async_batch_lists")==half&&
            stats.at("maximum_scratch_bytes").get<uint64_t>()<=128u*1024*1024,
            "Async fixture did not overlap two bounded real GPU submissions");
    }
    // Leave accepted transfer work to the explicit Scene destructor drain.
    clear(3);Require(unified.HasPendingGpuWork(),"Shutdown fixture has no pending transfer");
    std::cout<<"Physical capacity "<<physical_capacity<<", asynchronous batch "<<half<<" lists: ";
    std::cout<<"PASS: scene deferred query full-drain gating, unqueried predecessor, exact GPU counts/typed metadata, internal retirement, active subtotal, configured64/128 zero-query bound, synchronous default borrowed payload retirement, unified mixed64-list pressure, transfer-only zero-query gating readback/shutdown drains, queued alias red1/0/1 sampling, retained image/view/sampler generations, bounded stale pressure and optional two-fence partial generation/query ownership\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
