#pragma once
#include "submission_capacity.h"
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace gow3::renderer {
// Logical FIFO entries remain independently bounded/accepted/retired. Only a
// closed leader contributes a physical command buffer to its submission. This
// helper contains no GPU owners, callbacks or allocations.
struct NativeDrawGroupEntry {
    uintptr_t command=0;
    uint64_t group=0;
    uint32_t group_draws=0;
    bool draw=false;
    uint64_t draw_token=0;
};
struct NativeDrawGroupPlan {
    std::array<uint32_t,NativeSubmissionCapacity::kMaximum> leaders{};
    uint32_t physical_lists=0,logical_draws=0,logical_transfers=0,groups=0;
};
class NativeDrawCommandGroups {
public:
    static constexpr uint32_t kMaximumDraws=16;
    struct Admission {uint64_t group=0;bool leader=false;};
    struct Closed {uint64_t group=0;uint32_t draws=0;};
    struct Statistics {uint64_t draws=0,groups=0;uint32_t maximum_draws=0;};
    Admission Preview(uintptr_t command) const {
        if(!command)throw std::logic_error("Native draw group has no command buffer");
        if(command_) {
            if(command_!=command)throw std::logic_error("Native draw group changed its leader command buffer");
            if(draws_>=kMaximumDraws)throw std::logic_error("Native draw command group exceeds sixteen draws");
            return {group_,false};
        }
        if(next_group_==std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("Native draw command group generation exhausted");
        return {next_group_+1,true};
    }
    Admission Accept(uintptr_t command) {
        const auto accepted=Preview(command);
        if(accepted.leader){command_=command;group_=++next_group_;draws_=0;}
        ++draws_;++statistics_.draws;
        return accepted;
    }
    Closed Seal(uintptr_t command) {
        if(!command_||command_!=command||!draws_)
            throw std::logic_error("Native draw group seal lost its exact leader");
        const Closed result{group_,draws_};
        ++statistics_.groups;
        if(draws_>statistics_.maximum_draws)statistics_.maximum_draws=draws_;
        command_=0;group_=0;draws_=0;
        return result;
    }
    uintptr_t command() const {return command_;}
    uint64_t group() const {return group_;}
    uint32_t draws() const {return draws_;}
    const Statistics& statistics() const {return statistics_;}
    static NativeDrawGroupPlan Plan(std::span<const NativeDrawGroupEntry> entries) {
        if(entries.size()>NativeSubmissionCapacity::kMaximum)
            throw std::logic_error("Native draw group plan exceeds logical FIFO capacity");
        NativeDrawGroupPlan plan;
        for(uint32_t i=0;i<entries.size();++i) {
            if(entries[i].draw!=bool(entries[i].draw_token))
                throw std::logic_error("Native draw group has an invalid draw ownership token");
            if(entries[i].draw)
                for(uint32_t j=0;j<i;++j)
                    if(entries[j].draw&&entries[j].draw_token==entries[i].draw_token)
                        throw std::logic_error("Native draw group repeats a logical ownership token");
        }
        for(uint32_t i=0;i<entries.size();) {
            const auto& first=entries[i];
            if(!first.command)throw std::logic_error("Native draw group has an orphan follower");
            for(uint32_t prior=0;prior<plan.physical_lists;++prior)
                if(entries[plan.leaders[prior]].command==first.command)
                    throw std::logic_error("Native submission repeats one physical command buffer");
            plan.leaders[plan.physical_lists++]=i;
            if(!first.group) {
                if(first.group_draws)throw std::logic_error("Native ungrouped work carries a group count");
                if(first.draw)++plan.logical_draws;else ++plan.logical_transfers;
                ++i;continue;
            }
            if(!first.draw||!first.group_draws||first.group_draws>kMaximumDraws)
                throw std::logic_error("Native draw group leader is open or invalid");
            if(first.group_draws>entries.size()-i)
                throw std::logic_error("Native submission splits a physical draw group");
            for(uint32_t prior=0;prior+1<plan.physical_lists;++prior)
                if(entries[plan.leaders[prior]].group==first.group)
                    throw std::logic_error("Native draw group generation is reused after another leader");
            for(uint32_t j=1;j<first.group_draws;++j) {
                const auto& follower=entries[i+j];
                if(!follower.draw||follower.group!=first.group||follower.command||follower.group_draws)
                    throw std::logic_error("Native draw group has an invalid logical follower");
            }
            plan.logical_draws+=first.group_draws;++plan.groups;i+=first.group_draws;
        }
        return plan;
    }
private:
    uintptr_t command_=0;
    uint64_t group_=0,next_group_=0;
    uint32_t draws_=0;
    Statistics statistics_;
};
} // namespace gow3::renderer
