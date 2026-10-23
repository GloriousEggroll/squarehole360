#include "multi_draw_scene_test.h"
#include "plume_vulkan.h"
#include <array>
#include <cstring>
#include <iostream>
#include <unordered_map>
using namespace plume;
using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> ReadEdram(RenderDevice& device,RenderCommandQueue& queue,const NativeMemoryState& state) {
    constexpr uint32_t bytes=2048*5120;
    Require(state.capacity==bytes,"Grouped fixture EDRAM extent changed");
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    const auto command=static_cast<VulkanCommandList&>(*commands).vk;
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer=state.buffer;barrier.size=VK_WHOLE_SIZE;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,1,&barrier,0,nullptr);
    const VkBufferCopy copy{0,0,bytes};
    vkCmdCopyBuffer(command,state.buffer,static_cast<VulkanBuffer&>(*readback).vk,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto mapped=readback->map();Require(mapped!=nullptr,"Grouped fixture readback mapping failed");
    auto& buffer=static_cast<VulkanBuffer&>(*readback);
    Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
        "Grouped fixture EDRAM readback invalidation failed");
    std::vector<uint8_t> result(bytes);std::memcpy(result.data(),mapped,bytes);readback->unmap();return result;
}
// Records the actual queue submission calls without replacing Vulkan command
// buffers, queries, device, descriptors or fence completion semantics.
class SubmitObserver final : public RenderCommandQueue {
    RenderCommandQueue& queue_;
public:
    uint64_t submissions=0,lists=0;
    explicit SubmitObserver(RenderCommandQueue& queue):queue_(queue){}
    std::unique_ptr<RenderCommandList> createCommandList() override {return queue_.createCommandList();}
    std::unique_ptr<RenderSwapChain> createSwapChain(const RenderSwapChainDesc& desc) override {return queue_.createSwapChain(desc);}
    void executeCommandLists(const RenderCommandList** commands,uint32_t count,RenderCommandSemaphore** waits,
        uint32_t wait_count,RenderCommandSemaphore** signals,uint32_t signal_count,RenderCommandFence* fence) override {
        ++submissions;lists+=count;queue_.executeCommandLists(commands,count,waits,wait_count,signals,signal_count,fence);
    }
    void waitForCommandFence(RenderCommandFence* fence) override {queue_.waitForCommandFence(fence);}
};
} // namespace
void CheckMultiDrawCommandBuffers(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig base,Json draw,const std::vector<uint8_t>& source_state,const std::string& alias_pixel) {
    Require(!base.multi_draw_command_buffers,"Multi-draw command-buffer default changed");
    base.defer_native_queries=base.batch_transfers=base.unified_gpu_queue=base.async_gpu_submissions=true;
    base.retire_completed_query_prefixes=true;base.max_draw_batch_size=64;
    base.skip_readonly_sample_barriers=false;base.queue_cpu_memory_writes=false;
    base.gpu_batch_attribution=base.gpu_timestamps=false;base.test_grouped_draw_before_enqueue={};
    const std::string state_id=draw.at("device_state"),texture_id(64,'a');
    struct Result {std::vector<uint8_t> edram;Json queries;};
    auto run=[&](bool grouped,bool timestamps,bool discard_dummy,bool attachmentless=false) {
        auto config=base;config.multi_draw_command_buffers=grouped;config.gpu_draw_timestamps=timestamps;
        config.discard_dummy_fsi_attachment=discard_dummy;config.attachmentless_fsi=attachmentless;
        std::unordered_map<std::string,OwnedPayload::Snapshot> bank;
        bank[state_id]=std::make_shared<const std::vector<uint8_t>>(source_state);
        config.payload={};config.payload_snapshot=[&](const std::string& id) {
            const auto found=bank.find(id);Require(found!=bank.end(),"Grouped fixture payload missing");return found->second;
        };
        SceneExecutor scene(config,&device,&queue);uint64_t event=0;
        const std::vector<uint8_t> seed(2048*5120,0xcd);scene.InitializeEdram(seed);
        scene.InitializeMemory(0x5000,std::vector<uint8_t>(4096));
        auto record=[&](Json operation) {
            operation["event"]=++event;operation["frame"]=16;
            // Match RuntimeState::ExecuteOperation, including its per-draw
            // committed-state flush. A clean flush must not seal this group.
            scene.FlushCommittedState();scene.Execute(operation);
        };
        auto begin=[&](uint64_t generation) {record({{"type","query_begin"},{"query_generation",generation},
            {"query_handle",0x1000},{"report_address",0x2020}});};
        auto end=[&](uint64_t generation) {record({{"type","query_end"},{"query_generation",generation},
            {"query_handle",0x1000},{"report_address",0x2000}});};
        // All resize and pipeline creation work is fenced before the long run.
        record(draw);scene.FlushGpuWork();
        const uint32_t draws=2*config.async_submission_capacity+3;
        begin(1600);for(uint32_t i=0;i<draws;++i)record(draw);end(1600);
        // A genuine empty query follows an accepted grouped tail, before full
        // completion; its required logical serial includes those unrelated draws.
        begin(1601);end(1601);
        Require(scene.TakeCompletedQueries().empty(),
            "Grouped or empty query published ahead of its still-staged three-draw tail");
        const std::array<uint32_t,6> fetch{2u|(1u<<22),0x5000u|38u,0,0,0,1u<<9};
        const Json identity{{"base","00005000"},{"format",38},{"width",1},{"height",1}};
        for(uint32_t phase=0;phase<3;++phase) {
            std::vector<uint8_t> bytes(4096,uint8_t(phase+0x51));
            const std::array<float,4> pixel{phase==1?0.f:1.f,0,0,1};std::memcpy(bytes.data(),pixel.data(),16);
            auto owner=std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
            std::weak_ptr<const std::vector<uint8_t>> weak=owner;bank[texture_id]=owner;
            record({{"type","cpu_memory_write"},{"address",0x5000},{"bytes",4096},{"payload",texture_id},
                {"coherence_epoch",uint64_t(phase+1)}});
            bank.erase(texture_id);owner.reset();Require(weak.expired(),"Grouped update retained borrowed payload");
            begin(1602+phase);
            auto sample=draw;sample["pixel_shader"]=alias_pixel;
            sample["textures"]=Json::array({{{"slot",0},{"source","gpu_alias_or_previous_frame"},
                {"identity",identity},{"fetch",fetch}}});
            // First draw uses a fresh transition/standalone buffer; the two
            // exact immutable read generations can share the following buffer.
            for(uint32_t i=0;i<3;++i)record(sample);
            end(1602+phase);
        }
        scene.FlushGpuWork();const auto completed=scene.TakeCompletedQueries();
        Require(completed.size()==5,"Grouped draw/query logical generations were lost");
        Json oracle=Json::array();
        for(uint32_t i=0;i<completed.size();++i) {
            const uint64_t expected_draws=i==0?draws:i==1?0:3;
            const uint64_t expected_samples=i==3?0:expected_draws*256;
            const auto& query=completed[i];
            Require(query.frame==16&&query.generation==1600+i&&query.result.draws==expected_draws&&
                query.result.samples==expected_samples,"Grouped command buffers changed actual samples/identity");
            oracle.push_back({query.frame,query.generation,query.end_event,query.result.handle,
                query.result.begin_address,query.result.end_address,query.result.draws,query.result.samples});
        }
        const auto memory=scene.ReadPhysical(0x5000,16);
        const std::array<float,4> expected_memory{1,0,0,1};
        Require(memory.size()==16&&std::memcmp(memory.data(),expected_memory.data(),16)==0,
            "Grouped readback did not observe newest same-address CPU update");
        auto edram=ReadEdram(device,queue,scene.ExportState().edram);
        Require(edram==seed,"Grouped query shaders changed unrelated packed EDRAM bytes");
        const auto& report=scene.report();const auto& groups=report.at("draw_command_groups");
        const auto& work=report.at("gpu_work_queue");
        Require(work.at("accepted_work")==work.at("completed_work")&&!scene.HasPendingGpuWork(),
            "Grouped full drain left a logical owner outstanding");
        Require(work.at("maximum_lists").get<uint64_t>()<=config.async_submission_capacity&&
            work.at("maximum_scratch_bytes").get<uint64_t>()<=128ull*1024*1024,
            "Grouped physical lists widened logical/scratch bounds");
        if(grouped)Require(groups.at("maximum_group_draws")==16&&
            groups.at("logical_grouped_draws").get<uint64_t>()>groups.at("closed_groups").get<uint64_t>()&&
            groups.at("physical_lists").get<uint64_t>()<work.at("draw_lists").get<uint64_t>()+work.at("transfer_lists").get<uint64_t>()&&
            groups.at("standalone_preparation_draws").get<uint64_t>()>=3&&
            groups.at("readonly_barriers_deferred").get<uint64_t>()>=6&&groups.at("readonly_state_changed")==0,
            "Grouped fixture coverage is vacuous or skipped transition compatibility path");
        if(discard_dummy&&!attachmentless) {
            const auto& dummy=report.at("dummy_fsi_render_pass");
            Require(dummy.at("installs").get<uint64_t>()>0&&dummy.at("refusals")==0&&
                dummy.at("selected_draws")==work.at("draw_lists")&&
                dummy.at("queried_draws")==uint64_t(draws)+9,
                "Dummy discard grouped/standalone actual accepted pass/query coverage is vacuous");
            std::cout<<"DUMMY_FSI_GROUP_COVERAGE "<<dummy.dump()<<'\n';
        }
        if(attachmentless) {
            const auto& candidate=report.at("attachmentless_fsi_render_pass");
            Require(candidate.at("supported")==true&&candidate.at("active")==true&&candidate.at("refused")==false&&
                candidate.at("installs").get<uint64_t>()>0&&candidate.at("refusals")==0&&
                candidate.at("selected_draws")==work.at("draw_lists")&&candidate.at("queried_draws")==uint64_t(draws)+9&&
                candidate.at("barrier_draws").get<uint64_t>()>0&&report.at("fsi_raster_attachment").at("creations")==0&&
                report.at("dummy_fsi_render_pass").at("installs")==0,
                "Attachmentless grouped/standalone actual framebuffer/query/barrier coverage is vacuous");
            std::cout<<"ATTACHMENTLESS_FSI_GROUP_COVERAGE "<<candidate.dump()<<'\n';
        }
        if(timestamps) {
            const auto timing=scene.GpuDrawTimingReport();
            if(timing.value("supported",false))Require(timing.at("accepted")==timing.at("completed")&&
                timing.at("pending")==false&&!timing.value("incomplete",false),
                "Grouped inline timestamp ownership did not retire exact children");
        }
        std::cout<<"MULTIDRAW_GPU_COVERAGE "<<Json{{"enabled",grouped},{"inline_timestamps",timestamps},
            {"capacity",config.async_submission_capacity},{"groups",groups},{"logical_draws",work.at("draw_lists")},
            {"actual_samples",oracle}}.dump()<<'\n';
        // Normal destructor completion must close an accepted unsubmitted group.
        record(draw);record(draw);Require(scene.HasPendingDraws(),"Grouped shutdown tail completed too early");
        return Result{std::move(edram),std::move(oracle)};
    };
    const auto baseline=run(false,false,false),grouped=run(true,false,base.discard_dummy_fsi_attachment,base.attachmentless_fsi),
        timed=run(true,true,base.discard_dummy_fsi_attachment,base.attachmentless_fsi);
    if(base.discard_dummy_fsi_attachment) {
        const auto standalone_discard=run(false,false,true);
        Require(baseline.edram==standalone_discard.edram&&baseline.queries==standalone_discard.queries,
            "Dummy discard without grouping changed actual standalone query/EDRAM output");
    }
    if(base.attachmentless_fsi) {
        const auto standalone_empty=run(false,false,false,true);
        Require(baseline.edram==standalone_empty.edram&&baseline.queries==standalone_empty.queries,
            "Attachmentless standalone framebuffer changed actual full EDRAM/query ownership");
    }
    Require(baseline.edram==grouped.edram&&baseline.queries==grouped.queries&&
        baseline.edram==timed.edram&&baseline.queries==timed.queries,
        "Grouped/standalone/inline-timestamp actual query/EDRAM parity failed");
    std::cout<<"PASS: real multi-child physical buffers, logical64/128 pressure, mixed immutable/standalone texture transitions, same-address1/0/1 aliases, exact query/empty-prefix and full10MiB EDRAM parity\n";
}
void CheckMultiDrawFollowerFailure(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig config,Json draw) {
    config.defer_native_queries=config.retire_completed_query_prefixes=config.batch_transfers=
        config.unified_gpu_queue=config.async_gpu_submissions=config.multi_draw_command_buffers=true;
    config.max_draw_batch_size=16;config.gpu_timestamps=config.gpu_batch_attribution=config.gpu_draw_timestamps=false;
    config.reset_draw_command_pool=false;
    uint32_t observed_older=UINT32_MAX;bool inject=false;
    config.test_grouped_draw_before_enqueue=[&](uint32_t older) {
        if(inject&&older==1) {observed_older=older;throw std::runtime_error("Injected fully recorded unaccepted follower failure");}
    };
    SubmitObserver observer(queue);
    SceneExecutor scene(config,&device,&observer);uint64_t event=0;
    auto record=[&](Json operation) {
            operation["event"]=++event;operation["frame"]=16;
            // Match RuntimeState::ExecuteOperation, including its per-draw
            // committed-state flush. A clean flush must not seal this group.
            scene.FlushCommittedState();scene.Execute(operation);
        };
    scene.InitializeEdram(std::vector<uint8_t>(2048*5120));record(draw);scene.FlushGpuWork();
    bool dummy_discard_installed=false,attachmentless_installed=false;
    if(config.discard_dummy_fsi_attachment&&!config.attachmentless_fsi) {
        // Observe only the fenced warmup, before the accepted/unaccepted group
        // exists. Observing the later first child would invalidate this test.
        const auto& dummy=scene.report().at("dummy_fsi_render_pass");
        Require(dummy.at("installs").get<uint64_t>()>0&&dummy.at("selected_draws").get<uint64_t>()>0&&
            dummy.at("refusals")==0,"Fatal fixture did not own an actual discard pass");
        dummy_discard_installed=true;
    }
    if(config.attachmentless_fsi) {
        const auto& candidate=scene.report().at("attachmentless_fsi_render_pass");
        Require(candidate.at("active")==true&&candidate.at("refused")==false&&candidate.at("installs").get<uint64_t>()>0&&
            candidate.at("selected_draws").get<uint64_t>()>0&&candidate.at("refusals")==0&&
            scene.report().at("fsi_raster_attachment").at("creations")==0,
            "Fatal follower fixture did not own an actual attachmentless framebuffer before failure");
        attachmentless_installed=true;
    }
    const uint64_t submits_before=observer.submissions;
    record({{"type","query_begin"},{"query_generation",1900},{"query_handle",0x1000},{"report_address",0x2020}});
    inject=true;record(draw);
    bool fatal=false;try {record(draw);}catch(const NativeBatchCompletionError&){fatal=true;}
    Require(fatal&&observed_older==1&&scene.HasPendingDraws()&&scene.HasPendingGpuWork(),
        "Follower fault did not poison a previously accepted physical leader");
    Json refused=Json::array();
    auto refuse=[&](const char* name,auto&& operation) {
        bool rejected=false;try {operation();}catch(const NativeBatchCompletionError&){rejected=true;}
        Require(rejected,"Poisoned physical leader accepted a post-failure operation");refused.push_back(name);
        Require(observer.submissions==submits_before,"Poisoned physical leader was submitted during refusal/unwind");
    };
    refuse("draw",[&]{record(draw);});
    refuse("query_end",[&]{record({{"type","query_end"},{"query_generation",1900},{"query_handle",0x1000},{"report_address",0x2000}});});
    refuse("flush",[&]{scene.FlushGpuWork();});
    refuse("submit_progress",[&]{scene.SubmitGpuWork();});
    refuse("export_state",[&]{(void)scene.ExportState();});
    refuse("coherence",[&]{scene.FlushCommittedState();});
    refuse("readback",[&]{(void)scene.ReadPhysical(0,16);});
    refuse("query_take",[&]{(void)scene.TakeCompletedQueries();});
    refuse("query_poll",[&]{scene.PollCompletedQueries();});
    refuse("query_pressure",[&]{scene.FlushOldestQuery();});
    refuse("active_query",[&]{(void)scene.ActiveQuery();});
    refuse("report",[&]{(void)scene.report();});
    refuse("batch_report",[&]{(void)scene.GpuBatchAttributionReport();});
    refuse("draw_report",[&]{(void)scene.GpuDrawTimingReport();});
    std::cout<<"MULTIDRAW_FATAL_PROOF "<<Json{{"older_accepted_children",observed_older},
        {"submissions_since_first_draw",observer.submissions-submits_before},
        {"has_pending_owners",scene.HasPendingDraws()&&scene.HasPendingGpuWork()},
        {"refused_paths",refused},{"expected_scene_destructor_termination",true},
        {"dummy_discard_requested",config.discard_dummy_fsi_attachment},
        {"dummy_discard_installed",dummy_discard_installed},{"attachmentless_requested",config.attachmentless_fsi},
        {"attachmentless_installed",attachmentless_installed}}.dump()<<'\n'<<std::flush;
    // This scope intentionally reaches the real fatal destructor. Do not mask
    // it with _Exit, an ownership leak, a synthetic drain or an extra GPU wait.
}
