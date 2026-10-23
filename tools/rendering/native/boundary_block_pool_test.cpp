#include "renderer/native/boundary_block_pool.h"
#include <iostream>
#include <stdexcept>

using namespace gow3::renderer;
int main() {
    size_t checks=0;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Boundary pool ownership/budget regression");};
    NativeBoundaryBlockPool pool;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> held;
    std::vector<GraphicsBoundaryEvent> moved_events,copied_events;
    for(unsigned frame=0;frame<256;++frame) {
        GraphicsBoundaryEvent event;pool.Begin(event);event.blocks.reserve(103);
        for(unsigned kind=1;kind<105;++kind) {
            auto& block=event.blocks.emplace_back();block.kind=kind;
            const size_t bytes=kind==1?0x3600:(kind==104?152:(kind%4==0?64:32));
            pool.Prepare(kind,bytes,block.bytes,event.blocks.size()-1);
            block.bytes.assign(bytes,uint8_t(frame+kind));pool.Copied(bytes);
        }
        copied_events.push_back(event); // Failure diagnostics own independent bytes.
        auto& state=event.blocks.front();
        held.push_back(std::make_shared<const std::vector<uint8_t>>(std::move(state.bytes)));
        state.shared_bytes=held.back();
        if(frame%7==0) { // A sink may retain the entire event by moving it.
            moved_events.push_back(std::move(event));
        }
        pool.Reclaim(event);
        check(pool.statistics().retained_bytes<=pool.kMaximumRetainedBytes);
    }
    check(pool.statistics().buffer_reuses>20000);
    for(unsigned frame=0;frame<held.size();++frame) {
        check(held[frame]->size()==0x3600);
        for(const auto b:*held[frame])check(b==uint8_t(frame+1));
        for(const auto& block:copied_events[frame].blocks)
            for(const auto b:block.data())check(b==uint8_t(frame+block.kind));
    }
    for(unsigned i=0;i<moved_events.size();++i)
        for(const auto& block:moved_events[i].blocks)
            for(const auto b:block.data())check(b==uint8_t(i*7+block.kind));
    pool.Clear();
    check(pool.statistics().retained_bytes==0);
    GraphicsBoundaryEvent huge;pool.Begin(huge);
    for(unsigned kind=0;kind<105;++kind) {
        auto& block=huge.blocks.emplace_back();block.kind=kind;
        pool.Prepare(kind,kind?65536:65537,block.bytes,huge.blocks.size()-1);
        block.bytes.resize(kind?65536:65537);
    }
    pool.Reclaim(huge);
    check(pool.statistics().oversized_refusals==1);
    check(pool.statistics().budget_refusals>0);
    check(pool.statistics().retained_bytes<=pool.kMaximumRetainedBytes);
    // Reuse followed by pool teardown cannot modify or free transferred owners.
    pool.Clear();
    for(unsigned i=0;i<held.size();++i)check(held[i]->front()==uint8_t(i+1));
    std::cout<<"PASS boundary block pool "<<checks<<" ownership and byte checks\n";
}
