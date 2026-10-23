#pragma once
#include "plume_render_interface.h"
#include "gpu_timestamp.h"
#include "gpu_batch_attribution.h"
#include "gpu_work_frontier.h"
#include "submission_capacity.h"
#include "draw_command_groups.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
namespace gow3::renderer {
class NativeSubmissionGpuTimings;
class NativeGpuWorkError : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
// One FIFO owns every accepted list until its actual fence completes. Optional
// asynchronous pressure submission defaults to two batches in flight; an
// optional four-fence window keeps the identical 64/128-list ownership bounds
// and 32/64-list staging limits. The scratch bound remains 128MiB. Explicit
// Drain still fences all accepted work, irrespective of fence-window size.
class NativeGpuWorkQueue {
public:
    static constexpr uint32_t kMaxLists=NativeSubmissionCapacity::kMaximum;
    static constexpr uint32_t kMaximumInflightBatches=4;
    static constexpr uint64_t kMaxScratchBytes=128u*1024*1024;
    struct Completion {
        uint32_t draws=0,transfers=0;
        uint64_t last_event=0;
        double milliseconds=0;
        uint64_t submission=0;
        std::array<uint64_t,kMaxLists> draw_tokens{};
        bool timings_enabled=false;
        double submit_ms=0,fence_wait_ms=0,resource_retirement_ms=0,driver_receipt_wait_ms=0;
        // Inclusive device interval. Two in-flight intervals may overlap; a
        // sum is not exclusive GPU busy time and is not added to CPU timings.
        std::optional<NativeGpuTimestampInterval> gpu_interval;
    };
    // Fixed numeric attribution; serialization happens only on observation.
    enum class SubmissionReason : uint8_t {
        External,DrawBatchTarget,DrawOwnershipPressure,StagingCapacity,
        ReservationPressure,FrontierCoverage,ExplicitDrain,CoherenceProgress,Count
    };
    enum class WaitReason : uint8_t {
        External,InflightFencePressure,ReservationPressure,DrawOwnershipPressure,
        FrontierCoverage,ExplicitDrain,Count
    };
    struct SubmissionCounter {uint64_t submissions=0,lists=0,draws=0,transfers=0;};
    struct Statistics {
        // Existing timestamp diagnostics opt in; normal execution only takes
        // the disabled branch and creates no extra history or clock samples.
        bool reason_counters_enabled=false;
        std::array<SubmissionCounter,size_t(SubmissionReason::Count)> submission_reasons{};
        std::array<uint64_t,size_t(WaitReason::Count)> wait_reasons{};
        std::array<double,size_t(WaitReason::Count)> wait_reason_ms{};
        uint64_t reserve_calls=0,reserve_list_pressure=0,reserve_scratch_pressure=0;
        uint64_t inflight_fence_pressure=0,draw_slot_pressure=0,draw_upload_pressure=0;
        uint64_t explicit_drain_calls=0,frontier_wait_calls=0;
        uint64_t submissions=0,waits=0,draw_lists=0,transfer_lists=0;
        bool draw_command_groups_enabled=false;
        uint64_t physical_lists=0,grouped_draws=0,draw_command_groups=0;
        uint32_t maximum_group_draws=0,maximum_physical_lists_per_submission=0;
        uint64_t fence_polls=0,poll_retirements=0;
        uint64_t maximum_lists=0,maximum_scratch_bytes=0,maximum_inflight_batches=0;
        bool submission_worker_enabled=false;
        uint64_t driver_receipts=0;
        double driver_receipt_wait_ms=0;
        bool timings_enabled=false,timestamps_supported=false;
        uint32_t timestamp_valid_bits=0;
        double timestamp_period_ns=0;
        std::string timestamp_status="disabled";
        uint64_t gpu_intervals=0,gpu_unavailable=0;
        double gpu_interval_sum_ms=0,submit_ms=0,fence_wait_ms=0,resource_retirement_ms=0;
        double completion_callback_ms=0,timestamp_record_ms=0,timestamp_read_ms=0;
    };
    NativeGpuWorkQueue(plume::RenderDevice&,plume::RenderCommandQueue&,
                      std::function<void(const Completion&)> completed,bool asynchronous=false,
                      bool timestamps=false,uint32_t capacity=NativeSubmissionCapacity::kDefault,
                      bool batch_attribution=false,uint32_t max_inflight_batches=2,bool submission_worker=false,
                      bool physical_timestamps=false,uint32_t physical_first=0,uint32_t physical_end=6000,uint32_t physical_interval=30);
    ~NativeGpuWorkQueue();
    // Before recording query slots/resources: submit a full staging batch and
    // wait only enough oldest fences to make bounded room for the new work.
    void Reserve(uint64_t scratch_bytes=0);
    void EnqueuePinnedDraw(plume::RenderCommandList&,uint64_t event,uint64_t draw_token,
                           const NativeGpuWorkTag* tag=nullptr);
    // Startup-only opt-in. Each child keeps its original logical FIFO serial;
    // only the closed leader is submitted physically. The callback may only
    // end an accepted, fully recorded group, never a preparing current draw.
    void ConfigureDrawCommandGroups(std::function<void()> seal);
    void ValidateDrawCommandGroupAdmission(plume::RenderCommandList&,uint64_t draw_token) const;
    void EnqueuePinnedDrawInGroup(plume::RenderCommandList&,uint64_t event,uint64_t draw_token,
                                 const NativeGpuWorkTag* tag=nullptr);
    void ClosePinnedDrawGroup(plume::RenderCommandList&);
    void FailDrawCommandGroupRecording() noexcept {failed_=true;}
    uint32_t open_group_draws() const {return draw_command_groups_.draws();}
    void EnqueueTransfer(plume::RenderCommandList&,std::shared_ptr<void> lease,
                         uint64_t scratch_bytes,uint64_t event,const NativeGpuWorkTag* tag=nullptr);
    void SubmitPending(SubmissionReason reason=SubmissionReason::External);
    void WaitOldest(WaitReason reason=WaitReason::External);
    // Records the existing Scene pressure decision; never submits or waits.
    void RecordDrawPressure(bool slots,bool upload_bytes) {
        if(statistics_.reason_counters_enabled) {
            statistics_.draw_slot_pressure+=slots;
            statistics_.draw_upload_pressure+=upload_bytes;
        }
    }
    // Retires only already-signaled oldest fences. Never submits staging or
    // waits for a later batch; resources follow the same retirement callback.
    void PollCompleted();
    // Wait only through the batch containing this accepted-list serial.
    void WaitThrough(uint64_t required);
    void Drain();
    // CP-only diagnostic snapshot. Counts accepted logical lists, including
    // pending grouped children, without polling, sealing or copying owners.
    struct AcceptedWorkSnapshot {
        uint64_t draws=0,transfers=0,accepted=0,completed=0;
        bool available=false;
    };
    AcceptedWorkSnapshot ObserveAcceptedWork() const noexcept {
        if(failed_||retiring_||!capacity_.lists||capacity_.lists>kMaxLists||count_>capacity_.lists)return {};
        const auto accepted=frontier_.accepted(),completed=frontier_.completed();
        if(completed>accepted||accepted-completed!=count_||statistics_.draw_lists>completed||
            statistics_.transfer_lists!=completed-statistics_.draw_lists)return {};
        AcceptedWorkSnapshot out{statistics_.draw_lists,statistics_.transfer_lists,accepted,completed,false};
        for(uint32_t i=0;i<count_;++i) {
            if(entries_[(head_+i)%capacity_.lists].draw)++out.draws;else ++out.transfers;
        }
        out.available=out.draws<=accepted&&out.transfers==accepted-out.draws;
        return out;
    }
    uint64_t accepted_work() const { return frontier_.accepted(); }
    // CP-only observation of logical work; includes children in open groups.
    // A submitted frontier is not proof that its fence has signaled.
    struct SubmissionSnapshot {
        uint64_t accepted=0,submitted=0,completed=0;
        bool available=false;
    };
    SubmissionSnapshot ObserveSubmission() const noexcept {
        const auto accepted=frontier_.accepted(),completed=frontier_.completed();
        if(failed_||retiring_||!capacity_.lists||capacity_.lists>kMaxLists||
           count_>capacity_.lists||submitted_count_>count_||completed>accepted||
           accepted-completed!=count_)return {};
        return {accepted,completed+submitted_count_,completed,true};
    }
    uint64_t completed_work() const { return frontier_.completed(); }
    bool WorkCompleted(uint64_t required) const { return frontier_.Reached(required); }
    bool pending() const {return count_!=0;}
    uint32_t pending_lists() const {return count_;}
    uint32_t capacity() const {return capacity_.lists;}
    uint32_t async_batch_lists() const {return capacity_.AsyncBatchLists();}
    uint32_t staged_draws() const {return staged_draws_;}
    uint32_t inflight_batches() const {return batch_count_;}
    uint32_t inflight_batch_limit() const {return max_inflight_batches_;}
    bool failed() const {return failed_;}
    const Statistics& statistics() const {return statistics_;}
    NativeGpuBatchAttribution* attribution() const {return attribution_.get();}
    nlohmann::json PhysicalTimingReport(bool details=false) const;
private:
    struct Entry {
        plume::RenderCommandList* commands=nullptr;
        std::shared_ptr<void> lease;
        uint64_t scratch_bytes=0,event=0,draw_token=0;
        bool draw=false;
        uint64_t draw_group=0;
        uint32_t group_draws=0;
    };
    struct Batch {
        std::unique_ptr<plume::RenderCommandFence> fence;
        uint32_t count=0;
        uint64_t submission=0;
        double submit_ms=0;
        bool driver_receipt_pending=false,physical_timed=false;
    };
    struct TimestampState;
    struct SubmissionWorkerState;
    std::unique_ptr<SubmissionWorkerState> submission_worker_;
    bool CollectSubmission(uint32_t index,bool wait);
    NativeSubmissionCapacity capacity_;
    plume::RenderCommandQueue& queue_;
    std::function<void(const Completion&)> completed_;
    std::array<Entry,kMaxLists> entries_;
    std::array<Batch,kMaximumInflightBatches> batches_;
    std::unique_ptr<TimestampState> timestamps_;
    std::unique_ptr<NativeSubmissionGpuTimings> physical_timestamps_;
    bool physical_timestamps_requested_=false,batch_attribution_enabled_=false;
    // Allocate diagnostics only when requested. No shader/resource owners.
    std::unique_ptr<NativeGpuBatchAttribution> attribution_;
    std::unique_ptr<std::array<NativeGpuWorkTag,kMaxLists>> attribution_tags_;
    uint32_t head_=0,count_=0,submitted_count_=0,staged_draws_=0;
    uint32_t batch_head_=0,batch_count_=0,max_inflight_batches_=1;
    uint64_t scratch_bytes_=0;
    bool asynchronous_=false,failed_=false,retiring_=false;
    Statistics statistics_;
    NativeGpuWorkFrontier frontier_;
    void CheckEnqueue(uint64_t scratch_bytes) const;
    void SubmitStaged(SubmissionReason reason);
    void SealPendingDrawGroup();
    NativeDrawCommandGroups draw_command_groups_;
    std::function<void()> seal_draw_command_group_;
    uint32_t group_leader_index_=0;
    bool sealing_draw_command_group_=false;
    void RetireOldest(bool already_signaled,WaitReason reason=WaitReason::External);
};
}
