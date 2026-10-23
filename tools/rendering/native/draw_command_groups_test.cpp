#include "renderer/native/draw_command_groups.h"
#include "renderer/native/gpu_work_frontier.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>

using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
template<class F>void Reject(F&& work,const char* why) {
    bool rejected=false;try {work();}catch(const std::exception&) {rejected=true;}
    Require(rejected,why);
}
struct GroupState {
    uintptr_t command;uint64_t group;uint32_t draws;
    NativeDrawCommandGroups::Statistics statistics;
};
GroupState State(const NativeDrawCommandGroups& groups) {
    return {groups.command(),groups.group(),groups.draws(),groups.statistics()};
}
bool Same(const GroupState& a,const GroupState& b) {
    return a.command==b.command&&a.group==b.group&&a.draws==b.draws&&
        a.statistics.draws==b.statistics.draws&&a.statistics.groups==b.statistics.groups&&
        a.statistics.maximum_draws==b.statistics.maximum_draws;
}
void AdmissionAndRefusal() {
    NativeDrawCommandGroups groups;
    const auto empty=State(groups);
    Reject([&]{groups.Preview(0);},"Null physical leader admitted");
    Reject([&]{groups.Seal(1);},"Empty group sealed");
    Require(Same(empty,State(groups)),"Empty refusal changed group state");
    const auto first=groups.Preview(1),again=groups.Preview(1);
    Require(first.leader&&first.group==1&&again.group==first.group&&Same(empty,State(groups)),
        "Preview mutated admission generation or statistics");
    for(uint32_t i=0;i<NativeDrawCommandGroups::kMaximumDraws;++i) {
        const auto before=State(groups);const auto preview=groups.Preview(1);
        Require(preview.group==1&&preview.leader==(i==0)&&Same(before,State(groups)),
            "Repeated Preview changed current leader");
        const auto admitted=groups.Accept(1);
        Require(admitted.group==preview.group&&admitted.leader==preview.leader&&groups.draws()==i+1,
            "Accept differed from its exact Preview");
    }
    const auto full=State(groups);
    Reject([&]{groups.Accept(1);},"Seventeenth child admitted");
    Reject([&]{groups.Accept(2);},"Foreign physical buffer entered open group");
    Reject([&]{groups.Seal(2);},"Foreign physical buffer sealed another leader");
    Require(Same(full,State(groups)),"Rejected child or seal changed accepted group");
    const auto closed=groups.Seal(1);
    Require(closed.group==1&&closed.draws==16&&!groups.command()&&!groups.group()&&!groups.draws()&&
        groups.statistics().draws==16&&groups.statistics().groups==1&&groups.statistics().maximum_draws==16,
        "Exact sixteen-child seal lost logical accounting");
    const auto sealed=State(groups);
    Reject([&]{groups.Seal(1);},"Stale leader sealed twice");
    Require(Same(sealed,State(groups)),"Stale seal changed group state");
    const auto reuse=groups.Accept(1);
    Require(reuse.leader&&reuse.group==2,"Retired physical handle reuse repeated logical generation");
    Require(groups.Seal(1).draws==1,"One-child group changed mapping");
}
NativeDrawGroupEntry Draw(uintptr_t command,uint64_t group,uint32_t draws,uint64_t token) {
    return {command,group,draws,true,token};
}
void RejectedPlans() {
    const std::array good{Draw(11,1,2,1),Draw(0,1,0,2),
        NativeDrawGroupEntry{22,0,0,false,0},Draw(33,2,1,3),Draw(44,0,0,4)};
    const auto plan=NativeDrawCommandGroups::Plan(good);
    Require(plan.physical_lists==4&&plan.logical_draws==4&&plan.logical_transfers==1&&plan.groups==2&&
        plan.leaders[0]==0&&plan.leaders[1]==2&&plan.leaders[2]==3&&plan.leaders[3]==4,
        "Mixed singleton/transfer/group partition is wrong");
    auto reject_change=[&](size_t index,NativeDrawGroupEntry replacement,const char* why) {
        auto bad=good;bad[index]=replacement;
        Reject([&]{NativeDrawCommandGroups::Plan(bad);},why);
        const auto unchanged=NativeDrawCommandGroups::Plan(good);
        Require(unchanged.physical_lists==plan.physical_lists&&unchanged.logical_draws==plan.logical_draws&&
            unchanged.logical_transfers==plan.logical_transfers&&unchanged.groups==plan.groups&&
            unchanged.leaders==plan.leaders,"Failed planning changed immutable mapping input");
    };
    reject_change(0,Draw(11,1,0,1),"Open group accepted for submission");
    reject_change(0,Draw(11,1,17,1),"Oversized physical group accepted");
    reject_change(0,Draw(11,1,6,1),"Physical group split past staged span");
    reject_change(0,Draw(0,1,2,1),"Orphan follower accepted as leader");
    reject_change(1,Draw(55,1,0,2),"Follower carrying physical commands accepted");
    reject_change(1,Draw(0,2,0,2),"Foreign group follower accepted");
    reject_change(1,Draw(0,1,1,2),"Follower carrying closure count accepted");
    reject_change(1,NativeDrawGroupEntry{0,1,0,false,0},"Transfer swallowed as grouped draw");
    reject_change(2,NativeDrawGroupEntry{22,1,1,false,0},"Transfer admitted as group leader");
    reject_change(2,NativeDrawGroupEntry{22,0,1,false,0},"Ungrouped transfer carried group closure");
    reject_change(2,NativeDrawGroupEntry{22,0,0,false,99},"Transfer carried draw ownership token");
    reject_change(3,Draw(33,1,1,3),"Repeated group generation accepted in one staged span");
    reject_change(3,Draw(11,2,1,3),"Repeated physical command buffer submitted twice");
    reject_change(3,Draw(33,2,1,2),"Repeated logical draw token accepted");
    reject_change(4,Draw(44,0,0,0),"Draw with absent ownership token accepted");
    reject_change(4,Draw(0,0,0,4),"Ungrouped null command admitted");
    Reject([&]{NativeDrawCommandGroups::Plan(std::span(good).first(1));},"Prefix split a two-child physical group");
    Reject([&]{NativeDrawCommandGroups::Plan(std::span(good).subspan(1));},"Suffix orphaned group followers");
    std::array<NativeDrawGroupEntry,129> oversized;
    for(uint32_t i=0;i<oversized.size();++i)oversized[i]={i+1,0,0,false,0};
    Reject([&]{NativeDrawCommandGroups::Plan(oversized);},"Logical submission grew past128 entries");
    const auto zero=NativeDrawCommandGroups::Plan({});
    Require(!zero.physical_lists&&!zero.logical_draws&&!zero.logical_transfers&&!zero.groups,
        "Empty staged prefix invented physical work");
}
// Independent ownership oracle: explicit logical positions retain an owner;
// each physical command identity lists all positions it executes. The helper's
// closure count/leader-array arithmetic is compared against these trace groups.
// Fence retirement is modeled, not a claim to test Vulkan or the Scene latch.
struct Trace {
    struct Oracle {uintptr_t physical=0;bool draw=false,retired=false;};
    struct Slot {NativeDrawGroupEntry entry;size_t oracle=0;std::shared_ptr<int> owner;};
    struct Batch {uint32_t logical;std::vector<size_t> oracle;};
    NativeSubmissionCapacity capacity;
    NativeDrawCommandGroups groups;
    NativeGpuWorkFrontier frontier;
    std::array<Slot,NativeSubmissionCapacity::kMaximum> ring{};
    std::vector<Oracle> oracle;
    std::vector<Batch> batches;
    std::vector<std::weak_ptr<int>> weak;
    uint32_t head=0,count=0,submitted=0;
    uint64_t token=0;size_t open_leader=0;bool failed=false;
    explicit Trace(uint32_t logical):capacity(logical,true) {}
    Slot& At(uint32_t offset) {return ring[(head+offset)%capacity.lists];}
    void Admit(uintptr_t command,bool draw) {
        if(failed)throw std::logic_error("Modeled accepted failure forbids new work");
        if(count==capacity.lists)throw std::logic_error("Modeled logical queue is full");
        NativeDrawGroupEntry entry;
        if(draw) {
            const auto a=groups.Accept(command);entry=Draw(a.leader?command:0,a.group,0,++token);
            if(a.leader)open_leader=count;
        } else {Seal();entry={command,0,0,false,0};}
        const size_t index=oracle.size();oracle.push_back({command,draw,false});
        auto owner=std::make_shared<int>(int(index));weak.push_back(owner);
        At(count)={entry,index,std::move(owner)};++count;frontier.Accept();
    }
    void Seal() {
        if(!groups.command())return;
        const auto closed=groups.Seal(groups.command());
        auto& leader=At(uint32_t(open_leader));
        Require(leader.entry.group==closed.group&&leader.entry.command,"Seal did not find exact ring leader");
        leader.entry.group_draws=closed.draws;
    }
    std::vector<NativeDrawGroupEntry> Entries() {
        std::vector<NativeDrawGroupEntry> entries;
        for(uint32_t i=submitted;i<count;++i)entries.push_back(At(i).entry);
        return entries;
    }
    void Submit(bool injected_failure=false) {
        if(failed)throw std::logic_error("Modeled accepted failure forbids resubmission");
        Seal();const auto entries=Entries();const auto plan=NativeDrawCommandGroups::Plan(entries);
        std::vector<uint32_t> expected;
        uintptr_t previous=0;uint32_t draws=0,transfers=0,physical_groups=0;
        for(uint32_t i=0;i<entries.size();++i) {
            const auto& o=oracle[At(submitted+i).oracle];
            if(i==0||o.physical!=previous)expected.push_back(i);
            previous=o.physical;
            if(o.draw)++draws;else ++transfers;
        }
        for(const auto i:expected)if(entries[i].group)++physical_groups;
        Require(plan.physical_lists==expected.size()&&plan.logical_draws==draws&&
            plan.logical_transfers==transfers&&plan.groups==physical_groups,
            "Physical plan differs from explicit independent trace partition");
        for(uint32_t i=0;i<expected.size();++i)Require(plan.leaders[i]==expected[i],"Plan reordered physical leaders");
        if(injected_failure) {failed=true;return;}
        if(entries.empty())return;
        Batch batch{uint32_t(entries.size()),{}};
        for(uint32_t i=submitted;i<count;++i)batch.oracle.push_back(At(i).oracle);
        batches.push_back(std::move(batch));submitted=count;
    }
    void RetireOldest() {
        Require(!failed&&!batches.empty(),"Invalid modeled successful fence retirement");
        const auto batch=batches.front();
        for(const auto index:batch.oracle)Require(!oracle[index].retired&&!weak[index].expired(),
            "Physical fence owner released or retired twice before proof");
        // Every explicit physical command is wholly contained in this fence.
        for(const auto index:batch.oracle)for(size_t other=0;other<oracle.size();++other)
            if(!oracle[other].retired&&oracle[other].physical==oracle[index].physical)
                Require(std::find(batch.oracle.begin(),batch.oracle.end(),other)!=batch.oracle.end(),
                    "Fence completion split physical command ownership");
        for(uint32_t i=0;i<batch.logical;++i) {auto& slot=At(i);oracle[slot.oracle].retired=true;slot={};}
        frontier.Complete(batch.logical);head=(head+batch.logical)%capacity.lists;
        count-=batch.logical;submitted-=batch.logical;
        if(groups.command()) {Require(open_leader>=batch.logical,"Retirement reached a preparing open leader");open_leader-=batch.logical;}
        else open_leader=0;
        batches.erase(batches.begin());
        for(const auto index:batch.oracle)Require(weak[index].expired(),"Completed fence retained modeled immutable input");
    }
    bool QueryReady(uint64_t required) {
        const bool explicit_ready=std::all_of(oracle.begin(),oracle.begin()+size_t(required),
            [](const auto& command){return command.retired;});
        Require(frontier.Reached(required)==explicit_ready,"Logical query frontier differs from retired trace positions");
        return explicit_ready;
    }
};
void CapacityRingAndFrontiers(uint32_t capacity) {
    Trace t(capacity);uintptr_t physical=1000;
    auto half_batch=[&] {
        const uint32_t target=capacity/2;
        // One16-child group + transfer + another group + singleton transfer
        // fills32; a64-entry batch repeats the same independent pattern.
        for(uint32_t used=0;used<target;used+=32) {
            const auto first=++physical;
            for(unsigned draw=0;draw<16;++draw)t.Admit(first,true);
            t.Admit(++physical,false);
            const auto second=++physical;
            for(unsigned draw=0;draw<14;++draw)t.Admit(second,true);
            t.Admit(++physical,false);
        }
        t.Submit();
    };
    half_batch();half_batch();
    for(unsigned cycle=0;cycle<16;++cycle) {
        Require(t.count==capacity&&t.submitted==capacity&&t.batches.size()==2,
            "Logical64/128 capacity changed after physical consolidation");
        const auto before=t.frontier.accepted();
        Reject([&]{t.Admit(++physical,true);},"Grouped draws bypassed logical ownership capacity");
        Require(t.frontier.accepted()==before&&t.count==capacity,"Capacity refusal advanced logical frontier");
        const auto oldest_end=t.frontier.completed()+capacity/2;
        Require(!t.QueryReady(oldest_end),"Query completed before its covering fence");
        t.RetireOldest();Require(t.QueryReady(oldest_end)&&!t.QueryReady(before),
            "Oldest query waited on or falsely retired newer accepted work");
        // Refill while the second batch is still owned; alternate head0 and
        // head32/64 to reuse ring positions beyond the physical array end.
        half_batch();
    }
    while(!t.batches.empty())t.RetireOldest();
    Require(t.frontier.accepted()>capacity*4&&t.frontier.completed()==t.frontier.accepted()&&
        !t.count&&!t.submitted,"Ring-wrap scenario lost logical work or immutable owners");
    Trace prefix(capacity);
    for(unsigned draw=0;draw<8;++draw)prefix.Admit(51,true);
    const auto inside=prefix.frontier.accepted();
    for(unsigned draw=0;draw<8;++draw)prefix.Admit(51,true);
    prefix.Submit();
    prefix.Admit(52,false); // Newer unrelated staged tail remains owned.
    Require(!prefix.QueryReady(inside),"Query watermark inside group completed before physical fence");
    prefix.RetireOldest();
    Require(prefix.QueryReady(inside)&&prefix.count==1&&prefix.submitted==0&&
        !prefix.QueryReady(prefix.frontier.accepted()),"Whole group fence falsely retired newer staged tail");
    prefix.Submit();prefix.RetireOldest();
    Trace failure(capacity);
    failure.Admit(61,true);failure.Admit(61,true);failure.Admit(62,false);
    const auto accepted=failure.frontier.accepted();failure.Submit(true);
    Require(failure.failed&&failure.count==3&&failure.submitted==0&&failure.frontier.accepted()==accepted&&
        failure.frontier.completed()==0&&!failure.QueryReady(accepted)&&
        std::all_of(failure.weak.begin(),failure.weak.end(),[](const auto& owner){return !owner.expired();}),
        "Injected accepted submit failure released or falsely completed a logical owner");
    Reject([&]{failure.Admit(63,true);},"Modeled failure allowed fresh admission");
    Reject([&]{failure.Submit();},"Modeled failure replayed uncertain accepted work");
}
}
int main() try {
    AdmissionAndRefusal();RejectedPlans();
    CapacityRingAndFrontiers(64);CapacityRingAndFrontiers(128);
    std::cout<<"PASS: "<<checks<<" bounded group admission/partition and independent logical-owner/frontier checks; "
        "64/128 capacity, sixteen children, ring wrap, transfers, prefix fences, malformed groups and modeled submit failure\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
