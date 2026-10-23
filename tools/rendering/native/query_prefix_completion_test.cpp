#include "renderer/native/gpu_work_frontier.h"
#include <rex/graphics/native_completion.h>
#include <rex/graphics/native_query_tickets.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
using namespace rex::graphics;
namespace {
uint64_t checks=0;
void Require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
template<class F>void Reject(F&& operation,const char* message) {
    bool rejected=false;try {operation();}catch(const std::exception&) {rejected=true;}
    Require(rejected,message);
}
NativeQueryIdentity Identity(uint64_t generation,uint32_t address=0x1000) {
    return {address,generation,9,true};
}
// Independent trace oracle: each command has an explicit retired bit and a
// query end records command indexes, not the production serial arithmetic.
// Scripted GPU sample values are usable only after every required bit is set.
// This tests real frontier, SDK ticket and completion-latch primitives without
// claiming to simulate GPU execution or validate Vulkan fences.
struct Fixture {
    struct Command {uint64_t query,samples;bool retired=false;};
    struct End {NativeQueryIdentity identity;size_t command_end;uint64_t required,samples;};
    NativeGpuWorkFrontier frontier;
    NativeCompletionState completion;
    NativeQueryTickets tickets;
    std::vector<Command> commands;
    std::vector<End> ends;
    size_t consumed=0;
    uint64_t active=0,next=0,fail_generation=0;
    std::map<uint32_t,uint64_t> latest,reports;
    std::vector<uint64_t> writes;
    bool stopping=false;

    void Begin(uint32_t address) {
        Require(!stopping&&!active,"Invalid query admission");
        active=++next;latest[address]=active;tickets.SupersedeAddress(address);
    }
    void CommandList(uint64_t samples,bool query_draw=true) {
        Require(!stopping,"Work admitted after stop");
        commands.push_back({query_draw?active:0,samples,false});
        frontier.Accept();completion.AcceptPending();
    }
    void EndQuery(uint32_t address) {
        Require(active&&!stopping,"End without active query");
        const auto identity=Identity(active,address);
        const auto scope=tickets.BeginEndCallback(identity);
        uint64_t expected=0;
        for(const auto& command:commands)if(command.query==active)expected+=command.samples;
        ends.push_back({identity,commands.size(),frontier.accepted(),expected});
        tickets.Defer(identity);completion.AcceptPending();active=0;
    }
    void Retire(size_t count) {
        const size_t first=std::find_if(commands.begin(),commands.end(),[](const auto& command) {
            return !command.retired;
        })-commands.begin();
        Require(first+count<=commands.size(),"Oracle retirement exceeds command trace");
        frontier.Complete(uint32_t(count));
        for(size_t i=first;i<first+count;++i)commands[i].retired=true;
    }
    void Publish() {
        for(;consumed<ends.size();++consumed) {
            const auto& end=ends[consumed];
            const bool oracle=std::all_of(commands.begin(),commands.begin()+end.command_end,
                [](const auto& command){return command.retired;});
            Require(frontier.Reached(end.required)==oracle,"Frontier differs from independent command trace");
            if(!oracle)return;
            const bool current=latest.at(end.identity.end_address)==end.identity.generation;
            const bool wrote=tickets.Publish(end.identity,[&] {
                Require(current,"Superseded query reached report writer");
                if(end.identity.generation==fail_generation)throw std::runtime_error("Injected publication failure");
                Require(std::find(writes.begin(),writes.end(),end.identity.generation)==writes.end(),
                    "Query published twice");
                reports[end.identity.end_address]=end.samples;writes.push_back(end.identity.generation);
            });
            Require(wrote==current,"Ticket retirement changed address-reuse behavior");
        }
    }
    void Complete() {
        completion.Complete([&] {
            Retire(size_t(frontier.accepted()-frontier.completed()));Publish();
            Require(consumed==ends.size()&&tickets.empty(),"Full completion retained a publication");
        },true);
    }
};
void CheckOrderAndFailure() {
    Fixture f;
    f.CommandList(999,false); // Unqueried draw or transfer must still gate zero.
    f.Begin(0x1000);f.EndQuery(0x1000);
    f.Publish();Require(f.writes.empty(),"Zero query passed unrelated predecessor");
    f.Begin(0x1040);f.CommandList(17);f.EndQuery(0x1040);
    f.Retire(1);f.Publish();
    Require(f.writes==std::vector<uint64_t>{1}&&f.reports.at(0x1000)==0&&
        f.frontier.completed()<f.frontier.accepted()&&f.tickets.size()==1&&f.completion.pending(),
        "Completed zero prefix waited on future work or cleared completion latch");
    f.Begin(0x1080);f.CommandList(23);f.EndQuery(0x1080);
    f.Retire(2);f.fail_generation=3;
    Reject([&]{f.Publish();},"Injected report failure was swallowed");
    Require(f.consumed==2&&f.writes==std::vector<uint64_t>({1,2})&&f.tickets.size()==1&&f.completion.pending(),
        "Partial publication lost one-shot identities");
    f.fail_generation=0;f.Publish();f.Complete();
    Require(f.writes==std::vector<uint64_t>({1,2,3})&&!f.completion.pending()&&f.tickets.empty(),
        "Retry repeated a report or lost the full completion boundary");

    Fixture empty;empty.Begin(0x1000);empty.EndQuery(0x1000);empty.Publish();
    Require(empty.writes.size()==1&&empty.frontier.accepted()==0&&empty.completion.pending(),
        "Lone empty query requires future input or falsely clears the latch");
    empty.stopping=true;empty.Complete();
    Require(!empty.completion.pending(),"Shutdown failed to drain accepted empty query");

    const auto completed=f.frontier.completed();
    Reject([&]{f.frontier.Complete(1);},"Unaccepted work was retired");
    Require(f.frontier.completed()==completed,"Rejected retirement changed frontier");
    Reject([&]{f.frontier.Reached(f.frontier.accepted()+1);},"Future query watermark was accepted");
}
void CheckBoundedSchedules() {
    uint32_t random=0xdecafbad;
    auto pick=[&] {random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned schedule=0;schedule<512;++schedule) {
        Fixture f;
        for(unsigned query=0;query<96;++query) {
            const uint32_t address=0x1000+(pick()%8)*64;
            // Exact same-address generations may overlap; only latest writes.
            f.Begin(address);
            const auto count=pick()%5;
            for(unsigned draw=0;draw<count;++draw)f.CommandList(pick()%10000);
            if(pick()&1)f.CommandList(0,false); // Transfer/unqueried predecessor.
            f.EndQuery(address);
            const size_t pending=f.frontier.accepted()-f.frontier.completed();
            if(pending)f.Retire(pick()%(pending+1));
            f.Publish();
            Require(f.tickets.size()<=NativeQueryTickets::kCapacity,"Unbounded publication ownership");
            Require(f.completion.pending(),"Prefix publication cleared accepted-work latch");
        }
        f.stopping=true;f.Complete();
        Require(f.consumed==f.ends.size()&&f.tickets.empty()&&!f.completion.pending(),
            "Random retirement schedule lost exact query completion");
    }
    Fixture capacity;
    capacity.CommandList(0,false);
    for(uint32_t i=0;i<NativeQueryTickets::kCapacity;++i) {
        capacity.Begin(0x1000+i*64);capacity.EndQuery(0x1000+i*64);
    }
    Require(capacity.tickets.full(),"Zero-query pressure bypassed ticket bound");
    capacity.CommandList(0,false); // A newer unrelated tail is not needed.
    capacity.Retire(1);capacity.Publish();
    Require(capacity.tickets.empty()&&capacity.writes.size()==NativeQueryTickets::kCapacity&&
        capacity.frontier.completed()==1&&capacity.frontier.accepted()==2&&capacity.completion.pending(),
        "Capacity relief waited on newer work or cleared its ownership");
    capacity.Complete();
}
}
int main() try {
    CheckOrderAndFailure();CheckBoundedSchedules();
    std::cout<<"PASS: "<<checks<<" independent command-trace/frontier/ticket checks, 512 mixed/reused-address schedules, "
        "zero predecessors, capacity, partial publication failure/retry and terminal latch\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
