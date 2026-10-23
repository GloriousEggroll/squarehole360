#include "renderer/native/packet_ownership.h"
#include <iostream>
using namespace gow3::renderer;
namespace {
GraphicsCommandSpan Span(uint64_t generation,uint32_t address,std::initializer_list<uint32_t> words) {
    GraphicsCommandSpan s;s.generation=generation;s.api_sequence=generation+100;s.address=address;
    for(auto word:words)for(unsigned b=0;b<4;++b)s.bytes.push_back(uint8_t(word>>(24-b*8)));
    return s;
}
void Require(bool value) {if(!value)throw std::runtime_error("Packet ownership assertion failed");}
template<class F> void Reject(F f) {bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}Require(caught);}
}
int main() {
    try {
        // State + split native API draw chunks; two packets share an API span.
        auto a=Span(1,0x1000,{0x2102,9,0xc0003600,0x00030084,0xc0003600,0x00060084});
        const auto parsed=ParseCommandSpan(a);Require(parsed.draws.size()==2&&parsed.replacement_safe);
        PacketOwnership q;q.Enqueue(a);
        Require(!q.Inspect(0x9999,{}));
        auto first=q.Inspect(0x1008,{a.bytes.data()+8,8});Require(first&&first->generation==1&&!first->last_draw);
        auto changed=std::vector<uint8_t>(a.bytes.begin()+8,a.bytes.begin()+16);changed[7]^=1;
        Reject([&]{q.Inspect(0x1008,changed);});
        Require(q.Inspect(0x1008,{a.bytes.data()+8,8}).has_value());q.Consume(*first);
        Reject([&]{q.Consume(*first);});
        auto second=q.Inspect(0x1010,{a.bytes.data()+16,8});Require(second&&second->last_draw);q.Consume(*second);
        Require(q.pending_spans()==0);
        // The same command bytes at a recycled address remain distinct uses.
        a.generation=2;q.Enqueue(a);a.generation=3;q.Enqueue(a);
        for(uint64_t generation:{2,3})for(size_t i=0;i<2;++i) {
            auto m=q.Inspect(0x1008+uint32_t(i*8),{a.bytes.data()+8+i*8,8});
            Require(m&&m->generation==generation);q.Consume(*m);
        }
        Reject([&]{q.Enqueue(a);});
        // Do not pre-execute native work across memory/fence/control effects.
        auto controlled=Span(4,0x2000,{0xc0013d00,0x1000,9,0xc0003600,0x00030084});
        Require(!ParseCommandSpan(controlled).replacement_safe);
        auto conditional=Span(5,0x3000,{0xc0012200,0x100,0x00030084});
        Require(!ParseCommandSpan(conditional).replacement_safe);
        auto predicated=Span(5,0x3000,{0xc0003601,0x00030084});
        Require(!ParseCommandSpan(predicated).replacement_safe);
        auto predicated_state=Span(5,0x3000,{0xc0005501,0,0xc0003600,0x00030084});
        Require(!ParseCommandSpan(predicated_state).replacement_safe);
        PacketOwnership predicate_queue;predicate_queue.Enqueue(predicated);
        // A queued or skipped draw is not executable evidence and remains
        // pending. The exact packet callback proves only this single draw.
        Require(!predicate_queue.Inspect(0x3000,predicated.bytes)->replacement_safe);
        Require(!predicate_queue.InspectExecuting(0x3008,predicated.bytes));
        Require(predicate_queue.pending_spans()==1);
        const auto observed=predicate_queue.InspectExecuting(0x3000,predicated.bytes);
        Require(observed&&observed->replacement_safe&&observed->predicate_proven_by_callback);
        predicate_queue.Consume(*observed);Require(predicate_queue.pending_spans()==0);
        predicated_state.generation=6;predicate_queue.Enqueue(predicated_state);
        Require(!predicate_queue.InspectExecuting(0x3008,{predicated_state.bytes.data()+8,8})->replacement_safe);
        auto two_predicates=Span(7,0x5000,{0xc0003601,0x00030084,0xc0003601,0x00030084});
        Require(!ParseCommandSpan(two_predicates).executing_draw_proves_safe);
        auto mixed_predicates=Span(8,0x5000,{0xc0003600,0x00030084,0xc0003601,0x00030084});
        Require(!ParseCommandSpan(mixed_predicates).executing_draw_proves_safe);
        auto controlled_predicate=Span(9,0x5000,{0xc0013d00,0x1000,9,0xc0003601,0x00030084});
        Require(!ParseCommandSpan(controlled_predicate).executing_draw_proves_safe);
        // Actual startup Resolve: copy draw, count-one CACHE_FLUSH, then
        // COHER_DEST_BASE_1 reset. The SDK still executes both trailing packets
        // after the native copy fence has completed at the draw callback.
        auto trailing_flush=Span(10,0x1f9909c4,{0xc0003600,0x00030088,0xc0004600,6,0x2007,0});
        Require(ParseCommandSpan(trailing_flush).replacement_safe);
        auto predicated_copy_flush=Span(11,0x5000,{0xc0003601,0x00030088,0xc0004600,6});
        Require(!ParseCommandSpan(predicated_copy_flush).replacement_safe&&
            ParseCommandSpan(predicated_copy_flush).executing_draw_proves_safe);
        for(const auto& unsafe:{
            Span(12,0x5000,{0xc0004600,6,0xc0003600,0x00030088}),
            Span(13,0x5000,{0xc0003600,0x00030088,0xc0004600,6,0xc0003600,0x00030088}),
            Span(14,0x5000,{0xc0003600,0x00030088,0xc0004600,4}),
            Span(15,0x5000,{0xc0003600,0x00030088,0xc0004601,6}),
            Span(16,0x5000,{0xc0003600,0x00030088,0xc0024600,6,0x1000,0}),
            Span(17,0x5000,{0xc0003600,0x00030088,0xc0013d00,0x1000,9}),
            Span(18,0x5000,{0xc0003600,0x00030088,0xc0043c00,0,0x1000,0,0,0})}) {
            const auto state=ParseCommandSpan(unsafe);
            Require(!state.replacement_safe&&!state.executing_draw_proves_safe);
        }
        // SDK scratch-register writes can publish guest-memory completion even
        // though their packet is a register update rather than MEM_WRITE.
        for(const auto& scratch:{
            Span(19,0x5000,{0x578,9,0xc0003600,0x00030084}),
            Span(20,0x5000,{0x18578,8,9,0xc0003600,0x00030084}),
            Span(21,0x5000,{0x10577,8,9,0xc0003600,0x00030084}),
            Span(22,0x5000,{0x40000578,8,9,0xc0003600,0x00030084}),
            Span(23,0x5000,{0x402bc000,8,9,0xc0003600,0x00030084}),
            Span(24,0x5000,{0xc0015500,0x578,9,0xc0003600,0x00030084}),
            Span(25,0x5000,{0xc0015600,0x57f,9,0xc0003601,0x00030084})}) {
            const auto state=ParseCommandSpan(scratch);
            Require(!state.replacement_safe&&!state.executing_draw_proves_safe);
        }
        Require(ParseCommandSpan(Span(26,0x5000,{0x18577,8,9,0xc0003600,0x00030084})).replacement_safe);
        // Failure after committing a multi-packet API cannot make its remaining
        // reference draws executable again. Future uncommitted spans may drop.
        PacketOwnership committed;
        const auto multi=Span(1,0x6000,{0xc0003600,0x00030084,0xc0003600,0x00060084});
        committed.Enqueue(multi);
        committed.Enqueue(Span(2,0x7000,{0xc0003600,0x00030084}));
        const auto commit=committed.InspectExecuting(0x6000,{multi.bytes.data(),8});
        Require(commit&&!commit->native_committed&&!committed.HasCommittedSpan());
        committed.CommitSpan(*commit);Require(committed.HasCommittedSpan()&&committed.CommittedGeneration()==1);
        Reject([&]{committed.CommitSpan(*commit);});
        Reject([&]{committed.Clear();});
        committed.ClearUncommitted();Require(committed.pending_spans()==1);
        Require(committed.InspectExecuting(0x6000,{multi.bytes.data(),8})->native_committed);
        committed.Consume(*commit);
        committed.ClearUncommitted();Require(committed.HasCommittedSpan());
        const auto remainder=committed.InspectExecuting(0x6008,{multi.bytes.data()+8,8});
        Require(remainder&&remainder->native_committed&&remainder->last_draw);
        Require(!committed.InspectExecuting(0x7000,{multi.bytes.data(),8}));
        committed.Consume(*remainder);Require(!committed.HasCommittedSpan());
        committed.Clear();Require(committed.pending_spans()==0);
        committed.Enqueue(Span(3,0x6000,{0xc0003600,0x00030084}));
        const auto recycled=committed.InspectExecuting(0x6000,{multi.bytes.data(),8});
        Require(recycled&&recycled->generation==3&&!recycled->native_committed);
        auto stale_commit=*recycled;stale_commit.generation=1;
        Reject([&]{committed.CommitSpan(stale_commit);});
        committed.CommitSpan(*recycled);committed.ClearForShutdown();
        Require(!committed.HasCommittedSpan()&&committed.CommittedGeneration()==0&&committed.pending_spans()==0);
        auto indexed=Span(6,0x4000,{0xc0032200,0,0x00030004,0x123400,3});
        Require(ParseCommandSpan(indexed).replacement_safe);
        for(auto malformed:{Span(7,0x5000,{0xc0032200,0,0x00030004}),Span(8,0x5001,{0xc0003600,0x00030084}),Span(0,0x6000,{0xc0003600,0x00030084})})
            Reject([&]{ParseCommandSpan(malformed);});
        std::cout<<"Exact packet bytes, split spans, recycled generations, control rejection, trailing cache-flush ordering, executed single-draw predicate proof and non-consuming failure passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
