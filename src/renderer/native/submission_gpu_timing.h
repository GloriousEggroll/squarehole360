#pragma once
#include "submission_timing_ledger.h"
#include "gpu_batch_attribution_json.h"
#include "plume_vulkan.h"
#include <chrono>

namespace gow3::renderer {
// Optional adapter owned by NativeGpuWorkQueue, never by a logical child.
// New marker lists are interleaved only after the existing closed-list plan.
class NativeSubmissionGpuTimings {
public:
    using Ledger=NativeSubmissionTimingLedger;
    using Clock=std::chrono::steady_clock;
    static uint64_t Now() {return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());}
    NativeSubmissionGpuTimings(plume::RenderDevice& device,plume::RenderCommandQueue& queue,
        uint32_t capacity,uint32_t slots,Ledger::FrameSelection frames,size_t maximum_records=262144)
        :ledger_(capacity,slots,maximum_records,frames),capacity_(capacity),slots_(slots) {
        auto* d=dynamic_cast<plume::VulkanDevice*>(&device);
        auto* q=dynamic_cast<plume::VulkanCommandQueue*>(&queue);
        if(!d||!q||q->device!=d||!d->vk||!d->physicalDevice) {status_="unavailable: Vulkan device/queue";return;}
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(d->physicalDevice,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(d->physicalDevice,&count,families.data());
        if(q->familyIndex>=count||!(valid_bits_=families[q->familyIndex].timestampValidBits)) {
            status_="unavailable: timestamp counter";return;
        }
        period_ns_=d->physicalDeviceProperties.limits.timestampPeriod;
        if(!DecodeNativeGpuTimestamps(0,0,valid_bits_,period_ns_)) {status_="unavailable: counter properties";return;}
        // Allocation/creation failures propagate to the optional caller guard.
        // Create the query pool last, so a throwing constructor cannot leak it.
        for(uint32_t s=0;s<slots_;++s)for(uint32_t i=0;i<=capacity_;++i) {
            markers_[s][i]=queue.createCommandList();
            if(!markers_[s][i])throw std::runtime_error("Physical timestamp marker allocation");
        }
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2*capacity_*slots_;
        device_=d->vk;const auto result=vkCreateQueryPool(device_,&info,nullptr,&pool_);
        status_=result==VK_SUCCESS?"enabled":"unavailable: timestamp pool";
    }
    ~NativeSubmissionGpuTimings(){if(pool_)vkDestroyQueryPool(device_,pool_,nullptr);}
    bool Prepare(uint32_t slot,uint64_t generation,uint64_t serial,
        std::span<const NativeDrawGroupEntry> logical,std::span<const NativeGpuWorkTag> tags) noexcept {
        if(!pool_)return false;
        std::array<Ledger::Metadata,Ledger::kCapacity> metadata;
        if(tags.size()>metadata.size())return false;
        for(size_t i=0;i<tags.size();++i) {
            metadata[i].work=tags[i];metadata[i].vertex_variant=tags[i].vertex_variant;
            metadata[i].pixel_variant=tags[i].pixel_variant;metadata[i].cohort=tags[i].cohort;
            metadata[i].cohort_known=tags[i].cohort_known;
            metadata[i].fsi_state_specialized=tags[i].fsi_state_specialized;
        }
        // One clock sample per considered submission; no off-sample GPU markers.
        if(!ledger_.Prepare(slot,generation,serial,logical,{metadata.data(),tags.size()},Now()))return false;
        const auto begin=Now();
        try {
            const auto& plan=*ledger_.PreparedPlan(slot,generation);
            for(uint32_t i=0;i<=plan.work.physical_lists;++i) {
                auto& command=*markers_[slot][i];command.begin();
                auto& vk=static_cast<plume::VulkanCommandList&>(command);
                const auto& marker=plan.markers[i];
                if(marker.reset_count)vkCmdResetQueryPool(vk.vk,pool_,marker.reset_first,marker.reset_count);
                if(marker.end_query!=UINT32_MAX)
                    vkCmdWriteTimestamp(vk.vk,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool_,marker.end_query);
                if(marker.begin_query!=UINT32_MAX)
                    vkCmdWriteTimestamp(vk.vk,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool_,marker.begin_query);
                command.end();
            }
            record_ns_+=Now()-begin;return true;
        } catch(...) {ledger_.Poison(slot,generation);status_="disabled: marker recording";return false;}
    }
    const Ledger::Plan* Plan(uint32_t slot,uint64_t generation) const noexcept {return ledger_.PreparedPlan(slot,generation);}
    const plume::RenderCommandList* Marker(uint32_t slot,uint32_t ordinal) const {return markers_.at(slot).at(ordinal).get();}
    void Submitted(uint32_t slot,uint64_t generation) noexcept {
        // This is receipt observation, not GPU start or exclusive execution.
        if(!ledger_.Submitted(slot,generation,Now()))status_="disabled: submission identity";
    }
    void Poison(uint32_t slot,uint64_t generation) noexcept {ledger_.Poison(slot,generation);status_="disabled: uncertain submission";}
    void AfterOriginalFence(uint32_t slot,uint64_t generation) noexcept {
        const auto start=Now();const auto* plan=ledger_.PreparedPlan(slot,generation);
        if(!plan){ledger_.Poison(slot,generation);status_="disabled: fence identity";return;}
        struct Value {uint64_t ticks,available;};std::array<Value,2*Ledger::kCapacity> values{};
        std::array<std::optional<NativeGpuTimestampInterval>,Ledger::kCapacity> intervals;
        const auto result=vkGetQueryPoolResults(device_,pool_,plan->first_query,plan->query_count,
            plan->query_count*sizeof(Value),values.data(),sizeof(Value),
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT); // Never WAIT_BIT.
        // Lifetime starts BEFORE submit (including any submission-worker delay).
        // Conservatively reject an ambiguous counter wrap, even if receipt was late.
        const auto prepared=ledger_.PreparedTime(slot,generation);
        const auto now=Now();const auto period=std::ldexp(period_ns_,int(valid_bits_));
        if(result==VK_SUCCESS&&prepared&&now>=prepared&&double(now-prepared)<period)
            for(uint32_t p=0;p<plan->work.physical_lists;++p)
                if(values[2*p].available&&values[2*p+1].available)
                    intervals[p]=DecodeNativeGpuTimestamps(values[2*p].ticks,values[2*p+1].ticks,valid_bits_,period_ns_);
        if(!ledger_.CompleteAfterOriginalFence(slot,generation,{intervals.data(),plan->work.physical_lists},now))
            status_="disabled: retirement identity";
        read_ns_+=Now()-start;
    }
    nlohmann::json Report(const NativeGpuBatchAttribution* dictionary,bool details) const {
        using Json=nlohmann::json;const auto& c=ledger_.counters();const auto& f=ledger_.frames();
        Json out={{"enabled",true},{"status",status_},{"supported",pool_!=VK_NULL_HANDLE},
            {"scope","physical lists of sampled, already-closed unified-queue submissions"},
            {"semantics","Inclusive and possibly overlapping list intervals; not additive shader cost. All children retained. Direct synchronous work is outside this scope."},
            {"first_api_frame",f.first},{"end_api_frame_exclusive",f.end},{"sample_interval",f.interval},
            {"timestamp_valid_bits",valid_bits_},{"timestamp_period_ns",period_ns_},
            {"record_cpu_ms",double(record_ns_)/1e6},{"read_cpu_ms",double(read_ns_)/1e6},
            {"retention_complete",pool_!=VK_NULL_HANDLE&&ledger_.Complete()},{"capacity_stopped",ledger_.capacity_stopped()},
            {"considered_batches",c.considered_batches},{"skipped_batches",c.skipped_batches},
            {"unknown_frame_batches",c.unknown_frame_batches},{"prepared_batches",c.prepared_batches},
            {"completed_batches",c.completed_batches},{"records",ledger_.records().size()},
            {"available",c.available},{"unavailable",c.unavailable},{"invalid",c.invalid},
            {"dropped_groups",c.dropped_groups},{"first_dropped_ordinal",c.first_dropped_ordinal},
            {"capacity_stopped_batches",c.capacity_stopped_batches},
            {"maximum_records",ledger_.maximum_records()},{"maximum_children",ledger_.maximum_children()},
            {"retained_children",ledger_.retained_children()},{"allocated_history_bytes",ledger_.history_bytes()},
            {"maximum_history_bytes",Ledger::kMaximumHistoryBytes}};
        if(!details)return out;
        out["shader_pairs"]=Json::array();
        if(dictionary)for(size_t i=0;i<dictionary->pairs().size();++i) {
            const auto& pair=dictionary->pairs()[i];out["shader_pairs"].push_back({{"id",i+1},{"vertex",pair.vertex},{"pixel",pair.pixel}});
        }
        out["samples"]=Json::array();
        for(const auto& r:ledger_.records()) {
            Json j={{"submission",r.submission},{"batch_slot",r.batch_slot},{"physical_ordinal",r.physical_ordinal},
                {"group",r.group},{"leader_token",r.leader_token},{"phase",uint32_t(r.phase)},
                {"prepared_steady_ns",r.prepared_ns},{"receipt_observed_steady_ns",r.submitted_ns},{"retired_steady_ns",r.retired_ns},
                {"selected_api_frame",r.selected_frame},{"first_api_frame",r.first_frame},{"last_api_frame",r.last_frame},
                {"all_frames_known",r.frame_known},{"children",Json::array()},{"inclusive_gpu_ms",nullptr}};
            if(r.interval) {j["inclusive_gpu_ms"]=r.interval->milliseconds;j["begin_tick"]=r.interval->begin_tick;j["end_tick"]=r.interval->end_tick;}
            for(const auto& child:ledger_.children(r)) {
                const auto& m=child.metadata;const auto& t=m.work;
                j["children"].push_back({{"logical_serial",child.logical_serial},{"draw_token",child.draw_token},
                    {"draw",child.draw},{"event",t.event},{"api_frame",t.frame},{"kind",NativeGpuWorkKindName(t.kind)},
                    {"pair",t.pair},{"vertex_variant",m.vertex_variant},{"pixel_variant",m.pixel_variant},
                    {"cohort",m.cohort},{"cohort_known",m.cohort_known},{"fsi_state_specialized",m.fsi_state_specialized},
                    {"queried",t.queried},{"discarded",t.discarded},{"indices",t.indices},
                    {"depth_control",t.depth_control},{"color_mask",t.color_mask},{"blend_mask",t.blend_mask},
                    {"width",t.width},{"height",t.height}});
            }
            out["samples"].push_back(std::move(j));
        }
        return out;
    }
private:
    Ledger ledger_;
    uint32_t capacity_=0,slots_=0,valid_bits_=0;double period_ns_=0;
    VkDevice device_=VK_NULL_HANDLE;VkQueryPool pool_=VK_NULL_HANDLE;
    std::array<std::array<std::unique_ptr<plume::RenderCommandList>,Ledger::kMaximumMarkers>,Ledger::kBatchSlots> markers_;
    uint64_t record_ns_=0,read_ns_=0;
    std::string status_="unavailable";
};
}
