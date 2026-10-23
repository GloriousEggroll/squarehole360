#include "gpu_work_queue.h"
#include "submission_gpu_timing.h"
#include "submission_worker.h"
#include "live_device.h"
#include "plume_vulkan.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <span>
#include <vector>
namespace gow3::renderer {
namespace {
using Clock=std::chrono::steady_clock;
double Milliseconds(Clock::time_point begin,Clock::time_point end) {
    return std::chrono::duration<double,std::milli>(end-begin).count();
}
}
struct NativeGpuWorkQueue::SubmissionWorkerState {
    struct Request {
        std::array<const plume::RenderCommandList*,2*kMaxLists+3> lists{};
        uint32_t count=0;
        plume::RenderCommandFence* fence=nullptr;
    };
    NativeSubmissionWorker<Request,kMaximumInflightBatches> worker;
    SubmissionWorkerState(plume::RenderCommandQueue& queue,bool timing):worker([&queue](const Request& request) {
        // The queue owner lock and throwing completion contract are provided
        // by the live adapter. No rendering/guest state is accessed here.
        auto lists=request.lists;
        queue.executeCommandLists(lists.data(),request.count,nullptr,0,nullptr,0,request.fence);
    },timing) {}
};
struct NativeGpuWorkQueue::TimestampState {
    struct Slot {
        std::unique_ptr<plume::RenderCommandList> begin,end;
        uint64_t submission=0;
        Clock::time_point submitted;
    };
    VkDevice device=VK_NULL_HANDLE;
    VkQueryPool pool=VK_NULL_HANDLE;
    std::array<Slot,kMaximumInflightBatches> slots;
    uint32_t valid_bits=0;
    double period_ns=0;
    std::string status;
    TimestampState(plume::RenderDevice& render_device,plume::RenderCommandQueue& render_queue,uint32_t slot_count) {
        auto* vk_device=dynamic_cast<plume::VulkanDevice*>(&render_device);
        auto* vk_queue=dynamic_cast<plume::VulkanCommandQueue*>(&render_queue);
        if(!vk_device||!vk_queue||vk_queue->device!=vk_device||!vk_device->vk||!vk_device->physicalDevice) {
            status="unavailable: queue is not the supplied Vulkan device";return;
        }
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(vk_device->physicalDevice,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(vk_device->physicalDevice,&count,families.data());
        if(vk_queue->familyIndex>=count||!families[vk_queue->familyIndex].timestampValidBits) {
            status="unavailable: queue family has no timestamp counter";return;
        }
        valid_bits=families[vk_queue->familyIndex].timestampValidBits;
        period_ns=vk_device->physicalDeviceProperties.limits.timestampPeriod;
        if(!DecodeNativeGpuTimestamps(0,0,valid_bits,period_ns)) {
            status="unavailable: invalid timestamp counter properties";return;
        }
        device=vk_device->vk;
        // Fixed marker command lists belong to the configured fence slots.
        // They do not consume draw/query/FIFO capacity or retain guest resources.
        for(auto& slot:std::span(slots).first(slot_count)) {
            slot.begin=render_queue.createCommandList();slot.end=render_queue.createCommandList();
            if(!slot.begin||!slot.end)throw std::runtime_error("Cannot create native timestamp markers");
        }
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=slot_count*2;
        const auto result=vkCreateQueryPool(device,&info,nullptr,&pool);
        if(result!=VK_SUCCESS) {status="unavailable: timestamp query pool result "+std::to_string(result);return;}
        status="enabled";
    }
    ~TimestampState(){if(pool)vkDestroyQueryPool(device,pool,nullptr);}
    void Record(uint32_t index) {
        auto& slot=slots.at(index);
        if(!pool||slot.submission)throw NativeGpuWorkError("Native timestamp slot reused before its fence result");
        slot.begin->begin();auto& begin=static_cast<plume::VulkanCommandList&>(*slot.begin);
        vkCmdResetQueryPool(begin.vk,pool,index*2,2);
        vkCmdWriteTimestamp(begin.vk,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,index*2);slot.begin->end();
        slot.end->begin();auto& end=static_cast<plume::VulkanCommandList&>(*slot.end);
        vkCmdWriteTimestamp(end.vk,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,index*2+1);slot.end->end();
    }
    void Submitted(uint32_t index,uint64_t generation,Clock::time_point time) {
        slots[index].submission=generation;slots[index].submitted=time;
    }
    std::optional<NativeGpuTimestampInterval> Read(uint32_t index,uint64_t generation) {
        auto& slot=slots.at(index);
        if(!generation||slot.submission!=generation)
            throw NativeGpuWorkError("Native timestamp fence generation mismatch");
        struct Value {uint64_t ticks,available;};std::array<Value,2> values{};
        // This exact pair is fenced already. Never wait on the later slot or
        // add a timestamp-specific GPU/CPU synchronization boundary.
        const auto result=vkGetQueryPoolResults(device,pool,index*2,2,sizeof(values),values.data(),sizeof(Value),
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const auto elapsed=std::chrono::duration<double,std::nano>(Clock::now()-slot.submitted).count();
        slot.submission=0;
        // Conservative ambiguity rejection for unusually narrow counters or
        // long host stalls. No unavailable interval is reported as zero work.
        const auto counter_period=std::ldexp(period_ns,int(valid_bits));
        if(result!=VK_SUCCESS||!values[0].available||!values[1].available||elapsed>=counter_period)return {};
        return DecodeNativeGpuTimestamps(values[0].ticks,values[1].ticks,valid_bits,period_ns);
    }
};
NativeGpuWorkQueue::NativeGpuWorkQueue(plume::RenderDevice& device,plume::RenderCommandQueue& queue,
    std::function<void(const Completion&)> completed,bool asynchronous,bool timestamps,uint32_t capacity,bool batch_attribution,uint32_t max_inflight_batches,bool submission_worker,
    bool physical_timestamps,uint32_t physical_first,uint32_t physical_end,uint32_t physical_interval)
    :capacity_(capacity,asynchronous),queue_(queue),completed_(std::move(completed)),asynchronous_(asynchronous) {
    if(max_inflight_batches!=2&&max_inflight_batches!=4)
        throw std::invalid_argument("Native asynchronous fence window must be 2 or 4");
    max_inflight_batches_=asynchronous?max_inflight_batches:1;
    if(batch_attribution&&!timestamps)throw std::invalid_argument("GPU batch attribution requires timestamps");
    if(submission_worker&&(!asynchronous||!dynamic_cast<NativeCheckedQueueCompletion*>(&queue)))
        throw std::invalid_argument("Native submission worker requires a checked asynchronous queue");
    statistics_.reason_counters_enabled=timestamps;
    batch_attribution_enabled_=batch_attribution;physical_timestamps_requested_=physical_timestamps;
    if(physical_timestamps&&(!physical_interval||physical_first>=physical_end||physical_end==UINT32_MAX))
        throw std::invalid_argument("Invalid physical timestamp API frame range");
    if(batch_attribution||physical_timestamps) {
        attribution_=std::make_unique<NativeGpuBatchAttribution>();
        attribution_tags_=std::make_unique<std::array<NativeGpuWorkTag,kMaxLists>>();
    }
    for(auto& batch:std::span(batches_).first(max_inflight_batches_))batch.fence=device.createCommandFence();
    if(timestamps) {
        statistics_.timings_enabled=true;
        timestamps_=std::make_unique<TimestampState>(device,queue,max_inflight_batches_);
        statistics_.timestamp_status=timestamps_->status;
        statistics_.timestamps_supported=timestamps_->pool!=VK_NULL_HANDLE;
        statistics_.timestamp_valid_bits=timestamps_->valid_bits;
        statistics_.timestamp_period_ns=timestamps_->period_ns;
        if(!timestamps_->pool)timestamps_.reset();
    }
    if(physical_timestamps)try {
        physical_timestamps_=std::make_unique<NativeSubmissionGpuTimings>(device,queue,capacity_.lists,max_inflight_batches_,
            NativeSubmissionTimingLedger::FrameSelection{physical_first,physical_end,physical_interval});
    }catch(...) {/* Optional allocation must not prevent rendering. */}
    if(submission_worker) {
        submission_worker_=std::make_unique<SubmissionWorkerState>(queue,statistics_.timings_enabled);
        statistics_.submission_worker_enabled=true;
    }
}
NativeGpuWorkQueue::~NativeGpuWorkQueue(){submission_worker_.reset();if(count_||failed_)std::terminate();}
bool NativeGpuWorkQueue::CollectSubmission(uint32_t index,bool wait) {
    auto& batch=batches_[index];
    if(!batch.driver_receipt_pending)return true;
    try {
        const auto begin=statistics_.timings_enabled&&wait?Clock::now():Clock::time_point{};
        const auto receipt=submission_worker_->worker.Collect(index,batch.submission,wait);
        if(!receipt)return false;
        if(statistics_.timings_enabled&&wait)statistics_.driver_receipt_wait_ms+=Milliseconds(begin,Clock::now());
        batch.submit_ms=receipt->submit_ms;statistics_.submit_ms+=batch.submit_ms;
        if(timestamps_)timestamps_->Submitted(index,batch.submission,receipt->began);
        if(physical_timestamps_&&batch.physical_timed)physical_timestamps_->Submitted(index,batch.submission);
        batch.driver_receipt_pending=false;++statistics_.driver_receipts;
        return true;
    }catch(const std::exception& error){failed_=true;throw NativeGpuWorkError(error.what());}
    catch(...){failed_=true;throw NativeGpuWorkError("Native driver submission receipt failed");}
}
void NativeGpuWorkQueue::Reserve(uint64_t scratch_bytes) {
    if(failed_||retiring_)throw NativeGpuWorkError("Cannot reserve failed or retiring native GPU work");
    if(scratch_bytes>kMaxScratchBytes)throw std::invalid_argument("Native GPU scratch reservation exceeds queue bound");
    if(statistics_.reason_counters_enabled) {
        // Once per reservation at entry, before pressure can retire a fence.
        ++statistics_.reserve_calls;
        statistics_.reserve_list_pressure+=count_==capacity_.lists;
        statistics_.reserve_scratch_pressure+=scratch_bytes_>kMaxScratchBytes-scratch_bytes;
    }
    if(asynchronous_&&count_-submitted_count_>=capacity_.AsyncBatchLists())SubmitStaged(SubmissionReason::StagingCapacity);
    while(count_==capacity_.lists||scratch_bytes_>kMaxScratchBytes-scratch_bytes) {
        if(!asynchronous_){Drain();break;}
        // Submission itself doesn't release ownership or budget. Only an
        // actual oldest fence can make room for an additional accepted list.
        if(count_!=submitted_count_)SubmitStaged(SubmissionReason::ReservationPressure);
        // A third submission may itself retire the oldest occupied batch.
        // Recheck the budget before unnecessarily waiting on its successor.
        if(count_<capacity_.lists&&scratch_bytes_<=kMaxScratchBytes-scratch_bytes)break;
        if(!batch_count_)throw NativeGpuWorkError("Native GPU pressure has no submitted owner to retire");
        WaitOldest(WaitReason::ReservationPressure);
    }
}
void NativeGpuWorkQueue::CheckEnqueue(uint64_t scratch_bytes) const {
    frontier_.CheckAccept();
    if(failed_||retiring_||count_==capacity_.lists||scratch_bytes>kMaxScratchBytes||scratch_bytes_>kMaxScratchBytes-scratch_bytes)
        throw NativeGpuWorkError("Native GPU queue enqueue without sufficient reservation");
}
void NativeGpuWorkQueue::EnqueuePinnedDraw(plume::RenderCommandList& commands,uint64_t event,uint64_t token,const NativeGpuWorkTag* tag) {
    SealPendingDrawGroup();
    CheckEnqueue(0);if(!token)throw NativeGpuWorkError("Native draw has no ownership generation");
    if(attribution_tags_)(*attribution_tags_)[(head_+count_)%capacity_.lists]=tag?*tag:NativeGpuWorkTag{event};
    entries_[(head_+count_)%capacity_.lists]={&commands,{},0,event,token,true};++count_;++staged_draws_;frontier_.Accept();
    statistics_.maximum_lists=std::max(statistics_.maximum_lists,uint64_t(count_));
}
void NativeGpuWorkQueue::ConfigureDrawCommandGroups(std::function<void()> seal) {
    if(!seal||count_||failed_||retiring_||seal_draw_command_group_)
        throw std::invalid_argument("Native draw command groups must be configured exactly once before accepted work");
    seal_draw_command_group_=std::move(seal);statistics_.draw_command_groups_enabled=true;
}
void NativeGpuWorkQueue::ValidateDrawCommandGroupAdmission(plume::RenderCommandList& commands,uint64_t token) const {
    if(!seal_draw_command_group_)throw NativeGpuWorkError("Native draw command groups are not configured");
    CheckEnqueue(0);if(!token)throw NativeGpuWorkError("Native grouped draw has no ownership generation");
    (void)draw_command_groups_.Preview(reinterpret_cast<uintptr_t>(&commands));
}
void NativeGpuWorkQueue::EnqueuePinnedDrawInGroup(plume::RenderCommandList& commands,uint64_t event,uint64_t token,const NativeGpuWorkTag* tag) {
    ValidateDrawCommandGroupAdmission(commands,token);
    const auto admission=draw_command_groups_.Accept(reinterpret_cast<uintptr_t>(&commands));
    const uint32_t index=(head_+count_)%capacity_.lists;
    if(admission.leader)group_leader_index_=index;
    if(attribution_tags_)(*attribution_tags_)[index]=tag?*tag:NativeGpuWorkTag{event};
    // A follower is logical work only. Repeating the leader's VkCommandBuffer
    // in the submission array would execute all children multiple times.
    entries_[index]={admission.leader?&commands:nullptr,{},0,event,token,true,admission.group,0};
    ++count_;++staged_draws_;frontier_.Accept();
    ++statistics_.grouped_draws;
    statistics_.maximum_lists=std::max(statistics_.maximum_lists,uint64_t(count_));
}
void NativeGpuWorkQueue::ClosePinnedDrawGroup(plume::RenderCommandList& commands) {
    if(failed_||retiring_)throw NativeGpuWorkError("Native draw group close after failure or during retirement");
    auto& leader=entries_[group_leader_index_];
    if(draw_command_groups_.command()!=reinterpret_cast<uintptr_t>(&commands)||
       leader.commands!=&commands||leader.draw_group!=draw_command_groups_.group()||leader.group_draws)
        throw NativeGpuWorkError("Native draw group close lost its accepted leader ownership");
    const auto closed=draw_command_groups_.Seal(reinterpret_cast<uintptr_t>(&commands));
    leader.group_draws=closed.draws;++statistics_.draw_command_groups;
    statistics_.maximum_group_draws=std::max(statistics_.maximum_group_draws,closed.draws);
}
void NativeGpuWorkQueue::SealPendingDrawGroup() {
    if(!draw_command_groups_.command())return;
    if(failed_||retiring_)throw NativeGpuWorkError("Native draw group close after failure or during retirement");
    if(!seal_draw_command_group_||sealing_draw_command_group_) {
        failed_=true;
        throw NativeGpuWorkError("Native draw group has no nonrecursive close owner");
    }
    sealing_draw_command_group_=true;
    try {
        seal_draw_command_group_();
        if(draw_command_groups_.command())throw NativeGpuWorkError("Native draw group callback left an open command buffer");
        sealing_draw_command_group_=false;
    } catch(const std::exception& error) {
        failed_=true;sealing_draw_command_group_=false;throw NativeGpuWorkError(error.what());
    } catch(...) {
        failed_=true;sealing_draw_command_group_=false;throw NativeGpuWorkError("Native draw group close failed");
    }
}
void NativeGpuWorkQueue::EnqueueTransfer(plume::RenderCommandList& commands,std::shared_ptr<void> lease,
    uint64_t scratch_bytes,uint64_t event,const NativeGpuWorkTag* tag) {
    SealPendingDrawGroup();
    CheckEnqueue(scratch_bytes);
    if(!lease)throw NativeGpuWorkError("Native transfer queue requires an ownership lease");
    if(attribution_tags_)(*attribution_tags_)[(head_+count_)%capacity_.lists]=tag?*tag:NativeGpuWorkTag{event};
    entries_[(head_+count_)%capacity_.lists]={&commands,std::move(lease),scratch_bytes,event,0,false};++count_;
    scratch_bytes_+=scratch_bytes;frontier_.Accept();
    statistics_.maximum_lists=std::max(statistics_.maximum_lists,uint64_t(count_));
    statistics_.maximum_scratch_bytes=std::max(statistics_.maximum_scratch_bytes,scratch_bytes_);
}
void NativeGpuWorkQueue::SubmitStaged(SubmissionReason reason) {
    if(failed_||retiring_)throw NativeGpuWorkError("Native GPU submission after failure or during retirement");
    SealPendingDrawGroup();
    if(count_==submitted_count_)return;
    if(batch_count_==max_inflight_batches_) {
        if(statistics_.reason_counters_enabled)++statistics_.inflight_fence_pressure;
        WaitOldest(WaitReason::InflightFencePressure);
    }
    const uint32_t size=count_-submitted_count_;
    if(asynchronous_&&size>capacity_.AsyncBatchLists())throw NativeGpuWorkError("Native asynchronous staging exceeded configured batch capacity");
    std::array<const plume::RenderCommandList*,2*kMaxLists+3> lists{};
    const uint32_t marker_count=timestamps_?2:0,offset=timestamps_?1:0;
    uint32_t physical_size=size;
    if(!seal_draw_command_group_)
        for(uint32_t i=0;i<size;++i)lists[i+offset]=entries_[(head_+submitted_count_+i)%capacity_.lists].commands;
    const uint32_t batch_index=(batch_head_+batch_count_)%max_inflight_batches_;
    auto& batch=batches_[batch_index];
    bool physical_timed=false;
    try {
        std::array<NativeDrawGroupEntry,kMaxLists> logical;
        if(seal_draw_command_group_||physical_timestamps_) {
            for(uint32_t i=0;i<size;++i) {
                const auto& entry=entries_[(head_+submitted_count_+i)%capacity_.lists];
                logical[i]={reinterpret_cast<uintptr_t>(entry.commands),entry.draw_group,entry.group_draws,entry.draw,entry.draw_token};
            }
            const auto plan=NativeDrawCommandGroups::Plan({logical.data(),size});
            if(plan.logical_draws!=staged_draws_||plan.logical_draws+plan.logical_transfers!=size)
                throw NativeGpuWorkError("Native draw group physical plan changed its logical FIFO prefix");
            physical_size=plan.physical_lists;
            for(uint32_t i=0;i<physical_size;++i)
                lists[i+offset]=entries_[(head_+submitted_count_+plan.leaders[i])%capacity_.lists].commands;
        }
        uint32_t submit_count=physical_size+marker_count;
        if(physical_timestamps_) {
            std::array<NativeGpuWorkTag,kMaxLists> tags;
            for(uint32_t i=0;i<size;++i)tags[i]=(*attribution_tags_)[(head_+submitted_count_+i)%capacity_.lists];
            physical_timed=physical_timestamps_->Prepare(batch_index,statistics_.submissions+1,
                frontier_.completed()+submitted_count_+1,{logical.data(),size},{tags.data(),size});
            if(physical_timed) {
                const auto* plan=physical_timestamps_->Plan(batch_index,statistics_.submissions+1);
                if(!plan||plan->work.physical_lists!=physical_size)throw NativeGpuWorkError("Diagnostic physical plan mismatch");
                std::array<const plume::RenderCommandList*,kMaxLists+1> markers;
                for(uint32_t i=0;i<=physical_size;++i)markers[i]=physical_timestamps_->Marker(batch_index,i);
                submit_count=NativeSubmissionTimingLedger::Interleave<const plume::RenderCommandList*>(
                    lists,physical_size,marker_count,{markers.data(),physical_size+1});
            }
        }
        if(timestamps_) {
            const auto start=Clock::now();timestamps_->Record(batch_index);
            lists[0]=timestamps_->slots[batch_index].begin.get();
            lists[submit_count-1]=timestamps_->slots[batch_index].end.get();
            statistics_.timestamp_record_ms+=Milliseconds(start,Clock::now());
        }
        // Entries and all leases stay owned even if submission fails; no error
        // path can replay or release uncertain GPU work as reference commands.
        const auto submitted=statistics_.timings_enabled?Clock::now():Clock::time_point{};
        if(submission_worker_) {
            submission_worker_->worker.Submit(batch_index,statistics_.submissions+1,
                SubmissionWorkerState::Request{lists,submit_count,batch.fence.get()});
            batch.driver_receipt_pending=true;
        } else queue_.executeCommandLists(lists.data(),submit_count,nullptr,0,nullptr,0,batch.fence.get());
        batch.physical_timed=physical_timed;
        statistics_.physical_lists+=physical_size;
        statistics_.maximum_physical_lists_per_submission=std::max(statistics_.maximum_physical_lists_per_submission,physical_size);
        batch.submit_ms=statistics_.timings_enabled&&!submission_worker_?Milliseconds(submitted,Clock::now()):0;
        statistics_.submit_ms+=batch.submit_ms;
        batch.count=size;batch.submission=++statistics_.submissions;
        if(statistics_.reason_counters_enabled) {
            auto& counter=statistics_.submission_reasons[size_t(reason)];
            ++counter.submissions;counter.lists+=size;counter.draws+=staged_draws_;
            counter.transfers+=size-staged_draws_;
        }
        if(timestamps_&&!submission_worker_)timestamps_->Submitted(batch_index,batch.submission,submitted);
        if(physical_timed&&!submission_worker_)physical_timestamps_->Submitted(batch_index,batch.submission);
        ++batch_count_;submitted_count_+=size;staged_draws_=0;
        statistics_.maximum_inflight_batches=std::max(statistics_.maximum_inflight_batches,uint64_t(batch_count_));
    } catch(const std::exception& error){if(physical_timed)physical_timestamps_->Poison(batch_index,statistics_.submissions+1);failed_=true;throw NativeGpuWorkError(error.what());}
    catch(...){if(physical_timed)physical_timestamps_->Poison(batch_index,statistics_.submissions+1);failed_=true;throw NativeGpuWorkError("Native asynchronous GPU submission failed");}
}
void NativeGpuWorkQueue::SubmitPending(SubmissionReason reason) {
    if(asynchronous_)SubmitStaged(reason);else Drain();
}
void NativeGpuWorkQueue::WaitOldest(WaitReason reason) {SealPendingDrawGroup();RetireOldest(false,reason);}
void NativeGpuWorkQueue::WaitThrough(uint64_t required) {
    if(failed_||retiring_)throw NativeGpuWorkError("Native GPU prefix wait after failure or during retirement");
    SealPendingDrawGroup();
    if(statistics_.reason_counters_enabled)++statistics_.frontier_wait_calls;
    if(frontier_.Reached(required))return;
    // Existing submissions may already cover the end. Submitting unrelated
    // newer staging could pressure-wait another fence, so leave that tail alone.
    if(required>frontier_.completed()+submitted_count_)SubmitStaged(SubmissionReason::FrontierCoverage);
    while(!frontier_.Reached(required)) {
        if(!batch_count_)throw NativeGpuWorkError("Native query frontier has no covering submission");
        WaitOldest(WaitReason::FrontierCoverage);
    }
}
void NativeGpuWorkQueue::PollCompleted() {
    if(failed_||retiring_)throw NativeGpuWorkError("Native GPU poll after failure or during retirement");
    // Retire submitted prefixes only. Newer open groups are outside every
    // submitted batch, so their entries/leader/owners cannot be recycled here.
    while(batch_count_) {
        if(!CollectSubmission(batch_head_,false))return;
        const auto* fence=dynamic_cast<const plume::VulkanCommandFence*>(batches_[batch_head_].fence.get());
        if(!fence||!fence->device||!fence->device->vk)
            throw NativeGpuWorkError("Native nonblocking completion requires a Vulkan fence");
        ++statistics_.fence_polls;
        const auto result=vkGetFenceStatus(fence->device->vk,fence->vk);
        if(result==VK_NOT_READY)return;
        if(result!=VK_SUCCESS) {
            failed_=true;
            throw NativeGpuWorkError("Native GPU fence poll failed: "+std::to_string(result));
        }
        RetireOldest(true);
    }
}
void NativeGpuWorkQueue::RetireOldest(bool already_signaled,WaitReason reason) {
    if(failed_||retiring_)throw NativeGpuWorkError("Native GPU queue is failed or recursively retiring");
    if(!batch_count_)return;
    retiring_=true;auto& batch=batches_[batch_head_];
    const auto began=Clock::now();
    Completion completion;completion.submission=batch.submission;
    completion.timings_enabled=statistics_.timings_enabled;
    try {
        const auto receipt_wait_before=statistics_.driver_receipt_wait_ms;
        CollectSubmission(batch_head_,true);
        completion.driver_receipt_wait_ms=statistics_.driver_receipt_wait_ms-receipt_wait_before;
        completion.submit_ms=batch.submit_ms;
        const auto fence_began=statistics_.timings_enabled?Clock::now():Clock::time_point{};
        if(already_signaled) {
            // The successful status query is the completion proof. Reset the
            // same fence for reuse without an unnecessary host wait call.
            auto& fence=static_cast<plume::VulkanCommandFence&>(*batch.fence);
            if(vkResetFences(fence.device->vk,1,&fence.vk)!=VK_SUCCESS)
                throw NativeGpuWorkError("Native polled fence reset failed");
            ++statistics_.poll_retirements;
        } else {
            queue_.waitForCommandFence(batch.fence.get());++statistics_.waits;
            if(statistics_.reason_counters_enabled)++statistics_.wait_reasons[size_t(reason)];
        }
        if(statistics_.timings_enabled)completion.fence_wait_ms=Milliseconds(fence_began,Clock::now());
        if(statistics_.reason_counters_enabled&&!already_signaled)
            statistics_.wait_reason_ms[size_t(reason)]+=completion.fence_wait_ms;
        if(timestamps_) {
            const auto start=Clock::now();completion.gpu_interval=timestamps_->Read(batch_head_,batch.submission);
            statistics_.timestamp_read_ms+=Milliseconds(start,Clock::now());
            if(completion.gpu_interval) {
                ++statistics_.gpu_intervals;statistics_.gpu_interval_sum_ms+=completion.gpu_interval->milliseconds;
            } else ++statistics_.gpu_unavailable;
        }
        if(physical_timestamps_&&batch.physical_timed)
            physical_timestamps_->AfterOriginalFence(batch_head_,batch.submission);
        std::optional<NativeGpuBatchSample> attribution_sample;
        if(attribution_&&batch_attribution_enabled_) {
            attribution_sample.emplace();
            attribution_sample->submission=completion.submission;
            attribution_sample->interval=completion.gpu_interval;
            attribution_sample->host_wait_ms=completion.fence_wait_ms;
            attribution_sample->submit_ms=completion.submit_ms;
        }
        const auto retiring=statistics_.timings_enabled?Clock::now():Clock::time_point{};
        for(uint32_t i=0;i<batch.count;++i) {
            auto& entry=entries_[(head_+i)%capacity_.lists];
            if(attribution_sample)attribution_sample->Add((*attribution_tags_)[(head_+i)%capacity_.lists],entry.draw);
            completion.last_event=std::max(completion.last_event,entry.event);
            if(entry.draw)completion.draw_tokens[completion.draws++]=entry.draw_token;else ++completion.transfers;
            scratch_bytes_-=entry.scratch_bytes;entry={};
        }
        statistics_.draw_lists+=completion.draws;statistics_.transfer_lists+=completion.transfers;
        frontier_.Complete(batch.count);
        head_=(head_+batch.count)%capacity_.lists;count_-=batch.count;submitted_count_-=batch.count;
        batch.count=0;batch.submission=0;batch.submit_ms=0;batch.physical_timed=false;batch_head_=(batch_head_+1)%max_inflight_batches_;--batch_count_;
        const auto retired=Clock::now();
        if(statistics_.timings_enabled) {
            completion.resource_retirement_ms=Milliseconds(retiring,retired);
            statistics_.fence_wait_ms+=completion.fence_wait_ms;
            statistics_.resource_retirement_ms+=completion.resource_retirement_ms;
            // Preserve the old wait+resource-cleanup meaning without adding
            // the optional timestamp readback overhead into that legacy timer.
            completion.milliseconds=completion.driver_receipt_wait_ms+completion.fence_wait_ms+completion.resource_retirement_ms;
        } else completion.milliseconds=Milliseconds(began,retired);
        if(attribution_sample) {
            attribution_sample->retired_steady_ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(retired.time_since_epoch()).count());
            attribution_->Record(*attribution_sample);
        }
        if(completed_)completed_(completion);
        if(statistics_.timings_enabled)statistics_.completion_callback_ms+=Milliseconds(retired,Clock::now());
        retiring_=false;
    } catch(const std::exception& error){failed_=true;throw NativeGpuWorkError(error.what());}
    catch(...){failed_=true;throw NativeGpuWorkError("Native GPU fence retirement failed");}
}
nlohmann::json NativeGpuWorkQueue::PhysicalTimingReport(bool details) const {
    if(physical_timestamps_)return physical_timestamps_->Report(attribution_.get(),details);
    return {{"enabled",physical_timestamps_requested_},{"supported",false},
        {"retention_complete",false},{"status","disabled or diagnostic allocation failed"}};
}
void NativeGpuWorkQueue::Drain() {
    if(failed_||retiring_)throw NativeGpuWorkError("Native GPU queue is failed or recursively draining");
    if(statistics_.reason_counters_enabled)++statistics_.explicit_drain_calls;
    SubmitStaged(SubmissionReason::ExplicitDrain);
    while(batch_count_)WaitOldest(WaitReason::ExplicitDrain);
}
}
