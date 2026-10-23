#include "gpu_helpers.h"
#include "renderer/native/pass_transfer.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/transfer_batch.h"
#include "renderer/native/gpu_work_queue.h"
#include "renderer/native/live_device.h"
#include <mutex>
#include <nlohmann/json.hpp>
#include <future>
#include <semaphore>
#include <bit>
#include <cstdlib>
#include <exception>
#include "fullscreen_spv.h"
#include "resolve_spv.h"
#include "restore_depth_spv.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include <iostream>
#include <string_view>
using namespace plume;
using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::array<float,4> FirstPixel(RenderDevice& device,RenderCommandQueue& queue,RenderTexture& image) {
    const auto bytes=validation::ReadImage(device,queue,image,16);
    std::array<float,4> result;std::memcpy(result.data(),bytes.data(),sizeof(result));return result;
}
}
namespace {
// Small retained packets isolate FIFO/fence generation ownership independently
// of GPU execution speed. The physical capacity and scratch budget stay fixed.
void CheckFourFenceTransfers(RenderDevice& device,RenderCommandQueue& queue,uint32_t capacity) {
    struct Lease {std::unique_ptr<RenderCommandList> commands;};
    const uint32_t quarter=capacity/4,total=capacity+quarter+1;
    std::vector<std::weak_ptr<Lease>> owners;owners.reserve(total);
    std::vector<NativeGpuWorkQueue::Completion> completions;completions.reserve(6);
    NativeGpuWorkQueue work(device,queue,[&](const auto& completed) {
        Require(completed.submission==completions.size()+1&&completed.draws==0,
            "Four-fence transfer retirement changed FIFO generation");
        completions.push_back(completed);
    },true,true,capacity,true,4);
    const auto enqueue=[&](uint64_t event,uint64_t bytes) {
        work.Reserve(bytes);auto lease=std::make_shared<Lease>();owners.push_back(lease);
        lease->commands=queue.createCommandList();lease->commands->begin();lease->commands->end();
        work.EnqueueTransfer(*lease->commands,lease,bytes,event);
    };
    for(uint32_t i=0;i<capacity;++i) {
        enqueue(i+1,4096);
        if((i+1)%quarter==0)work.SubmitPending();
    }
    Require(work.inflight_batch_limit()==4&&work.inflight_batches()==4&&work.pending_lists()==capacity&&
        !work.statistics().waits&&work.statistics().maximum_inflight_batches==4,
        "Four-fence submission prematurely waited or exceeded same physical capacity");
    for(const auto& owner:owners)Require(!owner.expired(),"Four-fence packet owner released before its actual retirement");
    work.Reserve();
    Require(work.statistics().waits==1&&work.pending_lists()==capacity-quarter&&completions.size()==1&&
        completions[0].transfers==quarter&&completions[0].last_event==quarter,
        "Four-fence capacity pressure did not retire only oldest covering generation");
    for(uint32_t i=0;i<capacity;++i)Require(owners[i].expired()==(i<quarter),
        "Four-fence pressure retired another generation's packet lease");
    for(uint32_t i=capacity;i<capacity+quarter;++i)enqueue(i+1,4096);
    work.SubmitPending(); // Generation5 reuses only the retired first fence slot.
    enqueue(total,4096);  // Newest tail is deliberately unsubmitted.
    const auto submits=work.statistics().submissions;
    work.WaitThrough(capacity);
    Require(work.statistics().submissions==submits&&work.completed_work()==capacity&&
        work.pending_lists()==quarter+1&&completions.size()==4&&work.inflight_batches()==1,
        "Four-fence frontier wait submitted/retired unrelated later work");
    for(uint32_t i=0;i<owners.size();++i)Require(owners[i].expired()==(i<capacity),
        "Four-fence frontier released later/staged packet owner");
    work.Drain();Require(completions.size()==6&&work.accepted_work()==total&&work.completed_work()==total&&!work.pending(),
        "Four-fence final drain lost accepted generation/tail");
    for(const auto& owner:owners)Require(owner.expired(),"Four-fence completed lease was retained");
    Require(work.statistics().maximum_lists<=capacity&&work.statistics().maximum_scratch_bytes<=NativeGpuWorkQueue::kMaxScratchBytes,
        "Four-fence transfer widened ownership budgets");
    if(work.statistics().timestamps_supported)Require(work.statistics().gpu_intervals==6&&!work.statistics().gpu_unavailable,
        "Four-fence reused timestamp slots lost or exposed unfenced results");
    Require(work.attribution()->totals().batches==6,"Four-fence attribution missed wrapped fence generations");
    uint64_t submits_sum=0,lists_sum=0,waits_sum=0;
    for(const auto& reason:work.statistics().submission_reasons){submits_sum+=reason.submissions;lists_sum+=reason.lists;}
    for(const auto count:work.statistics().wait_reasons)waits_sum+=count;
    Require(submits_sum==work.statistics().submissions&&lists_sum==total&&waits_sum==work.statistics().waits,
        "Four-fence reasons failed submission/list/wait conservation");

    // Deliberate conservative byte charges exercise pressure without allocating
    // another128MiB. A fourth in-flight fence and staged fifth prefix coexist.
    NativeGpuWorkQueue pressure(device,queue,{},true,true,capacity,false,4);
    std::array<std::weak_ptr<Lease>,5> scratch;
    const auto packet=[&](uint32_t i,uint64_t bytes) {
        pressure.Reserve(bytes);auto lease=std::make_shared<Lease>();scratch[i]=lease;
        lease->commands=queue.createCommandList();lease->commands->begin();lease->commands->end();
        pressure.EnqueueTransfer(*lease->commands,lease,bytes,i+1);
    };
    packet(0,64ull*1024*1024);pressure.SubmitPending();
    for(uint32_t i=1;i<4;++i){packet(i,16ull*1024*1024);pressure.SubmitPending();}
    packet(4,16ull*1024*1024);pressure.Reserve(64ull*1024*1024);
    Require(pressure.statistics().waits==1&&pressure.inflight_batches()==4&&pressure.pending_lists()==4&&scratch[0].expired(),
        "Four-fence scratch pressure waited beyond enough oldest ownership");
    for(uint32_t i=1;i<scratch.size();++i)Require(!scratch[i].expired(),
        "Four-fence scratch pressure dropped newer accepted ownership");
    pressure.Drain();for(const auto& owner:scratch)Require(owner.expired(),"Four-fence scratch tail survived final drain");
    Require(pressure.statistics().maximum_scratch_bytes<=NativeGpuWorkQueue::kMaxScratchBytes,
        "Four-fence scratch accounting exceeded128MiB");
    for(uint32_t invalid:{0u,1u,3u,5u,UINT32_MAX}) {
        bool rejected=false;
        try {NativeGpuWorkQueue bad(device,queue,{},true,false,capacity,false,invalid);}
        catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"Unsupported native fence window was silently accepted");
    }
    std::cout<<"PASS: four realfence generations, oldest-only64/128list pressure, exact lease/acceptedfrontier retirement, wrappedtimestamps/attribution, stagedtail,128MiBscratch and numeric reasonconservation\n";
}
}

namespace {
// Same throwing completion contract as the live SharedQueue. This fixture uses
// real command buffers, submissions, fences, and the actual queue-owner mutex.
class CheckedCpuWriteQueue final : public VulkanCommandQueue,public NativeCheckedQueueCompletion {
public:
    uint64_t submissions=0,waits=0;
    bool collect_commands=false;std::vector<uint32_t> submitted_counts;
    bool inject_after_submit=false,inject_before_wait=false;
    std::function<void()> after_submit;
    RenderCommandFence* last_fence=nullptr;
    explicit CheckedCpuWriteQueue(VulkanDevice* device):VulkanCommandQueue(device,RenderCommandListType::DIRECT){}
    void executeCommandLists(const RenderCommandList** lists,uint32_t count,RenderCommandSemaphore**,
        uint32_t wait_count,RenderCommandSemaphore**,uint32_t signal_count,RenderCommandFence* fence) override {
        Require(lists&&count&&!wait_count&&!signal_count&&fence,"Unexpected fixture queue submission shape");
        std::vector<VkCommandBuffer> handles;handles.reserve(count);
        for(uint32_t i=0;i<count;++i) {
            auto* commands=dynamic_cast<const VulkanCommandList*>(lists[i]);
            Require(commands&&commands->queue->device==device,"Foreign fixture command buffer");
            handles.push_back(commands->vk);
        }
        const auto* completion=dynamic_cast<const VulkanCommandFence*>(fence);
        Require(completion&&completion->device==device,"Foreign fixture submit fence");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=count;info.pCommandBuffers=handles.data();
        std::scoped_lock lock(*queue->mutex);
        Require(vkQueueSubmit(queue->vk,1,&info,completion->vk)==VK_SUCCESS,"Checked fixture submit failed");
        ++submissions;last_fence=fence;
        if(collect_commands)submitted_counts.push_back(count);
        if(after_submit)after_submit();
        if(inject_after_submit) {inject_after_submit=false;throw std::runtime_error("Injected after actual submit");}
    }
    void waitForCommandFence(RenderCommandFence* fence) override {
        if(inject_before_wait) {inject_before_wait=false;throw std::runtime_error("Injected before actual wait");}
        const auto* completion=dynamic_cast<const VulkanCommandFence*>(fence);
        Require(completion&&completion->device==device,"Foreign fixture wait fence");
        Require(vkWaitForFences(device->vk,1,&completion->vk,VK_TRUE,UINT64_MAX)==VK_SUCCESS,"Checked fixture wait failed");
        Require(vkResetFences(device->vk,1,&completion->vk)==VK_SUCCESS,"Checked fixture reset failed");
        ++waits;
    }
};
void CheckSubmissionWorker(RenderDevice& device,RenderCommandQueue& unchecked,uint32_t capacity,bool physical=false) {
    auto& native=static_cast<VulkanDevice&>(device);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Worker fixture requires hardware Vulkan");
    bool rejected=false;try{NativeGpuWorkQueue invalid(device,unchecked,{},true,false,capacity,false,2,true);}
    catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"Unchecked backend admitted worker");
    for(const auto fences:{2u,4u})for(const bool timing:{false,true}) {
        auto run=[&](bool enabled) {
            CheckedCpuWriteQueue queue(&native);queue.collect_commands=physical;queue.submitted_counts.reserve(capacity*3+16);
            std::vector<uint32_t> retired_lists;
            std::promise<void> signaled;auto ready=signaled.get_future();std::binary_semaphore release(0);
            if(enabled)queue.after_submit=[&]{if(queue.submissions==1) {
                const auto& fence=static_cast<const VulkanCommandFence&>(*queue.last_fence);
                Require(vkWaitForFences(native.vk,1,&fence.vk,VK_TRUE,UINT64_MAX)==VK_SUCCESS,"Held fixture fence failed");
                signaled.set_value();release.acquire();
            }};
            struct Packet {std::unique_ptr<RenderCommandList> commands;};
            std::vector<std::weak_ptr<Packet>> owners;
            uint64_t retired=0;
            NativeGpuWorkQueue work(device,queue,[&](const auto& done) {
                Require(done.submission==++retired,"Worker retired out of order");
                if(physical)retired_lists.push_back(done.transfers+done.draws);
            },true,timing,capacity,timing,fences,enabled,physical,0,6000,30);
            auto bytes=device.createBuffer(RenderBufferDesc::DefaultBuffer(4096,RenderBufferFlag::STORAGE));
            const auto buffer=static_cast<VulkanBuffer&>(*bytes).vk;
            std::array<uint32_t,1024> expected{};
            const auto enqueue=[&](uint32_t value) {
                work.Reserve();auto packet=std::make_shared<Packet>();owners.push_back(packet);
                packet->commands=queue.createCommandList();packet->commands->begin();
                auto& commands=static_cast<VulkanCommandList&>(*packet->commands);
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(commands.vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                if(value==1) {
                    vkCmdFillBuffer(commands.vk,buffer,0,4096,0);
                    vkCmdPipelineBarrier(commands.vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                }
                vkCmdFillBuffer(commands.vk,buffer,0,4,value);
                vkCmdFillBuffer(commands.vk,buffer,value*4,4,value);
                packet->commands->end();NativeGpuWorkTag tag;tag.kind=NativeGpuWorkKind::Clear;
                tag.event=value;tag.frame=value==1?31:value%7==0?UINT32_MAX:value%3==0?31:30;
                work.EnqueueTransfer(*packet->commands,packet,4096,value,physical?&tag:nullptr);
                expected[0]=expected[value]=value;
            };
            enqueue(1);work.SubmitPending();
            if(enabled) {
                Require(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready,"Worker did not signal actual GPU fence");
                work.PollCompleted();
                Require(!owners[0].expired()&&work.completed_work()==0&&!queue.waits,
                    "GPU completed before driver receipt but ownership/fence was recycled");
                release.release();
            }
            // Mix explicit submits, pressure, nonblocking polls, prefix waits,
            // and fence-ring wrapping while immutable command owners are live.
            for(uint32_t i=2;i<=capacity*3+1;++i) {
                enqueue(i);
                if(i%(physical?capacity/2:16)==0)work.SubmitPending();
                if(i%13==0)work.PollCompleted();
                if(i==capacity)work.WaitThrough(capacity/2);
            }
            work.Drain();
            for(const auto& owner:owners)Require(owner.expired(),"Worker drain retained finished packet");
            const auto& stats=work.statistics();
            Require(!work.pending()&&work.accepted_work()==work.completed_work()&&stats.maximum_lists<=capacity&&
                stats.maximum_inflight_batches<=fences&&stats.submission_worker_enabled==enabled&&
                (!enabled||stats.driver_receipts==stats.submissions),"Worker capacity/receipt accounting failed");
            if(timing)Require(stats.gpu_intervals==stats.submissions&&stats.gpu_unavailable==0,"Worker timestamp generation was lost");
            if(physical) {
                const auto report=work.PhysicalTimingReport(true);
                Require(report.value("supported",false)&&report.value("retention_complete",false)&&
                    report.at("invalid")==0&&report.at("unavailable")==0&&
                    report.at("prepared_batches").get<uint64_t>()>0&&report.at("skipped_batches").get<uint64_t>()>0,
                    "Physical worker timer unavailable, incomplete or vacuous");
                std::vector<uint32_t> samples(stats.submissions);
                for(const auto& r:report.at("samples")) {
                    const auto gen=r.at("submission").get<uint64_t>();
                    Require(gen&&gen<=samples.size()&&r.at("children").size()==1&&
                        !r.at("children").at(0).at("draw").get<bool>(),"Transfer timer mapped the wrong owner");
                    ++samples[gen-1];
                }
                Require(retired_lists.size()==stats.submissions&&queue.submitted_counts.size()==stats.submissions,
                    "Physical timer introduced another execute/retirement");
                for(size_t i=0;i<samples.size();++i) {
                    Require(!samples[i]||samples[i]==retired_lists[i],"Partial physical submission was measured");
                    Require(queue.submitted_counts[i]==retired_lists[i]+(samples[i]?samples[i]+1:0)+(timing?2:0),
                        "Unsampled submission has markers or outer markers were lost");
                }
                Require(stats.maximum_physical_lists_per_submission==capacity/2,"Fixture missed maximum asynchronous staging size");
            }
            auto result=device.createBuffer(RenderBufferDesc::ReadbackBuffer(4096));auto commands=queue.createCommandList();
            auto fence=device.createCommandFence();commands->begin();
            commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes.get(),RenderBufferAccess::READ));
            if(physical) {
                VkMemoryBarrier producer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                producer.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;producer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&producer,0,nullptr,0,nullptr);
            }
            commands->copyBufferRegion(RenderBufferReference(result.get()),RenderBufferReference(bytes.get()),4096);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            commands->end();const RenderCommandList* list=commands.get();queue.executeCommandLists(&list,1,nullptr,0,nullptr,0,fence.get());queue.waitForCommandFence(fence.get());
            auto* mapped=result->map();const auto& rb=static_cast<const VulkanBuffer&>(*result);
            Require(vmaInvalidateAllocation(native.allocator,rb.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Worker readback invalidate failed");
            Require(std::memcmp(mapped,expected.data(),4096)==0,"Worker changed ordered GPU bytes");result->unmap();
        };
        run(false);run(true);
    }
    std::cout<<"SUBMISSION_WORKER_PASS capacity="<<capacity<<" fence_windows=2,4 timestamps=off,on compared_bytes=32768 held_signaled_fence=true bounded_fifo=true receipts_before_reset=true\n";
}

// Queue-level ownership/mapping proof, using ordered buffer writes inside
// logical draw groups. Full Scene/render-pass parity is a separate GPU gate.
void CheckPhysicalGroups(RenderDevice& device,uint32_t capacity) {
    auto& native=static_cast<VulkanDevice&>(device);
    for(bool worker:{false,true})for(bool outer:{false,true}) {
        CheckedCpuWriteQueue queue(&native);queue.collect_commands=true;queue.submitted_counts.reserve(32);
        struct Packet {std::unique_ptr<RenderCommandList> command;};
        std::vector<std::shared_ptr<Packet>> group_owners;
        std::vector<NativeGpuWorkQueue::Completion> retired;
        auto bytes=device.createBuffer(RenderBufferDesc::DefaultBuffer(4096,RenderBufferFlag::STORAGE));
        const auto buffer=static_cast<VulkanBuffer&>(*bytes).vk;
        std::array<uint32_t,1024> expected{};
        NativeGpuWorkQueue work(device,queue,[&](const auto& c){retired.push_back(c);},
            true,outer,capacity,false,4,worker,true,0,6000,30);
        RenderCommandList* open=nullptr;
        work.ConfigureDrawCommandGroups([&] {
            Require(open,"Group timer fixture lost natural close owner");
            open->end();work.ClosePinnedDrawGroup(*open);open=nullptr;
        });
        uint64_t token=1,event=1;
        const uint32_t logical_per_batch=capacity/2,physical_per_batch=logical_per_batch-15;
        for(uint32_t batch=0;batch<6;++batch) {
            work.Reserve();auto leader=std::make_shared<Packet>();leader->command=queue.createCommandList();
            group_owners.push_back(leader);open=leader->command.get();open->begin();
            if(batch==0) {
                vkCmdFillBuffer(static_cast<VulkanCommandList&>(*open).vk,buffer,0,4096,0);
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*open).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            }
            for(uint32_t i=0;i<16;++i) {
                work.Reserve();const uint32_t index=batch*logical_per_batch+i;
                vkCmdFillBuffer(static_cast<VulkanCommandList&>(*open).vk,buffer,index*4,4,index+1);
                expected[index]=index+1;
                NativeGpuWorkTag tag;tag.kind=NativeGpuWorkKind::Draw;tag.event=event;
                tag.frame=batch==0?31:i==1?30:i==2?UINT32_MAX:32;
                tag.vertex_variant=10;tag.pixel_variant=i%2?20:21;tag.cohort=5;tag.cohort_known=true;
                tag.fsi_state_specialized=bool(i%2);
                work.EnqueuePinnedDrawInGroup(*open,event++,token++,&tag);
            }
            for(uint32_t i=16;i<logical_per_batch;++i) {
                work.Reserve();auto packet=std::make_shared<Packet>();packet->command=queue.createCommandList();
                packet->command->begin();const uint32_t index=batch*logical_per_batch+i;
                vkCmdFillBuffer(static_cast<VulkanCommandList&>(*packet->command).vk,buffer,index*4,4,index+1);
                expected[index]=index+1;packet->command->end();
                NativeGpuWorkTag tag;tag.kind=NativeGpuWorkKind::Clear;tag.event=event;
                tag.frame=batch==0?31:32;
                work.EnqueueTransfer(*packet->command,packet,0,event++,&tag);
            }
            work.SubmitPending();
        }
        work.Drain();
        const auto report=work.PhysicalTimingReport(true);const auto& stats=work.statistics();
        Require(report.value("supported",false)&&report.value("retention_complete",false)&&
            report.at("invalid")==0&&report.at("unavailable")==0,
            "Grouped physical timer lost fence generations/availability");
        Require(stats.submissions==6&&stats.physical_lists==6*physical_per_batch&&
            stats.grouped_draws==96&&stats.draw_command_groups==6&&stats.maximum_group_draws==16&&
            work.accepted_work()==6*logical_per_batch&&work.completed_work()==work.accepted_work(),
            "Physical markers changed grouped FIFO work plan");
        Require(report.at("prepared_batches")==5&&report.at("skipped_batches")==1&&
            report.at("records")==5*physical_per_batch&&report.at("available")==5*physical_per_batch,
            "Grouped sampling omitted a mixed/unknown child or sampled wrong batch");
        Require(queue.submitted_counts.size()==6&&retired.size()==6,"Diagnostic changed original execute count");
        uint32_t group_records=0;
        for(const auto& r:report.at("samples")) {
            const auto generation=r.at("submission").get<uint64_t>();
            Require(generation>=2&&generation<=6&&r.at("selected_api_frame")==30,"Wrong sampled group generation/frame");
            if(r.at("group").get<uint64_t>()) {
                ++group_records;Require(r.at("children").size()==16&&!r.at("all_frames_known").get<bool>(),
                    "Physical group did not retain all sixteen mixed/unknown children");
                for(size_t i=0;i<16;++i) {
                    const auto& c=r.at("children").at(i);
                    Require(c.at("pixel_variant")==uint32_t(i%2?20:21)&&c.at("cohort")==5&&
                        c.at("fsi_state_specialized")==bool(i%2),"Final cohort metadata lost during group timing");
                }
            }
        }
        Require(group_records==5,"Grouped GPU proof was vacuous");
        for(uint32_t i=0;i<6;++i) {
            Require(retired[i].submission==i+1&&retired[i].draws==16&&retired[i].transfers==logical_per_batch-16,
                "Group follower retirement changed original logical prefix");
            const auto expected_commands=physical_per_batch+(i?physical_per_batch+1:0)+(outer?2:0);
            Require(queue.submitted_counts[i]==expected_commands,"Off-sample markers or lost outer timestamp wrapper");
        }
        if(outer)Require(stats.gpu_intervals==6&&!stats.gpu_unavailable,"Outer timestamps failed with physical markers");
        auto result=device.createBuffer(RenderBufferDesc::ReadbackBuffer(4096));auto commands=queue.createCommandList();
        auto fence=device.createCommandFence();commands->begin();
        commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes.get(),RenderBufferAccess::READ));
        // Fenced raw fills still require visibility to this device-side copy.
        // Plume's producer-stage tracking was bypassed by vkCmdFillBuffer.
        VkMemoryBarrier producer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        producer.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;producer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&producer,0,nullptr,0,nullptr);
        commands->copyBufferRegion(RenderBufferReference(result.get()),RenderBufferReference(bytes.get()),4096);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        commands->end();const RenderCommandList* list=commands.get();
        queue.executeCommandLists(&list,1,nullptr,0,nullptr,0,fence.get());queue.waitForCommandFence(fence.get());
        const auto* mapped=result->map();const auto& rb=static_cast<const VulkanBuffer&>(*result);
        Require(vmaInvalidateAllocation(native.allocator,rb.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Group readback invalidation");
        Require(std::memcmp(mapped,expected.data(),4096)==0,"Group timer altered ordered physical GPU writes");result->unmap();
    }
    std::cout<<"PHYSICAL_GROUPS_PASS capacity="<<capacity<<" group_children=16 batches=6 worker=off,on outer_timestamps=off,on mixed_unknown_frames=true exact_execute_counts=true\n";
}

void CheckCpuWriteUploadReuse(RenderDevice& device,RenderCommandQueue& unchecked,uint32_t capacity) {
    auto& native=static_cast<VulkanDevice&>(device);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"CPU-write upload fixture requires hardware Vulkan");
    constexpr uint32_t kBytes=64*1024,kPage=4096,kWrites=96;
    struct Outcome {
        std::vector<uint8_t> bytes;
        std::vector<uint64_t> versions;
        uint64_t submits=0,waits=0,allocations=0,reuses=0,uploads=0,queued_bypasses=0,unsupported_bypasses=0;
    };
    const auto run=[&](bool enabled) {
        CheckedCpuWriteQueue queue(&native);
        NativeGpuWorkQueue fifo(device,queue,{},true,false,capacity);
        NativeTransferBatch recorder(device,queue,&fifo);
        ResolveMemory history(device,queue,memory_store_spv,memory_load_spv,kBytes,0,&recorder);
        history.SetCpuWriteUploadReuseEnabled(enabled);
        std::vector<uint8_t> expected(kBytes,0x5a);history.Initialize(0,expected);
        const auto tex=DescribeTexture({2u|(1u<<22),0x1000u|38u,0u,0u,0u,1u<<9});
        std::vector<std::pair<std::shared_ptr<RenderTexture>,std::array<float,4>>> consumers;
        Outcome result;
        // Repeated same-page and alternating-page writes. Every one retains its
        // original direct submission and actual fence wait; only staging owner
        // allocation may be reused. Earlier decoded images stay alive across it.
        const std::array<uint32_t,5> addresses{0x1000,0x1000,0x2000,0x3000,0x1000};
        for(uint32_t phase=0;phase<kWrites;++phase) {
            const auto address=addresses[phase%addresses.size()];
            std::vector<uint8_t> payload(kPage);
            for(uint32_t i=0;i<kPage;++i)payload[i]=uint8_t((phase*37+i*13)^(i>>4));
            const std::array<float,4> pixel{float(phase+1),float(phase+2),.25f,1.f};
            std::memcpy(payload.data(),pixel.data(),sizeof(pixel));
            const auto before=history.Version(tex,address),serial=history.mutation_serial();
            const auto submits=queue.submissions,waits=queue.waits;
            history.InvalidateCpuWrite(address,kPage,false);
            Require(!history.BytesDefined(address,kPage),"Synchronous CPU invalidation retained defined bytes");
            history.InitializeCpuWrite(address,payload,false);
            Require(history.BytesDefined(address,kPage)&&history.Version(tex,address)>before&&history.mutation_serial()==serial+2,
                "Reusable upload changed per-operation ownership/version transitions");
            Require(queue.submissions==submits+1&&queue.waits==waits+1,
                "Reusable upload changed original synchronous submission/wait count");
            ++result.submits;++result.waits;result.versions.push_back(history.Version(tex,address));
            std::copy(payload.begin(),payload.end(),expected.begin()+address);
            payload.assign(kPage,0xee);payload.clear();payload.shrink_to_fit();
            if(phase%12==0)consumers.emplace_back(history.LoadShared(tex,address),pixel);
        }
        const auto after_pages=history.cpu_write_upload_reuse_statistics();
        Require(after_pages.checked_queue&&!after_pages.poisoned,"Checked queue identity/completion was lost");
        Require(after_pages.allocations==(enabled?1u:0u)&&after_pages.reuses==(enabled?kWrites-1u:0u)&&
            after_pages.uploads==(enabled?kWrites:0u)&&after_pages.uploaded_bytes==(enabled?uint64_t(kWrites)*kPage:0u)&&
            after_pages.submissions==(enabled?kWrites:0u)&&after_pages.waits==(enabled?kWrites:0u),
            "Reusable synchronous page-upload positive allocation/fence counters differ");
        for(auto& [image,pixel]:consumers)Require(FirstPixel(device,queue,*image)==pixel,
            "Earlier decoded consumer saw a later reused staging payload");
        // Ordinary initialization stays outside the optimization even at4KiB.
        const std::vector<uint8_t> ordinary(kPage,0x63);history.Initialize(0x5000,ordinary);
        std::copy(ordinary.begin(),ordinary.end(),expected.begin()+0x5000);
        Require(history.cpu_write_upload_reuse_statistics().uploads==after_pages.uploads,
            "Ordinary Initialize entered the CPU-write-only optimization");
        // Invalidation-only update has no staging upload, and exact neighboring
        // bytes remain defined. Restore it through ordinary initialization.
        history.InvalidateCpuWrite(0x4000,4,false);
        Require(!history.BytesDefined(0x4000,4)&&history.BytesDefined(0x4004,kPage-4),
            "Null invalidation changed neighboring ownership");
        const std::array<uint8_t,4> repair{0x5a,0x5a,0x5a,0x5a};history.Initialize(0x4000,repair);
        Require(history.cpu_write_upload_reuse_statistics().uploads==after_pages.uploads,
            "Invalidation-only operation created a staging upload");
        // Unsupported CPU extents/alignment take unchanged ordinary storage.
        for(const auto [address,count]:std::array<std::pair<uint32_t,uint32_t>,3>{{{0x4004,4},{0x6004,kPage},{0x8000,2*kPage}}}) {
            const std::vector<uint8_t> payload(count,uint8_t(address>>8));
            const auto submits=queue.submissions,waits=queue.waits;
            history.InvalidateCpuWrite(address,count,false);history.InitializeCpuWrite(address,payload,false);
            Require(queue.submissions==submits+1&&queue.waits==waits+1,"Unsupported upload changed synchronous fallback");
            std::copy(payload.begin(),payload.end(),expected.begin()+address);
        }
        // The existing queued path must retain its OWN immutable upload owner.
        auto queued=std::vector<uint8_t>(kPage,0x91);
        const auto submits=queue.submissions,waits=queue.waits;
        recorder.Run([&] {history.InvalidateCpuWrite(0xa000,kPage,true);history.InitializeCpuWrite(0xa000,queued,true);});
        Require(fifo.pending()&&queue.submissions==submits&&queue.waits==waits,
            "Queued upload unexpectedly became a synchronous cached upload");
        std::copy(queued.begin(),queued.end(),expected.begin()+0xa000);queued.assign(kPage,0xee);queued.clear();queued.shrink_to_fit();
        result.bytes=history.ReadBytes(0,kBytes);
        Require(result.bytes==expected&&!fifo.pending(),"Reusable upload/fallback changed full64KiB physical bytes or left work pending");
        const auto& writes=history.cpu_write_statistics();
        Require(writes.synchronous_uploads==kWrites+3&&writes.queued_uploads==1,
            "CPU-write direct/queued accounting changed");
        const auto& stats=history.cpu_write_upload_reuse_statistics();
        Require(stats.allocations==after_pages.allocations&&stats.reuses==after_pages.reuses&&stats.uploads==after_pages.uploads&&
            !stats.poisoned&&stats.queued_bypasses==(enabled?1u:0u)&&stats.unsupported_bypasses==(enabled?3u:0u),
            "Queued/unsupported writes reused or reallocated the synchronous staging owner");
        result.allocations=stats.allocations;result.reuses=stats.reuses;result.uploads=stats.uploads;
        result.queued_bypasses=stats.queued_bypasses;result.unsupported_bypasses=stats.unsupported_bypasses;
        return result;
    };
    const auto off=run(false),on=run(true);
    Require(off.bytes==on.bytes&&off.versions==on.versions&&off.submits==on.submits&&off.waits==on.waits,
        "Reusable upload OFF/ON differs in bytes, versions or synchronous fence semantics");
    // Raw Plume's void stderr-only fence handling is not an attested queue.
    // Requesting reuse here must keep the original allocating upload path.
    {
        ResolveMemory history(device,unchecked,memory_store_spv,memory_load_spv,kBytes);
        history.SetCpuWriteUploadReuseEnabled(true);
        const std::vector<uint8_t> payload(kPage,0xa3);history.Initialize(0,std::vector<uint8_t>(kBytes,0x5a));
        history.InvalidateCpuWrite(0x1000,kPage,false);history.InitializeCpuWrite(0x1000,payload,false);
        const auto& stats=history.cpu_write_upload_reuse_statistics();
        Require(!stats.checked_queue&&!stats.allocations&&!stats.reuses&&!stats.uploads&&stats.unchecked_queue_bypasses==1,
            "Unchecked standalone queue admitted reusable staging ownership");
        auto expected=std::vector<uint8_t>(kBytes,0x5a);std::copy(payload.begin(),payload.end(),expected.begin()+0x1000);
        Require(history.ReadBytes(0,kBytes)==expected,"Unchecked queue bypass changed physical guards");
    }
    std::cout<<"PASS: real checked Vulkan queue, "<<kWrites<<" synchronous writes each OFF/ON, 8 preserved earlier consumers, full64KiB guards, identical page versions/submits/waits, oneallocation/"<<on.reuses
        <<" reuses, ordinary/null/3unsupported/queued/unchecked bypasses; capacity="<<capacity<<"; GPU="<<native.physicalDeviceProperties.deviceName<<"\n";
}
}

namespace {
void CheckCpuWriteUploadReuseUncertain(RenderDevice& device,bool after_submit) {
    auto& native=static_cast<VulkanDevice&>(device);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Upload failure fixture requires hardware Vulkan");
    CheckedCpuWriteQueue queue(&native);
    {
        ResolveMemory history(device,queue,memory_store_spv,memory_load_spv,64*1024);
        history.SetCpuWriteUploadReuseEnabled(true);
        history.Initialize(0,std::vector<uint8_t>(64*1024,0x5a));
        queue.inject_after_submit=after_submit;queue.inject_before_wait=!after_submit;
        const std::vector<uint8_t> payload(4096,0x83);bool injected=false;
        try {history.InitializeCpuWrite(0x1000,payload,false);}
        catch(const std::runtime_error& error) {
            injected=std::string_view(error.what())==(after_submit?"Injected after actual submit":"Injected before actual wait");
        }
        const auto& stats=history.cpu_write_upload_reuse_statistics();
        Require(injected&&stats.poisoned&&stats.allocations==1&&!stats.uploads&&!stats.waits,
            "Uncertain accepted upload did not poison its owner");
        const auto submits=queue.submissions,waits=queue.waits,serial=history.mutation_serial();
        bool refused=false;
        try {history.InitializeCpuWrite(0x2000,payload,false);}
        catch(const std::runtime_error& error) {refused=std::string_view(error.what())=="CPU upload reuse is poisoned after a failed write";}
        Require(refused&&queue.submissions==submits&&queue.waits==waits&&history.mutation_serial()==serial&&!stats.reuses,
            "Poisoned upload owner was reused or submitted again");
        // Test-only external completion prevents leaving actual GPU work live
        // when the death probe exits. It intentionally does NOT reset the
        // production uncertainty bit or attest recovery to ResolveMemory.
        Require(queue.last_fence!=nullptr,"Injected submission has no actual fence");
        queue.waitForCommandFence(queue.last_fence);
        Require(queue.waits==waits+1,"Failure probe did not finish the actual accepted copy");
        std::cout<<"PASS predeath: "<<(after_submit?"after-submit":"before-wait")
            <<" exception retained ownership, poison refused reuse, actual GPU copy completed; expecting destructor termination code85\n"<<std::flush;
        std::set_terminate([] {std::_Exit(85);});
    }
    throw std::runtime_error("Uncertain upload destructor unexpectedly released its owners");
}
}

namespace {
// Raw D32 and stencil planes, following the existing stencil-export fixture.
std::vector<uint8_t> ReadPassTransferDepth(RenderDevice& device,RenderCommandQueue& queue,RenderTexture& image) {
    const auto& shape=static_cast<const VulkanTexture&>(image).desc;
    const uint32_t pixels=shape.width*shape.height;
    auto buffer=device.createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(pixels)*5));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();
    commands->begin();commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&image,RenderTextureLayout::COPY_SOURCE));
    std::array<VkBufferImageCopy,2> regions{};
    regions[0].imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
    regions[1].imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};regions[1].bufferOffset=uint64_t(pixels)*4;
    for(auto& region:regions)region.imageExtent={shape.width,shape.height,1};
    auto& native=static_cast<VulkanBuffer&>(*buffer);const auto command=static_cast<VulkanCommandList&>(*commands).vk;
    vkCmdCopyImageToBuffer(command,static_cast<VulkanTexture&>(image).vk,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        native.vk,uint32_t(regions.size()),regions.data());
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto* bytes=static_cast<const uint8_t*>(buffer->map());
    Require(bytes!=nullptr,"Pass transfer depth readback mapping failed");
    Require(vmaInvalidateAllocation(native.device->allocator,native.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
        "Pass transfer depth readback invalidation failed");
    std::vector<uint8_t> result(bytes,bytes+size_t(pixels)*5);buffer->unmap();return result;
}
void CheckPassTransferDescriptorReuse(RenderDevice& device,uint32_t capacity) {
    auto& native=static_cast<VulkanDevice&>(device);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,
        "Pass transfer descriptor fixture requires hardware Vulkan");
    Require(std::endian::native==std::endian::little,"Descriptor raw-pixel fixture requires little-endian host");
    constexpr uint32_t width=17,height=9,pixels=width*height;
    const RenderRect rectangle(0,0,width,height);
    const std::array<std::array<float,4>,2> colors{{{.25f,.5f,.75f,85.f/255.f},{.75f,.25f,.5f,170.f/255.f}}};
    struct Outcome {std::vector<uint8_t> bytes;PassTransfer::DescriptorPool::Statistics stats;uint64_t lists=0,submits=0,waits=0;};
    const auto run=[&](bool enabled) {
        CheckedCpuWriteQueue queue(&native);
        NativeGpuWorkQueue work(device,queue,{},true,false,capacity);
        NativeTransferBatch batch(device,queue,&work);
        PassTransfer passes(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch);
        passes.SetDescriptorPoolEnabled(enabled);
        std::array<std::unique_ptr<RenderTexture>,2> sources;
        for(uint32_t i=0;i<2;++i) {
            sources[i]=device.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R32G32B32A32_FLOAT));
            const auto& c=colors[i];passes.Clear(sources[i].get(),nullptr,true,false,false,rectangle,{c[0],c[1],c[2],c[3]},0);
        }
        struct Target {std::unique_ptr<RenderTexture> image;bool depth;uint32_t source;};
        std::vector<Target> targets;targets.reserve(136);
        // Assertions never destroy accepted images before an actual completed fence.
        struct Drain {NativeTransferBatch& batch;~Drain(){batch.Flush();}} drain{batch};
        const auto record=[&](bool depth,uint32_t source) {
            targets.push_back({device.createTexture(depth?
                RenderTextureDesc::DepthTarget(width,height,RenderFormat::D32_FLOAT_S8_UINT):
                RenderTextureDesc::ColorTarget(width,height,RenderFormat::R32G32B32A32_FLOAT)),depth,source});
            auto& destination=*targets.back().image;
            if(depth)passes.RestoreDepth(*sources[source],destination);
            else passes.Resolve(*sources[source],destination,rectangle,rectangle);
        };
        batch.Run([&]{for(uint32_t i=0;i<64;++i)record(false,i%2);},1);work.SubmitPending();
        const auto first=passes.DescriptorPoolStatistics();
        Require(work.accepted_work()==1&&!work.completed_work()&&work.inflight_batches()==1,
            "First descriptor packet lost its accepted frontier");
        if(enabled)Require(first.requests==64&&first.allocations==64&&first.leased_sets==64&&!first.hits&&!first.returns,
            "First submitted descriptors returned or reused before retirement");
        batch.Run([&]{for(uint32_t i=0;i<64;++i)record(true,(i+1)%2);},2);work.SubmitPending();
        batch.Run([&]{for(uint32_t i=0;i<4;++i)record(i%2,i%2);},3);
        const auto full=passes.DescriptorPoolStatistics();
        Require(work.accepted_work()==3&&!work.completed_work()&&work.inflight_batches()==2&&!work.statistics().waits,
            "Descriptor capacity forced a submission, fence or early packet retirement");
        if(enabled)Require(full.requests==132&&full.allocations==132&&full.capacity_fallbacks==4&&
            full.leased_sets==128&&!full.idle_sets&&!full.hits&&!full.returns&&!full.uncached_destructions,
            "128-live descriptor bound failed its uncached allocation/ownership path");
        work.WaitOldest();const auto partial=passes.DescriptorPoolStatistics();
        Require(work.completed_work()==1&&work.inflight_batches()==1&&work.pending_lists()==2,
            "Oldest descriptor packet retirement covered a newer generation");
        if(enabled)Require(partial.returns==64&&partial.idle_sets==64&&partial.leased_sets==64&&
            !partial.uncached_destructions,"Oldest fence released newer descriptor owners");
        batch.Run([&]{for(uint32_t i=0;i<4;++i)record(i%2,(i+1)%2);},4);
        const auto reused=passes.DescriptorPoolStatistics();
        if(enabled)Require(reused.hits==4&&reused.returns==64&&reused.leased_sets==68&&reused.idle_sets==60,
            "Retired descriptors were not exclusively rebound while newer packets remained pending");
        work.SubmitPending();batch.Flush();
        Outcome result;result.stats=passes.DescriptorPoolStatistics();result.lists=work.accepted_work();
        result.submits=work.statistics().submissions;result.waits=work.statistics().waits;
        Require(result.lists==4&&work.completed_work()==4&&result.submits==3&&result.waits==3&&!work.pending(),
            "Descriptor option changed logical FIFO or physical submission/wait boundaries");
        const auto& s=result.stats;
        if(enabled)Require(s.requests==136&&s.hits==4&&s.misses==132&&s.allocations==132&&s.capacity_fallbacks==4&&
            s.returns==132&&s.uncached_destructions==4&&!s.leased_sets&&s.idle_sets==128&&s.peak_live_sets==128&&
            s.requests==s.hits+s.misses&&s.live_sets==s.leased_sets+s.idle_sets,
            "Final descriptor allocation/reuse/owner accounting differs");
        else Require(!s.enabled&&!s.requests&&!s.allocations&&!s.hits&&!s.returns,
            "Disabled transfer descriptor pool performed pooled work");
        // A post-recording error completes the already accepted prefix before
        // returning its lease; no failed operation is replayed or silently ignored.
        bool rejected=false;
        try {batch.Run([&] {
            passes.Resolve(*sources[1],*targets[0].image,rectangle,rectangle);
            passes.Resolve(*sources[0],*sources[0],rectangle,rectangle);
        },5);}catch(const NativeTransferBatchError&){rejected=true;}
        targets[0].source=1;
        Require(rejected&&!work.pending()&&!passes.DescriptorPoolStatistics().leased_sets&&
            work.accepted_work()==5&&work.completed_work()==5,"Accepted-prefix error lost its descriptor lifetime");
        const auto accepted_bytes=validation::ReadImage(device,queue,*targets[0].image,16);
        Require(accepted_bytes.size()==pixels*16,"Accepted prefix image extent changed");
        for(uint32_t i=0;i<pixels;++i)Require(std::memcmp(accepted_bytes.data()+i*16,colors[1].data(),16)==0,
            "Accepted-prefix exception abandoned the recorded resolve result");
        const auto before=passes.DescriptorPoolStatistics();
        passes.Resolve(*sources[0],*targets[0].image,rectangle,rectangle);targets[0].source=0;
        const auto bypass=passes.DescriptorPoolStatistics();
        Require(bypass.requests==before.requests&&bypass.synchronous_bypasses==uint64_t(enabled),
            "Standalone transfer reused a queued descriptor backing");
        rejected=false;try {passes.SetDescriptorPoolEnabled(!enabled);}catch(const std::logic_error&){rejected=true;}
        Require(rejected,"Transfer descriptor pool accepted late configuration");
        for(const auto& target:targets) {
            auto bytes=target.depth?ReadPassTransferDepth(device,queue,*target.image):
                validation::ReadImage(device,queue,*target.image,16);
            const auto& expected=colors[target.source];
            Require(bytes.size()==pixels*(target.depth?5u:16u),"Transfer target byte extent changed");
            for(uint32_t i=0;i<pixels;++i) {
                if(target.depth) {
                    Require(std::memcmp(bytes.data()+i*4,&expected[0],4)==0&&
                        bytes[pixels*4+i]==(target.source?170:85),"Restored raw D32/stencil retained another descriptor source");
                }else Require(std::memcmp(bytes.data()+i*16,expected.data(),16)==0,
                    "Resolved full RGBA target retained another descriptor source");
            }
            result.bytes.insert(result.bytes.end(),bytes.begin(),bytes.end());
        }
        return result;
    };
    const auto off=run(false),on=run(true);
    Require(off.bytes==on.bytes&&off.lists==on.lists&&off.submits==on.submits&&off.waits==on.waits,
        "Pass transfer descriptor reuse changed exact image bytes or submission boundaries");
    std::cout<<"PASS: pass_transfer_descriptor_pool capacity="<<capacity
        <<" targets=136 color=68 depth_stencil=68 compared_bytes="<<on.bytes.size()
        <<" requests="<<on.stats.requests<<" hits="<<on.stats.hits<<" allocations="<<on.stats.allocations
        <<" capacity_fallbacks="<<on.stats.capacity_fallbacks<<" returns="<<on.stats.returns
        <<" uncached_destructions="<<on.stats.uncached_destructions<<" leased="<<on.stats.leased_sets
        <<" idle="<<on.stats.idle_sets<<" submissions="<<on.submits<<" waits="<<on.waits
        <<"; real two-fence ownership, oldest-only reuse, changed immutable sources, raw D32/stencil parity, accepted-prefix exception, synchronous bypass, startup-only config\n";
}
void CheckPassTransferDescriptorUncertain(RenderDevice& device,bool after_submit,uint32_t capacity) {
    auto& native=static_cast<VulkanDevice&>(device);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,
        "Descriptor failure fixture requires hardware Vulkan");
    CheckedCpuWriteQueue queue(&native);
    {
        NativeGpuWorkQueue work(device,queue,{},true,false,capacity);
        NativeTransferBatch batch(device,queue,&work);
        PassTransfer passes(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch);
        passes.SetDescriptorPoolEnabled(true);
        auto source=device.createTexture(RenderTextureDesc::ColorTarget(1,1,RenderFormat::R32G32B32A32_FLOAT));
        auto target=device.createTexture(RenderTextureDesc::ColorTarget(1,1,RenderFormat::R32G32B32A32_FLOAT));
        const RenderRect one(0,0,1,1);passes.Clear(source.get(),nullptr,true,false,false,one,{1,0,0,1},0);
        batch.Run([&]{passes.Resolve(*source,*target,one,one);},1);
        queue.inject_after_submit=after_submit;queue.inject_before_wait=!after_submit;
        bool injected=false;
        try {if(after_submit)work.SubmitPending();else batch.Flush();}
        catch(const std::runtime_error& error) {
            injected=std::string_view(error.what())==(after_submit?"Injected after actual submit":"Injected before actual wait");
        }
        const auto before=passes.DescriptorPoolStatistics();const auto submits=queue.submissions;
        Require(injected&&work.failed()&&before.requests==1&&before.leased_sets==1&&!before.returns&&!before.hits,
            "Uncertain submission returned or recycled its descriptor lease");
        bool refused=false;try {work.SubmitPending();}catch(const NativeGpuWorkError&){refused=true;}
        const auto after=passes.DescriptorPoolStatistics();
        Require(refused&&queue.submissions==submits&&after.leased_sets==1&&!after.returns&&!after.hits,
            "Poisoned descriptor queue replayed or released accepted ownership");
        Require(queue.last_fence!=nullptr,"Uncertain descriptor submission has no real fence");
        // Complete externally for test teardown only; never attest recovery to
        // the poisoned Native queue or return its still-owned descriptor lease.
        queue.waitForCommandFence(queue.last_fence);
        Require(passes.DescriptorPoolStatistics().leased_sets==1&&!passes.DescriptorPoolStatistics().returns,
            "External fence probe silently returned a poisoned descriptor");
        std::cout<<"PASS predeath: pass transfer descriptor "<<(after_submit?"after-submit":"before-wait")
            <<" retained lease and refused replay; expecting destructor termination code85\n"<<std::flush;
        std::set_terminate([]{std::_Exit(85);});
    }
    throw std::runtime_error("Poisoned descriptor queue unexpectedly destroyed accepted owners");
}
}

namespace {
// Pure key witnesses use independent struct storage, never padding or pointer identity.
uint64_t CheckRenderPassKeys() {
    using Cache=VulkanFramebufferRenderPassCache;
    std::array<VkAttachmentDescription,2> attachments{};
    attachments[0].format=VK_FORMAT_R32G32B32A32_SFLOAT;attachments[0].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout=attachments[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1]=attachments[0];attachments[1].format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    attachments[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachments[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout=attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const auto key_for=[&](const auto& a,const auto& c,const auto& d,Cache::Key& key) {
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1;sub.pColorAttachments=&c;sub.pDepthStencilAttachment=&d;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount=2;info.pAttachments=a.data();info.subpassCount=1;info.pSubpasses=&sub;
        return Cache::MakeKey(info,key);
    };
    Cache::Key baseline;Require(key_for(attachments,color,depth,baseline),"Canonical combined render pass key refused");
    uint64_t checks=1;
    auto copied=attachments;Cache::Key identical;
    Require(key_for(copied,VkAttachmentReference(color),VkAttachmentReference(depth),identical)&&identical==baseline,
        "Render pass key depends on description addresses");++checks;
    for(uint32_t attachment=0;attachment<2;++attachment)for(uint32_t field=0;field<9;++field) {
        auto changed=attachments;auto& a=changed[attachment];
        switch(field) {
        case 0:a.flags=VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;break;
        case 1:a.format=VK_FORMAT_R16G16B16A16_SFLOAT;break;
        case 2:a.samples=VK_SAMPLE_COUNT_4_BIT;break;
        case 3:a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;break;
        case 4:a.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;break;
        case 5:a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;break;
        case 6:a.stencilStoreOp=attachment?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE;break;
        case 7:a.initialLayout=VK_IMAGE_LAYOUT_GENERAL;break;
        case 8:a.finalLayout=VK_IMAGE_LAYOUT_GENERAL;break;
        }
        Cache::Key altered;Require(key_for(changed,color,depth,altered)&&!(altered==baseline),
            "Render pass key omitted an attachment field");++checks;
    }
    for(bool color_layout:{false,true}) {
        auto c=color,d=depth;(color_layout?c:d).layout=VK_IMAGE_LAYOUT_GENERAL;Cache::Key altered;
        Require(key_for(attachments,c,d,altered)&&!(altered==baseline),"Render pass key omitted reference layout");++checks;
    }
    for(uint32_t refusal=0;refusal<13;++refusal) {
        auto c=color,d=depth;VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1;sub.pColorAttachments=&c;sub.pDepthStencilAttachment=&d;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount=2;info.pAttachments=attachments.data();info.subpassCount=1;info.pSubpasses=&sub;
        VkSubpassDependency dependency{};uint32_t preserve=0;
        switch(refusal) {
        case 0:info.pNext=&dependency;break; // Never submitted; unsupported key only.
        case 1:info.flags=1;break;
        case 2:info.dependencyCount=1;info.pDependencies=&dependency;break;
        case 3:info.subpassCount=2;break;
        case 4:sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_COMPUTE;break;
        case 5:sub.flags=1;break;
        case 6:sub.pResolveAttachments=&c;break;
        case 7:sub.inputAttachmentCount=1;sub.pInputAttachments=&c;break;
        case 8:sub.preserveAttachmentCount=1;sub.pPreserveAttachments=&preserve;break;
        case 9:sub.colorAttachmentCount=2;break;
        case 10:c.attachment=1;break;
        case 11:d.attachment=0;break;
        case 12:info.pAttachments=nullptr;break;
        }
        Cache::Key unused;Require(!Cache::MakeKey(info,unused),"Unsupported render pass key was admitted");++checks;
    }
    return checks;
}
std::unique_ptr<VulkanFramebuffer> CachedColorFramebuffer(VulkanDevice& device,RenderTexture& image,
        VulkanFramebufferRenderPassCache* cache) {
    const RenderTexture* attachment=&image;RenderFramebufferDesc desc;
    desc.colorAttachments=&attachment;desc.colorAttachmentsCount=1;
    auto result=std::make_unique<VulkanFramebuffer>(&device,desc,cache);
    Require(result->vk&&result->renderPass,"Fixture cached framebuffer allocation failed");return result;
}
void RecordFramebufferClear(NativeTransferBatch& batch,std::unique_ptr<VulkanFramebuffer> framebuffer,
        const std::shared_ptr<RenderTexture>& image,const RenderColor& color,uint64_t event) {
    batch.Run([&] {
        auto& commands=batch.BeginStage();auto* retained=batch.Keep(std::move(framebuffer));batch.KeepLease(image);
        commands.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(image.get(),RenderTextureLayout::COLOR_WRITE));
        commands.setFramebuffer(retained);
        const RenderRect rectangle(0,0,retained->width,retained->height);commands.clearColor(0,color,&rectangle,1);
    },event);
}
uint64_t CheckRenderPassBackend(RenderDevice& device,uint32_t capacity) {
    auto& native=static_cast<VulkanDevice&>(device);using Cache=VulkanFramebufferRenderPassCache;
    CheckedCpuWriteQueue queue(&native);NativeGpuWorkQueue work(device,queue,{},true,false,capacity);
    NativeTransferBatch batch(device,queue,&work);
    std::array<std::shared_ptr<RenderTexture>,2> images{
        device.createTexture(RenderTextureDesc::ColorTarget(17,9,RenderFormat::R32G32B32A32_FLOAT)),
        device.createTexture(RenderTextureDesc::ColorTarget(23,11,RenderFormat::R32G32B32A32_FLOAT))};
    auto cache=std::make_unique<Cache>(native.vk);
    auto first=CachedColorFramebuffer(native,*images[0],cache.get());
    auto second=CachedColorFramebuffer(native,*images[1],cache.get());
    Require(first->renderPassOwner&&first->renderPassOwner==second->renderPassOwner&&first->vk!=second->vk&&
        first->width==17&&first->height==9&&second->width==23&&second->height==11,
        "Same-format changed image/extent failed exact immutable render pass sharing");
    std::weak_ptr<const VulkanFramebufferRenderPass> owner=first->renderPassOwner;
    // Separate depth, combined and read-only topologies must never alias a color pass.
    auto depth=device.createTexture(RenderTextureDesc::DepthTarget(17,9,RenderFormat::D32_FLOAT_S8_UINT));
    RenderFramebufferDesc depth_desc;depth_desc.depthAttachment=depth.get();
    auto depth_fb=std::make_unique<VulkanFramebuffer>(&native,depth_desc,cache.get());
    const RenderTexture* color=images[0].get();auto combined_desc=depth_desc;
    combined_desc.colorAttachments=&color;combined_desc.colorAttachmentsCount=1;
    auto combined_fb=std::make_unique<VulkanFramebuffer>(&native,combined_desc,cache.get());
    depth_desc.depthAttachmentReadOnly=true;
    auto readonly_fb=std::make_unique<VulkanFramebuffer>(&native,depth_desc,cache.get());
    Require(depth_fb->vk&&combined_fb->vk&&readonly_fb->vk&&depth_fb->renderPassOwner&&
        combined_fb->renderPassOwner&&readonly_fb->renderPassOwner&&
        first->renderPass!=depth_fb->renderPass&&first->renderPass!=combined_fb->renderPass&&
        depth_fb->renderPass!=combined_fb->renderPass&&depth_fb->renderPass!=readonly_fb->renderPass,
        "Render pass cache merged distinct attachment/read-only topologies");
    auto other=device.createTexture(RenderTextureDesc::ColorTarget(17,9,RenderFormat::R32G32B32A32_FLOAT));
    const RenderTexture* two_colors[]{images[0].get(),other.get()};RenderFramebufferDesc unsupported;
    unsupported.colorAttachments=two_colors;unsupported.colorAttachmentsCount=2;
    auto independent=std::make_unique<VulkanFramebuffer>(&native,unsupported,cache.get());
    Require(independent->vk&&independent->renderPass&&!independent->renderPassOwner,
        "Unsupported two-color topology did not use original independent allocation");
    const auto initial=cache->statistics();
    Require(initial.requests==6&&initial.hits==1&&initial.misses==5&&initial.creations==4&&
        initial.unsupported_bypasses==1&&!initial.capacity_bypasses&&!initial.create_failures&&initial.cached_entries==4,
        "Backend render pass topology counters differ");
    independent.reset();readonly_fb.reset();combined_fb.reset();depth_fb.reset();
    const std::array<std::array<float,4>,2> colors{{{.125f,.25f,.5f,1.f},{.75f,.5f,.25f,.125f}}};
    const auto render_color=[](const auto& c){return RenderColor{c[0],c[1],c[2],c[3]};};
    struct Drain {NativeTransferBatch& batch;~Drain(){batch.Flush();}} drain{batch};
    RecordFramebufferClear(batch,std::move(first),images[0],render_color(colors[0]),1);work.SubmitPending();
    RecordFramebufferClear(batch,std::move(second),images[1],render_color(colors[1]),2);work.SubmitPending();
    cache.reset();Require(!owner.expired()&&work.accepted_work()==2&&!work.completed_work(),
        "Cache destruction invalidated pending framebuffer render pass leases");
    work.WaitOldest();Require(!owner.expired()&&work.completed_work()==1&&work.pending_lists()==1,
        "Oldest framebuffer fence destroyed a render pass shared with newer work");
    batch.Flush();Require(owner.expired()&&work.completed_work()==2&&!work.pending(),
        "Last actual framebuffer fence did not release the retired render pass");
    uint64_t bytes_checked=0;
    for(uint32_t i=0;i<2;++i) {
        auto bytes=validation::ReadImage(device,queue,*images[i],16);bytes_checked+=bytes.size();
        for(size_t at=0;at<bytes.size();at+=16)Require(std::memcmp(bytes.data()+at,colors[i].data(),16)==0,
            "Framebuffer owner/extent reuse changed rendered image bytes");
    }
    // Fill16 distinct valid descriptions. The17th real framebuffer must remain
    // independently allocated; no cache eviction or drain may manufacture a hit.
    Cache full(native.vk);
    for(uint32_t i=0;i<16;++i) {
        VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R32_SFLOAT;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=(i&1)?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp=(i&2)?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=(i&4)?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout=(i&8)?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};
        sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&reference;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=1;
        info.pAttachments=&attachment;info.subpassCount=1;info.pSubpasses=&sub;VkResult result=VK_SUCCESS;
        Require(bool(full.Acquire(native.vk,info,result))&&result==VK_SUCCESS,"Valid render pass capacity seed failed");
    }
    auto fallback=CachedColorFramebuffer(native,*images[0],&full);
    Require(!fallback->renderPassOwner,"Full render pass cache did not preserve original allocation fallback");
    const auto stats=full.statistics();Require(stats.requests==17&&stats.hits==0&&stats.misses==17&&
        stats.creations==16&&stats.cached_entries==16&&stats.capacity_bypasses==1&&
        !stats.unsupported_bypasses&&!stats.create_failures&&stats.maximum_entries==16,
        "Render pass capacity fallback changed fixed backing bound");
    RecordFramebufferClear(batch,std::move(fallback),images[0],render_color(colors[1]),3);batch.Flush();
    const auto bytes=validation::ReadImage(device,queue,*images[0],16);bytes_checked+=bytes.size();
    for(size_t at=0;at<bytes.size();at+=16)Require(std::memcmp(bytes.data()+at,colors[1].data(),16)==0,
        "Capacity fallback framebuffer changed actual clear output");
    return bytes_checked;
}
void CheckPassTransferRenderPassReuse(RenderDevice& device,uint32_t capacity) {
    auto& native=static_cast<VulkanDevice&>(device);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,
        "Render pass fixture requires hardware Vulkan");
    Require(std::endian::native==std::endian::little,"Render pass raw-pixel fixture requires little-endian host");
    const auto key_checks=CheckRenderPassKeys();
    struct Outcome {std::vector<uint8_t> bytes;PassTransfer::RenderPassStatistics stats;uint64_t lists,submits,waits;};
    const auto run=[&](bool enabled) {
        CheckedCpuWriteQueue queue(&native);NativeGpuWorkQueue work(device,queue,{},true,false,capacity);
        NativeTransferBatch batch(device,queue,&work);
        PassTransfer passes(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch);
        passes.SetDescriptorPoolEnabled(false);passes.SetRenderPassCacheEnabled(enabled);
        struct Target {uint32_t width,height;std::array<float,4> color;std::unique_ptr<RenderTexture> source,color_image,depth_image;};
        std::vector<Target> targets;targets.reserve(4);
        std::unique_ptr<RenderTexture> single;
        struct Drain {NativeTransferBatch& batch;~Drain(){batch.Flush();}} drain{batch};
        for(uint32_t i=0;i<4;++i) {
            const uint32_t width=(i%2)?23:17,height=(i%2)?11:9;
            const std::array<float,4> color=(i<2)?std::array<float,4>{.25f,.5f,.75f,85.f/255.f}:
                std::array<float,4>{.75f,.25f,.5f,170.f/255.f};
            targets.push_back({width,height,color,
                device.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R32G32B32A32_FLOAT)),
                device.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R32G32B32A32_FLOAT)),
                device.createTexture(RenderTextureDesc::DepthTarget(width,height,RenderFormat::D32_FLOAT_S8_UINT))});
            auto& target=targets.back();const RenderRect all(0,0,width,height),part(1,2,width-2,height-1),corner(0,0,1,1);
            batch.Run([&] {
                passes.Clear(target.source.get(),nullptr,true,false,false,all,{color[0],color[1],color[2],color[3]},0);
                passes.Clear(target.color_image.get(),target.depth_image.get(),true,true,true,all,{.125f,.25f,.375f,.5f},.875f,90);
                passes.Resolve(*target.source,*target.color_image,part,part);
                passes.RestoreDepth(*target.source,*target.depth_image,&part);
                passes.Clear(target.color_image.get(),target.depth_image.get(),true,true,true,corner,{1,.5f,.25f,.125f},.625f,163);
            },i+1);
        }
        single=device.createTexture(RenderTextureDesc::ColorTarget(17,9,RenderFormat::R32_FLOAT));
        batch.Run([&] {const RenderRect all(0,0,17,9);
            passes.Clear(single.get(),nullptr,true,false,false,all,{.125f,0,0,0},0);
            passes.Resolve(*targets[0].source,*single,all,all);
        },5);batch.Flush();
        Outcome out;out.stats=passes.RenderPassCacheStatistics();out.lists=work.accepted_work();
        out.submits=work.statistics().submissions;out.waits=work.statistics().waits;
        Require(out.lists==5&&work.completed_work()==5&&out.submits==1&&out.waits==1&&
            !work.pending()&&!passes.DescriptorPoolStatistics().enabled,
            "Render pass fixture changed completed transfer frontier or enabled descriptor pooling");
        const auto& s=out.stats;
        if(enabled)Require(s.enabled&&s.requests==22&&s.hits==18&&s.misses==4&&s.creations==4&&
            s.cached_entries==4&&s.maximum_entries==16&&!s.create_failures&&!s.unsupported_bypasses&&!s.capacity_bypasses,
            "PassTransfer render pass cache did not reuse exact topology across attachments/extents");
        else Require(!s.enabled&&!s.requests&&!s.hits&&!s.misses&&!s.creations&&!s.cached_entries,
            "Disabled render pass cache performed cached work");
        for(const auto& target:targets) {
            const auto color=validation::ReadImage(device,queue,*target.color_image,16);
            const auto depth=ReadPassTransferDepth(device,queue,*target.depth_image);const uint32_t pixels=target.width*target.height;
            Require(color.size()==pixels*16&&depth.size()==pixels*5,"Render pass target byte extent changed");
            for(uint32_t y=0;y<target.height;++y)for(uint32_t x=0;x<target.width;++x) {
                const uint32_t pixel=y*target.width+x;const bool inside=x>=1&&x<target.width-2&&y>=2&&y<target.height-1;
                const bool corner=x==0&&y==0;
                const std::array<float,4> expected=corner?std::array<float,4>{1,.5f,.25f,.125f}:
                    inside?target.color:std::array<float,4>{.125f,.25f,.375f,.5f};
                const float expected_depth=corner?.625f:inside?target.color[0]:.875f;
                const uint8_t stencil=corner?163:inside?(target.color[0]==.25f?85:170):90;
                Require(std::memcmp(color.data()+pixel*16,expected.data(),16)==0&&
                    std::memcmp(depth.data()+pixel*4,&expected_depth,4)==0&&depth[pixels*4+pixel]==stencil,
                    "Render pass sharing changed partial color/depth/stencil or untouched pixels");
            }
            out.bytes.insert(out.bytes.end(),color.begin(),color.end());out.bytes.insert(out.bytes.end(),depth.begin(),depth.end());
        }
        const auto single_bytes=validation::ReadImage(device,queue,*single,4);const float single_expected=.25f;
        Require(single_bytes.size()==17*9*4,"R32 render pass output extent changed");
        for(size_t at=0;at<single_bytes.size();at+=4)Require(std::memcmp(single_bytes.data()+at,&single_expected,4)==0,
            "Different color format reused an incompatible render pass");
        out.bytes.insert(out.bytes.end(),single_bytes.begin(),single_bytes.end());
        // An accepted prefix must drain before propagation and preserve the actual result.
        bool rejected=false;const RenderRect all(0,0,17,9);
        try {batch.Run([&] {
            passes.Resolve(*targets[0].source,*targets[0].color_image,all,all);
            passes.Resolve(*targets[0].source,*targets[0].source,all,all);
        },6);}catch(const NativeTransferBatchError&){rejected=true;}
        Require(rejected&&!work.pending()&&work.accepted_work()==6&&work.completed_work()==6,
            "Render pass accepted-prefix failure lost completion or replayed work");
        const auto prefix=validation::ReadImage(device,queue,*targets[0].color_image,16);
        Require(prefix.size()==17*9*16,"Render pass accepted-prefix readback extent changed");
        for(size_t at=0;at<prefix.size();at+=16)Require(std::memcmp(prefix.data()+at,targets[0].color.data(),16)==0,
            "Render pass accepted prefix lost its actual image result");
        rejected=false;try{passes.SetRenderPassCacheEnabled(!enabled);}catch(const std::logic_error&){rejected=true;}
        Require(rejected,"Render pass cache accepted repeated/late configuration");
        return out;
    };
    const auto off=run(false),on=run(true);Require(off.bytes==on.bytes&&off.lists==on.lists&&
        off.submits==on.submits&&off.waits==on.waits,"Render pass cache changed bytes or submission boundaries");
    const auto backend_bytes=CheckRenderPassBackend(device,capacity);
    std::cout<<"PASS: pass_transfer_render_pass_cache capacity="<<capacity<<" key_checks="<<key_checks
        <<" cases=4 color_formats=2 topologies=4 compared_bytes="<<on.bytes.size()<<" backend_bytes="<<backend_bytes
        <<" accepted_prefix_bytes=2448"
        <<" requests="<<on.stats.requests<<" hits="<<on.stats.hits<<" creations="<<on.stats.creations
        <<" lists="<<on.lists<<" submissions="<<on.submits<<" waits="<<on.waits
        <<"; exact partial color/raw D32/stencil, independent attachment/extents, read-only separation,16-entry capacity and unsupported fallback, cache deletion/two actual fence leases, accepted-prefix failure, startup-only config\n";
}
void CheckPassTransferRenderPassUncertain(RenderDevice& device,bool after_submit,uint32_t capacity,bool worker=false) {
    auto& native=static_cast<VulkanDevice&>(device);CheckedCpuWriteQueue queue(&native);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,
        "Render pass failure fixture requires hardware Vulkan");
    {
        NativeGpuWorkQueue work(device,queue,{},true,false,capacity,false,2,worker);NativeTransferBatch batch(device,queue,&work);
        auto cache=std::make_unique<VulkanFramebufferRenderPassCache>(native.vk);
        std::shared_ptr<RenderTexture> image=device.createTexture(RenderTextureDesc::ColorTarget(1,1,RenderFormat::R32G32B32A32_FLOAT));
        auto framebuffer=CachedColorFramebuffer(native,*image,cache.get());
        std::weak_ptr<const VulkanFramebufferRenderPass> owner=framebuffer->renderPassOwner;
        RecordFramebufferClear(batch,std::move(framebuffer),image,{1,0,0,1},1);cache.reset();
        queue.inject_after_submit=after_submit;queue.inject_before_wait=!after_submit;
        bool injected=false;try{if(after_submit){work.SubmitPending();if(worker)work.Drain();}else batch.Flush();}
        catch(const std::runtime_error& e){injected=std::string_view(e.what())==(after_submit?"Injected after actual submit":"Injected before actual wait");}
        const auto submits=queue.submissions;Require(injected&&work.failed()&&!owner.expired(),
            "Uncertain GPU work released its packet-owned render pass");
        bool refused=false;try{work.SubmitPending();}catch(const NativeGpuWorkError&){refused=true;}
        Require(refused&&queue.submissions==submits&&!owner.expired()&&queue.last_fence,
            "Poisoned render pass packet was replayed or released");
        queue.waitForCommandFence(queue.last_fence);Require(!owner.expired(),
            "External fence probe silently retired poisoned render pass ownership");
        std::cout<<"PASS predeath: pass transfer render pass "<<(after_submit?"after-submit":"before-wait")
            <<" retained lease and refused replay; expecting destructor termination code85\n"<<std::flush;
        std::set_terminate([]{std::_Exit(85);});
    }
    throw std::runtime_error("Poisoned render pass queue unexpectedly released accepted owners");
}
}


namespace {
void CheckGpuAliasState(RenderDevice& device,RenderCommandQueue& queue,uint32_t capacity) {
    NativeGpuWorkQueue work(device,queue,[](const auto&){},true,false,capacity);
    NativeTransferBatch batch(device,queue,&work);
    constexpr uint32_t capacity_bytes=2048u*5120;
    ResolveMemory memory(device,queue,memory_store_spv,memory_load_spv,capacity_bytes,0,&batch);
    const auto layout=DescribeTexture({2u|(1u<<22),0x1000u|38u,7u|(1u<<13),0u,0u,1u<<9});
    uint64_t checks=0;
    auto inspect=[&](const TextureLayout& view,uint32_t address,bool any,bool full) {
        const auto stages=batch.statistics().stages,submits=work.statistics().submissions,waits=work.statistics().waits;
        const NativeGpuAliasState expected{memory.AnyDefined(view,address),memory.FullyDefined(view,address),memory.Version(view,address)};
        const auto value=memory.InspectTextureState(view,address);
        const auto hits=memory.gpu_alias_state_statistics().hits;
        const auto repeated=memory.InspectTextureState(view,address);++checks;
        Require(value==expected&&repeated==value&&value.any_defined==any&&value.fully_defined==full,
            "Actual GPU alias state differs from old APIs or independent definedness expectation");
        Require(memory.gpu_alias_state_statistics().hits==hits+1&&batch.statistics().stages==stages&&
                work.statistics().submissions==submits&&work.statistics().waits==waits,
            "GPU alias metadata inspection recorded work, waited, or lost same-serial reuse");
        return value;
    };
    auto check=[&](bool any,bool full){return inspect(layout,4096,any,full);};
    check(false,false);
    std::vector<uint8_t> row(128);for(size_t i=0;i<row.size();++i)row[i]=uint8_t(i*17+3);
    memory.Initialize(4096,row);check(true,false);
    memory.Initialize(4608,row);check(true,true);
    memory.Invalidate(4100,4);check(true,false);
    memory.Initialize(4100,std::span<const uint8_t>(row).subspan(4,4));const auto repaired=check(true,true);
    memory.Initialize(0x9000,std::array<uint8_t,4>{7,8,9,10});
    Require(check(true,true).version==repaired.version,"Unrelated write changed sampled version");
    // Endpoint touch leaves exact ownership unchanged but conservatively bumps
    // its shared4KiB version, exactly like the original page-version API.
    memory.Invalidate(4224,4);Require(check(true,true).version>repaired.version,
        "Unsampled same-page invalidation failed to invalidate conservative alias version");
    const auto before=check(true,true).version;
    memory.RecordSurfaceWrite({80,16,80,0,2,false});
    Require(check(true,true).version>before,"Actual RecordSurfaceWrite did not invalidate alias-state memo");
    memory.Copy(4096,8192,128);inspect(layout,8192,true,false);
    memory.Copy(4608,8704,128);inspect(layout,8192,true,true);
    // A queued metadata change is visible in operation order before the fence;
    // inspecting it must not submit/drain that accepted prefix.
    batch.Run([&] {
        memory.InvalidateCpuWrite(4100,4,true);check(true,false);
        memory.InitializeCpuWrite(4100,std::span<const uint8_t>(row).subspan(4,4),true);check(true,true);
    },1);
    batch.Seal();work.Drain();
    const auto& queued=memory.cpu_write_statistics();
    Require(queued.queued_invalidations==1&&queued.queued_uploads==1&&
            queued.synchronous_invalidations==0&&queued.synchronous_uploads==0,
            "GPU alias queued-state witness fell back to synchronous mutation");
    Require(memory.ReadBytes(4096,128)==row&&memory.ReadBytes(4608,128)==row&&
            memory.ReadBytes(8192,128)==row&&memory.ReadBytes(8704,128)==row,
            "Metadata cache changed actual copied/repaired GPU bytes");
    // Actual2048-key footprint clear/re-admission; retain only a copied result.
    const auto copied=check(true,true);
    for(uint32_t i=0;i<2051;++i) {
        const auto value=memory.InspectTextureState(layout,i*4);
        Require(value==NativeGpuAliasState{memory.AnyDefined(layout,i*4),memory.FullyDefined(layout,i*4),memory.Version(layout,i*4)},
            "Footprint cache clear/re-admission changed full-key state");++checks;
    }
    Require(check(true,true)==copied&&memory.gpu_alias_state_statistics().cache_clears>0,
        "Actual footprint clear retained stale memo or modified a copied result");
    // Original validation runs before any memo hit, even for a previously seen fetch.
    auto invalid=layout;invalid.dimension=2;bool rejected=false;
    try{(void)memory.InspectTextureState(invalid,4096);}catch(const std::runtime_error& e){rejected=std::string_view(e.what())=="Unsupported storage footprint";}
    Require(rejected,"Cached GPU alias state bypassed original malformed-layout refusal");
    rejected=false;try{(void)memory.InspectTextureState(layout,capacity_bytes-4);}catch(const std::runtime_error& e){rejected=std::string_view(e.what())=="Resolved texture exceeds backing allocation";}
    Require(rejected,"GPU alias admission bypassed physical capacity refusal");
    const auto& stats=memory.gpu_alias_state_statistics();
    Require(stats.requests==stats.hits+stats.scans&&stats.hits>0&&work.accepted_work()==work.completed_work(),
        "GPU alias state fixture lost cache accounting or queue completion");
    std::cout<<"GPU_ALIAS_STATE_PASS capacity="<<capacity<<" checks="<<checks
        <<" requests="<<stats.requests<<" hits="<<stats.hits<<" scans="<<stats.scans
        <<" cache_clears="<<stats.cache_clears<<" raw_bytes_checked=512"
        <<" original_api_parity=true queued_metadata_order=true partial_redefine=true page_only_write=true validation_preserved=true\n";
}
}

int main(int argc,char** argv) try {
    bool gpu_alias_state_fixture=false;
    bool submission_worker_fixture=false,physical_timestamp_fixture=false;
    int submission_worker_uncertain=0;
    bool cpu_write_upload_reuse_fixture=false;
    bool pass_transfer_render_pass_fixture=false;
    int pass_transfer_render_pass_uncertain=0;
    bool pass_transfer_descriptor_fixture=false;
    int pass_transfer_descriptor_uncertain=0;
    int cpu_write_upload_uncertain_fixture=0;
    uint32_t capacity=NativeSubmissionCapacity::kDefault;
    for(int i=1;i<argc;++i) {
        const std::string_view argument=argv[i];
        if(argument=="--async-submission-capacity=64")capacity=64;
        else if(argument=="--async-submission-capacity=128")capacity=128;
        else if(argument=="--submission-worker-uncertain-submit-fixture")submission_worker_uncertain=1;
        else if(argument=="--submission-worker-uncertain-wait-fixture")submission_worker_uncertain=2;
        else if(argument=="--submission-worker-fixture")submission_worker_fixture=true;
        else if(argument=="--physical-timestamp-fixture")physical_timestamp_fixture=true;
        else if(argument=="--gpu-alias-state-fixture")gpu_alias_state_fixture=true;
        else if(argument=="--pass-transfer-render-pass-fixture")pass_transfer_render_pass_fixture=true;
        else if(argument=="--pass-transfer-render-pass-uncertain-submit-fixture")pass_transfer_render_pass_uncertain=1;
        else if(argument=="--pass-transfer-render-pass-uncertain-wait-fixture")pass_transfer_render_pass_uncertain=2;
        else if(argument=="--pass-transfer-descriptor-fixture")pass_transfer_descriptor_fixture=true;
        else if(argument=="--pass-transfer-descriptor-uncertain-submit-fixture")pass_transfer_descriptor_uncertain=1;
        else if(argument=="--pass-transfer-descriptor-uncertain-wait-fixture")pass_transfer_descriptor_uncertain=2;
        else if(argument=="--cpu-write-upload-reuse-fixture")cpu_write_upload_reuse_fixture=true;
        else if(argument=="--cpu-write-upload-reuse-uncertain-submit-fixture")cpu_write_upload_uncertain_fixture=1;
        else if(argument=="--cpu-write-upload-reuse-uncertain-wait-fixture")cpu_write_upload_uncertain_fixture=2;
        else throw std::invalid_argument("Unknown transfer fixture argument");
    }
    const uint32_t half=capacity/2,total=capacity+half;
    const auto wrapped=DecodeNativeGpuTimestamps(0x1234fff0,0xabcd0010,16,2.5);
    const auto wide=DecodeNativeGpuTimestamps(UINT64_MAX-4,3,64,1.0);
    Require(wrapped&&wrapped->begin_tick==65520&&wrapped->end_tick==16&&wrapped->elapsed_ticks==32&&
        std::abs(wrapped->milliseconds-.00008)<1e-12&&wide&&wide->elapsed_ticks==8,
        "Timestamp decoding lost valid-bit masking, counter wrap or nanosecond period");
    Require(!DecodeNativeGpuTimestamps(0,1,0,1)&&!DecodeNativeGpuTimestamps(0,1,65,1)&&
        !DecodeNativeGpuTimestamps(0,1,32,0)&&!DecodeNativeGpuTimestamps(0,1,32,-1)&&
        !DecodeNativeGpuTimestamps(0,1,32,std::numeric_limits<double>::quiet_NaN()),
        "Unsupported timestamp properties produced a misleading interval");
    auto interface=CreateVulkanInterface();auto device=interface->createDevice();
    Require(bool(device),"Vulkan unavailable");auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
    if(submission_worker_uncertain){CheckPassTransferRenderPassUncertain(*device,submission_worker_uncertain==1,capacity,true);return 1;}
    if(submission_worker_fixture){CheckSubmissionWorker(*device,*queue,capacity);return 0;}
    if(physical_timestamp_fixture){CheckSubmissionWorker(*device,*queue,capacity,true);CheckPhysicalGroups(*device,capacity);return 0;}
    if(gpu_alias_state_fixture){CheckGpuAliasState(*device,*queue,capacity);return 0;}
    if(pass_transfer_render_pass_uncertain){CheckPassTransferRenderPassUncertain(*device,pass_transfer_render_pass_uncertain==1,capacity);return 1;}
    if(pass_transfer_render_pass_fixture){CheckPassTransferRenderPassReuse(*device,capacity);return 0;}
    if(pass_transfer_descriptor_uncertain) {CheckPassTransferDescriptorUncertain(*device,pass_transfer_descriptor_uncertain==1,capacity);return 1;}
    if(pass_transfer_descriptor_fixture) {CheckPassTransferDescriptorReuse(*device,capacity);return 0;}
    if(cpu_write_upload_uncertain_fixture) {CheckCpuWriteUploadReuseUncertain(*device,cpu_write_upload_uncertain_fixture==1);return 1;}
    if(cpu_write_upload_reuse_fixture) {CheckCpuWriteUploadReuse(*device,*queue,capacity);return 0;}
    CheckFourFenceTransfers(*device,*queue,capacity);
    NativeTransferBatch batch(*device,*queue);
    PassTransfer passes(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch);
    ResolveMemory memory(*device,*queue,memory_store_spv,memory_load_spv,1024*1024,0,&batch);
    // A conservative image charge reaches upload pressure without another
    // 128MiB allocation. Its recorded prefix and owner must finish before the
    // <=4KiB upload selects synchronous execution for the remaining scope.
    {
        std::weak_ptr<RenderTexture> image_owner;
        uint64_t retirements=0;bool image_owned_at_retirement=false;
        NativeGpuWorkQueue fifo(*device,*queue,[&](const auto&) {
            ++retirements;image_owned_at_retirement=!image_owner.expired();
        },true,false,capacity);
        NativeTransferBatch recorder(*device,*queue,&fifo);
        PassTransfer transfer(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&recorder);
        ResolveMemory history(*device,*queue,memory_store_spv,memory_load_spv,1024*1024,0,&recorder);
        const auto tex=DescribeTexture({2u|(1u<<22),0x1000u|38u,0u,0u,0u,1u<<9});
        const RenderRect one(0,0,1,1);const std::array<float,4> prior_pixel{.25f,.5f,.75f,1.f};
        auto image=std::shared_ptr<RenderTexture>(device->createTexture(
            RenderTextureDesc::ColorTarget(1,1,RenderFormat::R32G32B32A32_FLOAT)));
        image_owner=image;
        history.Initialize(0x1000,std::vector<uint8_t>(3*4096,0x5a));
        const std::vector<uint8_t> expected(4096,0x42);auto payload=expected;
        recorder.Run([&] {
            recorder.KeepImage(image,NativeTransferBatch::kMaxImageBytes-2048);
            transfer.Clear(image.get(),nullptr,true,false,false,one,{.25f,.5f,.75f,1.f},0);
            history.Store(*image,tex,one,0x1000);image.reset();
            Require(!image_owner.expired()&&recorder.has_recording()&&!fifo.accepted_work(),
                "Upload-pressure prefix lost its unsubmitted image owner");
            Require(!recorder.ReserveUploadBytes(payload.size()),
                "Page upload crossed the scratch bound without synchronous fallback");
            Require(retirements==1&&image_owned_at_retirement&&image_owner.expired()&&
                !recorder.active()&&!recorder.has_recording()&&!fifo.pending()&&
                fifo.accepted_work()==1&&fifo.completed_work()==1&&fifo.statistics().waits==1&&
                recorder.statistics().synchronous_fallbacks==1,
                "Upload pressure dropped its owner or failed to fence the exact prefix");
            history.InvalidateCpuWrite(0x3000,payload.size(),true);
            history.InitializeCpuWrite(0x3000,payload,true);
            std::fill(payload.begin(),payload.end(),0xee);
            Require(history.cpu_write_statistics().synchronous_uploads==1&&
                !history.cpu_write_statistics().queued_uploads&&!recorder.has_recording(),
                "Upload-pressure remainder did not preserve synchronous immutable writes");
        });
        const auto bytes=history.ReadBytes(0x1000,3*4096);std::array<float,4> actual_pixel;
        std::memcpy(actual_pixel.data(),bytes.data(),16);
        Require(actual_pixel==prior_pixel&&
            std::all_of(bytes.begin()+16,bytes.begin()+2*4096,[](auto b){return b==0x5a;})&&
            std::equal(bytes.begin()+2*4096,bytes.end(),expected.begin())&&
            fifo.statistics().maximum_scratch_bytes<=NativeGpuWorkQueue::kMaxScratchBytes,
            "Upload-pressure fallback changed prior GPU bytes, neighboring bytes or CPU payload");
        Require(!recorder.in_scope()&&!fifo.pending(),"Upload-pressure scope retained accepted work");
    }
    // A preceding GPU store, two same-address immutable CPU updates and two
    // decoded consumers must preserve exact FIFO values across one final fence.
    auto cpu_write_case=[&](bool enabled) {
        NativeGpuWorkQueue fifo(*device,*queue,{},true,false,capacity);
        NativeTransferBatch recorder(*device,*queue,&fifo);
        ResolveMemory history(*device,*queue,memory_store_spv,memory_load_spv,1024*1024,0,&recorder);
        PassTransfer transfer(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&recorder);
        const auto tex=DescribeTexture({2u|(1u<<22),0x1000u|38u,0u,0u,0u,1u<<9});
        const RenderRect one(0,0,1,1);
        auto target=device->createTexture(RenderTextureDesc::ColorTarget(1,1,RenderFormat::R32G32B32A32_FLOAT));
        const std::vector<uint8_t> sentinel(3*4096,0x5a);history.Initialize(0x1000,sentinel);
        const auto version0=history.Version(tex,0x1000);
        recorder.Run([&] {
            transfer.Clear(target.get(),nullptr,true,false,false,one,{.25f,.5f,.75f,1.f},0);
            history.Store(*target,tex,one,0x1000);
        });
        std::array<std::shared_ptr<RenderTexture>,2> decoded;
        for(unsigned phase=0;phase<2;++phase) {
            std::vector<uint8_t> payload(4096,uint8_t(0x31+phase));
            const std::array<float,4> pixel{float(phase+1),0,0,1};std::memcpy(payload.data(),pixel.data(),16);
            const auto before=fifo.statistics().waits;
            recorder.Run([&] {
                history.InvalidateCpuWrite(0x1000,4096,enabled);
                history.InitializeCpuWrite(0x1000,payload,enabled);
            });
            Require(history.Version(tex,0x1000)>version0&&history.BytesDefined(0x1000,4096),
                "Queued CPU update lost logical byte ownership/version");
            if(enabled)Require(fifo.pending()&&fifo.statistics().waits==before,
                "Immutable CPU update inserted an eager fence");
            payload.assign(4096,0xee);payload.clear();payload.shrink_to_fit();
            recorder.Run([&]{decoded[phase]=history.LoadShared(tex,0x1000);});
        }
        Require(history.cpu_write_statistics().queued_uploads==(enabled?2u:0u)&&
            history.cpu_write_statistics().synchronous_uploads==(enabled?0u:2u),
            "CPU write queue counters do not distinguish the paired paths");
        const auto bytes=history.ReadBytes(0x1000,3*4096);
        Require(!fifo.pending(),"CPU readback failed to complete queued uploads/consumers");
        for(unsigned phase=0;phase<2;++phase)Require(
            FirstPixel(*device,*queue,*decoded[phase])==std::array<float,4>{float(phase+1),0,0,1},
            "Earlier decoded consumer saw a later same-address upload");
        Require(std::all_of(bytes.begin()+4096,bytes.end(),[](auto b){return b==0x5a;}),
            "Queued CPU update changed bytes outside its exact physical page");
        // Null/freed update invalidates only metadata, retains the prior queued
        // consumer and rejects CPU reads of now-undefined bytes.
        recorder.Run([&]{decoded[0]=history.LoadShared(tex,0x1000);});
        const auto null_before=fifo.statistics().waits;
        recorder.Run([&]{history.InvalidateCpuWrite(0x1000,4,enabled);});
        Require(!history.BytesDefined(0x1000,4)&&history.BytesDefined(0x1004,4092),
            "Null update retained invalid bytes or invalidated unrelated bytes");
        if(enabled)Require(fifo.pending()&&fifo.statistics().waits==null_before,
            "Metadata-only invalidation inserted an eager fence");
        bool rejected=false;try{history.ReadBytes(0x1000,4);}catch(const std::runtime_error&){rejected=true;}
        Require(rejected&&!fifo.pending(),"Undefined read was accepted or failed to drain accepted owners");
        history.Initialize(0x1000,std::span(bytes.data(),4096));
        recorder.Run([&]{decoded[0]=history.LoadShared(tex,0x1000);});
        const std::array<uint8_t,16> copied{9,8,7,6,5,4,3,2,1};
        history.CopyFromCpu(0x1000,0x3000,copied);
        Require(!fifo.pending()&&history.ReadBytes(0x3000,16)==std::vector<uint8_t>(copied.begin(),copied.end()),
            "CPU memory_copy stopped fencing its source owners/destination write");
        // More than one page remains synchronous even if explicitly requested.
        recorder.Run([&]{decoded[0]=history.LoadShared(tex,0x1000);});
        const auto large_before=fifo.statistics().waits;
        recorder.Run([&]{history.InitializeCpuWrite(0x1000,std::vector<uint8_t>(8192,0x42),enabled);});
        Require(!fifo.pending()&&fifo.statistics().waits>large_before,
            "Larger CPU update bypassed the bounded synchronous path");
        Require(fifo.statistics().maximum_scratch_bytes<=NativeGpuWorkQueue::kMaxScratchBytes,
            "CPU uploads exceeded the shared scratch bound");
        return bytes;
    };
    Require(cpu_write_case(false)==cpu_write_case(true),"Queued/eager CPU update physical bytes differ");
    auto source=device->createTexture(RenderTextureDesc::ColorTarget(16,16,RenderFormat::R32G32B32A32_FLOAT));
    auto destination=device->createTexture(RenderTextureDesc::ColorTarget(16,16,RenderFormat::R32G32B32A32_FLOAT));
    const RenderRect rectangle(0,0,16,16);
    const auto layout=DescribeTexture({2u|(1u<<22),0x1000u|38u,15u|(15u<<13),0u,0u,1u<<9});
    batch.Run([&] {
        passes.Clear(source.get(),nullptr,true,false,false,rectangle,{.25f,.5f,.75f,1.f},0);
        memory.Store(*source,layout,rectangle,0x1000);
        auto decoded=memory.LoadShared(layout,0x1000);
        passes.Resolve(*decoded,*destination,rectangle,rectangle,1,PassTransfer::Conversion::Color,0x60a);
        memory.Store(*destination,layout,rectangle,0x4000);
    });
    auto stats=batch.statistics();
    Require(stats.stages==5&&stats.submissions==1&&stats.waits==1,"Five mixed GPU transfer stages did not share one fence");
    auto decoded=memory.Load(layout,0x4000);
    Require(FirstPixel(*device,*queue,*decoded)==std::array<float,4>{1.5f,1.f,.5f,2.f},
        "Compute/raster/compute batch changed format conversion, exponent or swizzle");
    Require(memory.FullyDefined(layout,0x4000),"Batched store lost exact defined ownership");

    const auto before_limit=batch.statistics();
    batch.Run([&] {
        for(unsigned i=0;i<70;++i)passes.Clear(source.get(),nullptr,true,false,false,rectangle,{float(i),0,0,1},0);
    });
    stats=batch.statistics();
    Require(stats.stages-before_limit.stages==70&&stats.submissions-before_limit.submissions==2&&
        stats.waits-before_limit.waits==2&&stats.maximum_stages==64,"Stage limit did not fence before command/resource reuse");
    Require(FirstPixel(*device,*queue,*source)==std::array<float,4>{69,0,0,1},"Stage-limit flush changed operation order");

    // CPU readback within a scope is a real boundary. Further GPU work may
    // accumulate only after the readback has obtained completed bytes.
    const auto before_read=batch.statistics();
    batch.Run([&] {
        passes.Clear(source.get(),nullptr,true,false,false,rectangle,{1,0,0,1},0);
        memory.Store(*source,layout,rectangle,0x1000);
        const auto bytes=memory.ReadBytes(0x1000,16);std::array<float,4> value;
        std::memcpy(value.data(),bytes.data(),16);
        Require(value==std::array<float,4>{1,0,0,1},"Readback passed an unsubmitted transfer");
        passes.Clear(source.get(),nullptr,true,false,false,rectangle,{0,0,1,1},0);
        memory.Store(*source,layout,rectangle,0x1000);
    });
    Require(batch.statistics().submissions-before_read.submissions==2,"Readback boundary failed to split transfer batches");
    decoded=memory.Load(layout,0x1000);
    Require(FirstPixel(*device,*queue,*decoded)==std::array<float,4>{0,0,1,1},"Work after readback was not completed");

    // The 65th small scratch image exhausts the bounded owner list without
    // allocating a large image. Finish the prefix and preserve the public API
    // through synchronous execution for the remainder of this operation.
    const auto before_images=batch.statistics();std::shared_ptr<RenderTexture> final_image;
    batch.Run([&]{for(unsigned i=0;i<65;++i)final_image=memory.LoadShared(layout,0x1000);});
    Require(batch.statistics().synchronous_fallbacks==before_images.synchronous_fallbacks+1,
        "Scratch-image owner pressure did not select bounded synchronous execution");
    Require(FirstPixel(*device,*queue,*final_image)==std::array<float,4>{0,0,1,1},"Scratch-pressure fallback changed pixels");

    bool rejected=false;
    try {batch.Run([&]{
        passes.Clear(source.get(),nullptr,true,false,false,rectangle,{0,1,0,1},0);
        throw std::runtime_error("intentional later stage validation failure");
    });}catch(const NativeTransferBatchError&){rejected=true;}
    Require(rejected&&!batch.in_scope(),"Accepted-stage exception was not drained and escalated");
    Require(FirstPixel(*device,*queue,*source)==std::array<float,4>{0,1,0,1},"Exception abandoned recorded image layout/write");
    const auto before_reject=batch.statistics();rejected=false;
    try {batch.Run([&]{passes.Resolve(*source,*source,rectangle,rectangle);});}
    catch(const NativeTransferBatchError&){throw std::runtime_error("Pre-record validation incorrectly became accepted-work failure");}
    catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&batch.statistics().stages==before_reject.stages&&
        batch.statistics().submissions==before_reject.submissions,"Pre-record rejection submitted work");
    // The same helpers may seal multiple operations behind a shared fence.
    // A retirement callback must see an unsealed later recorder during scratch
    // pressure, so callers cannot publish a zero-draw query ahead of it.
    NativeTransferBatch* recording=nullptr;uint64_t unsealed_retirements=0;
    NativeGpuWorkQueue shared(*device,*queue,[&](const NativeGpuWorkQueue::Completion&) {
        if(recording&&recording->has_recording())++unsealed_retirements;
    });
    NativeTransferBatch unified(*device,*queue,&shared);recording=&unified;
    PassTransfer shared_passes(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&unified);
    ResolveMemory shared_memory(*device,*queue,memory_store_spv,memory_load_spv,1024*1024,0,&unified);
    unified.Run([&]{shared_passes.Clear(source.get(),nullptr,true,false,false,rectangle,{.5f,.25f,1,1},0);});
    unified.Run([&]{shared_memory.Store(*source,layout,rectangle,0x1000);});
    Require(shared.pending_lists()==2&&shared.statistics().submissions==0,
        "Unified operation exit fenced or lost an owned transfer");
    Require(!shared.statistics().timings_enabled&&!shared.statistics().timestamps_supported&&
        shared.statistics().timestamp_status=="disabled","Timestamp diagnostics changed the default queue");
    auto actual=shared_memory.ReadBytes(0x1000,16);std::array<float,4> unified_pixel;
    std::memcpy(unified_pixel.data(),actual.data(),16);
    Require(unified_pixel==std::array<float,4>{.5f,.25f,1,1}&&shared.statistics().submissions==1&&
        !shared.pending(),"Legacy CPU readback did not fence the whole shared FIFO");
    const auto pressure_before=shared.statistics().submissions;
    for(unsigned i=0;i<70;++i)unified.Run([&]{
        shared_passes.Clear(source.get(),nullptr,true,false,false,rectangle,{float(i),0,0,1},0);
    });
    Require(shared.statistics().submissions==pressure_before+1&&shared.pending_lists()==6,
        "Shared64-list pressure did not retain the exact remaining FIFO");
    unified.Flush();Require(shared.statistics().submissions==pressure_before+2&&
        FirstPixel(*device,*queue,*source)==std::array<float,4>{69,0,0,1},"FIFO pressure changed transfer order");

    constexpr uint64_t large_bytes=64ull*1024*1024;
    auto large_desc=RenderTextureDesc::ColorTarget(2048,2048,RenderFormat::R32G32B32A32_FLOAT);
    auto first_large=std::shared_ptr<RenderTexture>(device->createTexture(large_desc));
    auto second_large=std::shared_ptr<RenderTexture>(device->createTexture(large_desc));
    const RenderRect large_rect(0,0,2048,2048);
    std::weak_ptr<RenderTexture> first_owner=first_large,second_owner=second_large;
    unified.Run([&]{
        unified.KeepImage(first_large,large_bytes);
        shared_passes.Clear(first_large.get(),nullptr,true,false,false,large_rect,{1,0,0,1},0);
    });
    first_large.reset();Require(!first_owner.expired(),"Sealed packet lost its scratch image owner");
    auto tiny=std::shared_ptr<RenderTexture>(device->createTexture(RenderTextureDesc::ColorTarget(1,1,RenderFormat::R32G32B32A32_FLOAT)));
    unified.Run([&]{
        unified.KeepImage(second_large,large_bytes);
        shared_passes.Clear(second_large.get(),nullptr,true,false,false,large_rect,{0,1,0,1},0);
        //64MiB retained FIFO +64MiB current recorder +16B next scratch owner
        // forces retirement while the current command list remains unsealed.
        unified.KeepImage(tiny,16);
        Require(unsealed_retirements==1&&first_owner.expired()&&!second_owner.expired(),
            "Scratch pressure overtook unsealed work or released its resources");
        shared_passes.Clear(tiny.get(),nullptr,true,false,false,RenderRect(0,0,1,1),{0,0,1,1},0);
    });
    second_large.reset();Require(!second_owner.expired(),"Current scope failed to transfer scratch ownership");
    Require(shared.statistics().maximum_scratch_bytes<=NativeGpuWorkQueue::kMaxScratchBytes,
        "Shared scratch accounting exceeded128MiB");
    unified.Flush();Require(second_owner.expired()&&!shared.pending(),"Fenced scratch owners were not retired");
    Require(FirstPixel(*device,*queue,*tiny)==std::array<float,4>{0,0,1,1},"Unsealed-pressure tail did not execute");
    // Submission is genuinely asynchronous: both half-capacity batches retain all
    // resources until their own fences, even if the hardware already finished.
    // A later batch remains owned when only the oldest prefix is retired.
    std::array<uint64_t,3> retired_events{};size_t retirement_count=0;
    std::array<std::optional<NativeGpuTimestampInterval>,3> gpu_intervals;
    NativeGpuWorkQueue overlap(*device,*queue,[&](const NativeGpuWorkQueue::Completion& completion) {
        Require(retirement_count<retired_events.size()&&completion.transfers==half&&completion.draws==0,
            "Async transfer fence retired the wrong accepted prefix");
        Require(completion.submission==retirement_count+1&&completion.timings_enabled&&
            std::isfinite(completion.submit_ms)&&completion.submit_ms>=0&&
            std::isfinite(completion.fence_wait_ms)&&completion.fence_wait_ms>=0&&
            std::isfinite(completion.resource_retirement_ms)&&completion.resource_retirement_ms>=0&&
            std::abs(completion.milliseconds-completion.fence_wait_ms-completion.resource_retirement_ms)<1e-9,
            "Optional queue timings lost fence generation or mixed timestamp overhead into host waiting");
        gpu_intervals[retirement_count]=completion.gpu_interval;
        retired_events[retirement_count++]=completion.last_event;
    },true,true,capacity,true);
    NativeTransferBatch asynchronous_batch(*device,*queue,&overlap);
    PassTransfer asynchronous_passes(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&asynchronous_batch);
    std::vector<std::weak_ptr<RenderTexture>> owners;owners.reserve(total);
    std::shared_ptr<RenderTexture> last_image;
    auto record_image=[&](unsigned i) {
        auto image=std::shared_ptr<RenderTexture>(device->createTexture(RenderTextureDesc::ColorTarget(16,16,RenderFormat::R32G32B32A32_FLOAT)));
        owners.push_back(image);if(i==total-1)last_image=image;
        NativeGpuWorkTag tag;tag.event=i+1;tag.frame=uint32_t(i/half);tag.kind=NativeGpuWorkKind::Clear;
        asynchronous_batch.Run([&]{
            asynchronous_batch.KeepImage(image,16*16*16);
            asynchronous_passes.Clear(image.get(),nullptr,true,false,false,rectangle,{float(i),0,0,1},0);
        },i+1,&tag);
    };
    for(unsigned i=0;i<capacity;++i)record_image(i);
    overlap.SubmitPending();
    Require(overlap.inflight_batches()==2&&overlap.pending_lists()==capacity&&overlap.statistics().waits==0,
        "Async submission waited before two half-capacity batches were in flight");
    Require(overlap.attribution()->totals().batches==0,"Attribution overtook an actual retirement fence");
    for(const auto& owner:owners)Require(!owner.expired(),"An unfenced transfer resource was released");
    overlap.WaitOldest();
    Require(overlap.attribution()->totals().batches==1&&overlap.attribution()->recent(0).last_event==half,
        "Attribution consumed the wrong fence prefix");
    for(unsigned i=0;i<capacity;++i)Require(owners[i].expired()==(i<half),"Fence A retired resources from another batch");
    Require(overlap.pending_lists()==half&&retirement_count==1&&retired_events[0]==half,
        "First async fence did not retain the exact FIFO tail");
    for(unsigned i=capacity;i<total;++i)record_image(i);
    overlap.SubmitPending();Require(overlap.inflight_batches()==2,"Retired ring capacity did not support a second live batch");
    overlap.WaitOldest();
    for(unsigned i=0;i<total;++i)Require(owners[i].expired()==(i<capacity),"Fence B reused or released the wrong ring generation");
    asynchronous_batch.Flush();
    Require(FirstPixel(*device,*queue,*last_image)==std::array<float,4>{float(total-1),0,0,1},
        "Asynchronous ring reuse changed actual transfer contents");
    last_image.reset();for(const auto& owner:owners)Require(owner.expired(),"Final drain retained a scratch owner");
    Require(retirement_count==3&&retired_events==std::array<uint64_t,3>{half,capacity,total}&&!overlap.pending()&&
        overlap.statistics().maximum_inflight_batches==2&&overlap.statistics().maximum_lists==capacity&&
        overlap.statistics().maximum_scratch_bytes<=NativeGpuWorkQueue::kMaxScratchBytes,
        "Asynchronous submission/retirement order or global resource bound changed");
    Require(overlap.attribution()->totals().batches==3&&overlap.attribution()->totals().homogeneous==3,
        "Retired attribution lost transfer operation labels");
    for(unsigned i=0;i<3;++i) {
        const auto& sample=overlap.attribution()->recent(i);
        Require(sample.submission==i+1&&sample.first_frame==i&&sample.last_frame==i&&
            sample.first_event==i*half+1&&sample.last_event==(i+1)*half&&
            sample.transfers==half&&sample.components[0].tag.kind==NativeGpuWorkKind::Clear&&
            sample.retired_steady_ns&&sample.interval.has_value()==gpu_intervals[i].has_value(),
            "Attribution metadata or timestamp generation changed across ring reuse");
        if(sample.interval)Require(sample.interval->milliseconds==gpu_intervals[i]->milliseconds,
            "Attribution changed the original fenced GPU interval");
    }
    const auto& measured=overlap.statistics();
    Require(measured.timings_enabled&&measured.submit_ms>=0&&measured.fence_wait_ms>=0&&
        measured.resource_retirement_ms>=0&&measured.completion_callback_ms>=0,
        "Queue CPU phase timers are invalid");
    if(measured.timestamps_supported) {
        Require(measured.timestamp_status=="enabled"&&measured.gpu_intervals==3&&measured.gpu_unavailable==0,
            "A fenced timestamp pair was missing or consumed from the other in-flight slot");
        double interval_sum=0;
        for(const auto& interval:gpu_intervals) {
            // Very coarse counters may legitimately round tiny workloads to
            // zero. Availability and exact fence generation remain mandatory.
            Require(interval&&std::isfinite(interval->milliseconds)&&interval->milliseconds>=0,
                "Fenced transfer work produced an unavailable/invalid GPU interval");
            interval_sum+=interval->milliseconds;
        }
        Require(std::abs(interval_sum-measured.gpu_interval_sum_ms)<1e-9,
            "Reused timestamp pairs were lost or accumulated more than once");
        std::cout<<"Timestamp diagnostics: "<<measured.gpu_intervals<<" fenced pairs, GPU inclusive interval sum "
            <<measured.gpu_interval_sum_ms<<"ms, submit "<<measured.submit_ms<<"ms, host wait "
            <<measured.fence_wait_ms<<"ms, resource retirement "<<measured.resource_retirement_ms<<"ms\n";
    } else std::cout<<"Timestamp diagnostics unavailable: "<<measured.timestamp_status<<'\n';
    // Scratch reservation can force submission of a staged tail while both
    // fence slots are occupied. If freeing one slot also satisfies the byte
    // budget, it must not wait for the next batch as well. Conservative byte
    // charges test the admission policy without allocating another128MiB.
    struct ReservationLease {std::unique_ptr<RenderCommandList> commands;};
    NativeGpuWorkQueue pressure(*device,*queue,{},true,false,capacity);
    std::array<std::weak_ptr<ReservationLease>,3> reservation_owners;
    auto reserve_packet=[&](unsigned i,uint64_t bytes) {
        pressure.Reserve(bytes);auto owner=std::make_shared<ReservationLease>();
        owner->commands=queue->createCommandList();owner->commands->begin();owner->commands->end();
        reservation_owners[i]=owner;pressure.EnqueueTransfer(*owner->commands,owner,bytes,i+1);
    };
    reserve_packet(0,64ull*1024*1024);pressure.SubmitPending();
    reserve_packet(1,16ull*1024*1024);pressure.SubmitPending();
    reserve_packet(2,16ull*1024*1024);
    pressure.Reserve(64ull*1024*1024);
    Require(pressure.statistics().waits==1&&pressure.inflight_batches()==2&&pressure.pending_lists()==2&&
        reservation_owners[0].expired()&&!reservation_owners[1].expired()&&!reservation_owners[2].expired(),
        "Sufficient scratch-pressure retirement waited for an unrelated later batch");
    pressure.Drain();
    for(const auto& owner:reservation_owners)Require(owner.expired(),"Scratch reservation lease survived its final fence");
    std::cout<<"Capacity "<<capacity<<", asynchronous batch "<<half<<" lists: ";
    std::cout<<"PASS: five GPU stages/one fence, 64-stage limit, exact conversion/swizzle/ownership, CPU readback boundary, scratch-pressure fallback, exception drain, unified64-list FIFO/readback and128MiB unsealed-pressure ownership, two asynchronous fences and exact prefix resource retirement\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
