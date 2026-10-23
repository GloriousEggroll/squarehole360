#pragma once
#include "draw_command_groups.h"
#include "gpu_batch_attribution.h"
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>

namespace gow3::renderer {
// Pure numeric diagnostic. The adapter supplies the EXISTING fence proof;
// this class cannot poll, wait, submit, close a group, or retain GPU resources.
// Samples whole submissions containing a matching known API frame. Matching
// one child retains every physical list, including mixed/unknown frame work.
class NativeSubmissionTimingLedger {
public:
    static constexpr uint32_t kCapacity=NativeSubmissionCapacity::kMaximum;
    static constexpr uint32_t kBatchSlots=4,kMaximumMarkers=kCapacity+1;
    static constexpr uint32_t kMaximumCommandLists=2*kCapacity+1;
    static constexpr uint64_t kMaximumHistoryBytes=128u*1024*1024;
    enum class Phase:uint8_t { Prepared,Submitted,Completed,Poisoned };
    // The source pair and actual variants/cohort are distinct. Unknown stays
    // unknown; neither null PS nor a source hash authenticates the selected PSO.
    struct Metadata {
        NativeGpuWorkTag work;
        uint32_t vertex_variant=0,pixel_variant=0,cohort=0;
        bool cohort_known=false,fsi_state_specialized=false;
    };
    struct Child {
        uint64_t logical_serial=0,draw_token=0;
        bool draw=false;
        Metadata metadata;
    };
    struct Record {
        uint64_t submission=0,group=0,leader_token=0;
        uint64_t prepared_ns=0,submitted_ns=0,retired_ns=0;
        uint32_t batch_slot=0,physical_ordinal=0,child_count=0;
        uint32_t selected_frame=UINT32_MAX,first_frame=UINT32_MAX,last_frame=0;
        bool frame_known=true;
        Phase phase=Phase::Prepared;
        size_t first_child=0;
        std::optional<NativeGpuTimestampInterval> interval;
    };
    struct Marker {
        // UINT32_MAX means absent; boundary0 resets the entire exact range.
        uint32_t end_query=UINT32_MAX,begin_query=UINT32_MAX;
        uint32_t reset_first=0,reset_count=0;
    };
    struct Plan {
        NativeDrawGroupPlan work;
        uint32_t first_query=0,query_count=0,command_count=0;
        std::array<Marker,kMaximumMarkers> markers{};
        // Order is marker0, unchanged physical leader0, marker1, leader1,...
        uint32_t WorkCommandIndex(uint32_t i) const {return 2*i+1;}
        uint32_t MarkerCommandIndex(uint32_t i) const {return 2*i;}
    };
    struct Counters {
        uint64_t examined_groups=0,dropped_groups=0,first_dropped_ordinal=0;
        uint64_t prepared_batches=0,completed_batches=0,available=0,unavailable=0;
        uint64_t invalid=0;
        uint64_t considered_batches=0,skipped_batches=0,unknown_frame_batches=0;
        uint64_t capacity_stopped_batches=0;
    };
    struct FrameSelection {
        uint32_t first=0,end=6000,interval=30;
        bool Contains(uint32_t frame) const noexcept {
            return frame!=UINT32_MAX&&frame>=first&&frame<end&&(frame-first)%interval==0;
        }
    };
    template<class T> static uint32_t Interleave(std::span<T> lists,uint32_t physical,
        uint32_t outer_markers,std::span<const T> markers) {
        const auto offset=outer_markers?1u:0u,total=2*physical+1+outer_markers;
        if(!physical||physical>kCapacity||(outer_markers!=0&&outer_markers!=2)||
           markers.size()!=physical+1||lists.size()<total)
            throw std::invalid_argument("Invalid physical timestamp interleave");
        for(uint32_t i=physical;i>0;--i) {
            lists[offset+2*(i-1)+1]=lists[offset+i-1];
            lists[offset+2*(i-1)]=markers[i-1];
        }
        lists[offset+2*physical]=markers[physical];
        return total;
    }
    static Plan MakePlan(std::span<const NativeDrawGroupEntry> logical,
                         uint32_t capacity,uint32_t slot,uint32_t slots) {
        if((capacity!=64&&capacity!=128)||!slots||slots>kBatchSlots||slot>=slots||
           logical.empty()||logical.size()>capacity)
            throw std::invalid_argument("Invalid physical timestamp bounds");
        Plan plan;plan.work=NativeDrawCommandGroups::Plan(logical);
        const auto count=plan.work.physical_lists;
        plan.first_query=slot*2*capacity;plan.query_count=2*count;
        plan.command_count=2*count+1;
        for(uint32_t i=0;i<=count;++i) {
            auto& marker=plan.markers[i];
            if(i)marker.end_query=plan.first_query+2*i-1;
            else {marker.reset_first=plan.first_query;marker.reset_count=plan.query_count;}
            if(i<count)marker.begin_query=plan.first_query+2*i;
        }
        return plan;
    }
    NativeSubmissionTimingLedger(uint32_t capacity,uint32_t slots,size_t maximum_records,FrameSelection frames)
        :capacity_(capacity),slot_count_(slots),maximum_records_(maximum_records),frames_(frames) {
        if((capacity!=64&&capacity!=128)||!slots||slots>kBatchSlots||!maximum_records||
           maximum_records>(kMaximumHistoryBytes-sizeof(*this))/(sizeof(Record)+2*sizeof(Child))||!frames.interval||
           frames.first>=frames.end||frames.end==UINT32_MAX)
            throw std::invalid_argument("Invalid physical timestamp history bounds");
        maximum_children_=2*maximum_records;
        history_=std::make_unique<Record[]>(maximum_records);
        children_=std::make_unique<Child[]>(maximum_children_);
    }
    // Called after the original natural close and physical-plan validation,
    // before the original execute call. A skipped/failed diagnostic never asks
    // the queue to change its batch, make room, or wait.
    bool Prepare(uint32_t slot,uint64_t generation,uint64_t first_serial,
                 std::span<const NativeDrawGroupEntry> logical,
                 std::span<const Metadata> metadata,uint64_t now) noexcept {
        ++counters_.considered_batches;
        if(disabled_)return false;
        if(capacity_stopped_) {++counters_.capacity_stopped_batches;return false;}
        try {
            if(slot>=slot_count_||slots_[slot].busy||!generation||generation<=last_generation_||
               !first_serial||first_serial<=last_serial_||first_serial>UINT64_MAX-logical.size()||metadata.size()!=logical.size())
                throw std::logic_error("Invalid diagnostic submission ownership");
            uint32_t selected_frame=UINT32_MAX;bool known=false;
            for(const auto& tag:metadata) {
                known|=tag.work.frame!=UINT32_MAX;
                if(frames_.Contains(tag.work.frame))selected_frame=std::min(selected_frame,tag.work.frame);
            }
            if(selected_frame==UINT32_MAX) {
                ++counters_.skipped_batches;if(!known)++counters_.unknown_frame_batches;return false;
            }
            const auto plan=MakePlan(logical,capacity_,slot,slot_count_);
            const auto count=plan.work.physical_lists;
            const auto first_ordinal=counters_.examined_groups+1;
            counters_.examined_groups+=count;
            if(count>maximum_records_-size_||logical.size()>maximum_children_-children_size_) {
                counters_.dropped_groups+=count;
                if(!counters_.first_dropped_ordinal)counters_.first_dropped_ordinal=first_ordinal;
                capacity_stopped_=true;
                return false; // Whole batch omitted; never overwrite a record.
            }
            auto& owner=slots_[slot];owner.busy=true;owner.submitted=false;
            owner.generation=generation;owner.first=size_;owner.count=count;owner.plan=plan;
            last_generation_=generation;
            last_serial_=first_serial+logical.size()-1;
            for(uint32_t p=0;p<count;++p) {
                auto& record=history_[size_++];
                const auto leader=plan.work.leaders[p];
                const auto end=p+1<count?plan.work.leaders[p+1]:uint32_t(logical.size());
                record.submission=generation;record.group=logical[leader].group;
                record.leader_token=logical[leader].draw_token;record.prepared_ns=now;
                record.batch_slot=slot;record.physical_ordinal=p;record.child_count=end-leader;
                record.selected_frame=selected_frame;
                record.first_child=children_size_;
                for(uint32_t i=leader;i<end;++i) {
                    children_[children_size_++]={first_serial+i,logical[i].draw_token,logical[i].draw,metadata[i]};
                    if(metadata[i].work.frame==UINT32_MAX)record.frame_known=false;
                    else {record.first_frame=std::min(record.first_frame,metadata[i].work.frame);
                          record.last_frame=std::max(record.last_frame,metadata[i].work.frame);}
                }
            }
            ++counters_.prepared_batches;return true;
        } catch(...) {Disable();return false;}
    }
    const Plan* PreparedPlan(uint32_t slot,uint64_t generation) const noexcept {
        if(slot>=slot_count_)return nullptr;
        const auto& owner=slots_[slot];
        return owner.busy&&owner.generation==generation?&owner.plan:nullptr;
    }
    uint64_t PreparedTime(uint32_t slot,uint64_t generation) const noexcept {
        return PreparedPlan(slot,generation)?history_[slots_[slot].first].prepared_ns:0;
    }
    bool Submitted(uint32_t slot,uint64_t generation,uint64_t time) noexcept {
        if(!Match(slot,generation)||slots_[slot].submitted)return Fail();
        auto& owner=slots_[slot];
        if(time<history_[owner.first].prepared_ns)return Fail();
        owner.submitted=true;
        for(uint32_t i=0;i<owner.count;++i) {
            auto& record=history_[owner.first+i];record.phase=Phase::Submitted;record.submitted_ns=time;
        }
        return true;
    }
    // Result arrays are obtained by the adapter only AFTER the original queue
    // fence signals. The caller must not turn this API into a new fence wait.
    // Validate the whole batch before releasing any diagnostic query range.
    bool CompleteAfterOriginalFence(uint32_t slot,uint64_t generation,
        std::span<const std::optional<NativeGpuTimestampInterval>> results,uint64_t now) noexcept {
        if(!Match(slot,generation)||!slots_[slot].submitted)return Fail();
        auto& owner=slots_[slot];
        if(results.size()!=owner.count||now<history_[owner.first].submitted_ns)return Fail();
        for(uint32_t i=0;i<slot_count_;++i)
            if(slots_[i].busy&&slots_[i].generation<generation)return Fail();
        for(const auto& result:results)
            if(result&&(!std::isfinite(result->milliseconds)||result->milliseconds<0))return Fail();
        for(uint32_t i=0;i<owner.count;++i) {
            auto& record=history_[owner.first+i];record.phase=Phase::Completed;
            record.retired_ns=now;record.interval=results[i];
            if(results[i])++counters_.available;else ++counters_.unavailable;
        }
        owner.busy=false;++counters_.completed_batches;return true;
    }
    // Marker recording/submission failure poisons the range. Never recycle
    // uncertain GPU ownership, even if other previously submitted batches retire.
    void Poison(uint32_t slot,uint64_t generation) noexcept {
        if(slot<slot_count_&&slots_[slot].busy&&slots_[slot].generation==generation)
            for(uint32_t i=0;i<slots_[slot].count;++i)history_[slots_[slot].first+i].phase=Phase::Poisoned;
        Disable();
    }
    bool Busy(uint32_t slot) const noexcept {return slot<slot_count_&&slots_[slot].busy;}
    bool disabled() const noexcept {return disabled_;}
    bool capacity_stopped() const noexcept {return capacity_stopped_;}
    const FrameSelection& frames() const noexcept {return frames_;}
    const Counters& counters() const noexcept {return counters_;}
    std::span<const Record> records() const noexcept {return {history_.get(),size_};}
    std::span<const Child> children(const Record& record) const noexcept {
        if(record.first_child>children_size_||record.child_count>children_size_-record.first_child)return {};
        return {children_.get()+record.first_child,record.child_count};
    }
    size_t maximum_records() const noexcept {return maximum_records_;}
    size_t maximum_children() const noexcept {return maximum_children_;}
    size_t retained_children() const noexcept {return children_size_;}
    uint64_t history_bytes() const noexcept {return sizeof(*this)+maximum_records_*sizeof(Record)+maximum_children_*sizeof(Child);}
    bool Complete() const noexcept {
        if(disabled_||counters_.dropped_groups)return false;
        for(uint32_t i=0;i<slot_count_;++i)if(slots_[i].busy)return false;
        return true; // Metadata retention; unavailable intervals stay explicit.
    }
private:
    struct Slot {bool busy=false,submitted=false;uint64_t generation=0;size_t first=0;uint32_t count=0;Plan plan;};
    bool Match(uint32_t slot,uint64_t generation) const noexcept {
        return slot<slot_count_&&generation&&slots_[slot].busy&&slots_[slot].generation==generation&&
               history_[slots_[slot].first].phase!=Phase::Poisoned;
    }
    void Disable() noexcept {disabled_=true;++counters_.invalid;}
    bool Fail() noexcept {Disable();return false;}
    uint32_t capacity_=0,slot_count_=0;size_t maximum_records_=0,size_=0;
    size_t maximum_children_=0,children_size_=0;
    std::unique_ptr<Record[]> history_;
    std::unique_ptr<Child[]> children_;
    std::array<Slot,kBatchSlots> slots_{};
    Counters counters_;
    FrameSelection frames_;
    uint64_t last_generation_=0,last_serial_=0;
    bool capacity_stopped_=false,disabled_=false;
};
}
