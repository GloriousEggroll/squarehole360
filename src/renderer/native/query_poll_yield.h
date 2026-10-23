#pragma once
#include <array>
#include <cstdint>
#include <utility>

namespace gow3::renderer {
// Only the existing blocking retry call is eligible. The initial GetData call
// and every nonblocking caller preserve their original scheduling.
inline constexpr uint32_t kQueryPollRetryCaller=0x829619A0;
inline constexpr uint32_t kQueryPollYieldInterval=128;
inline constexpr std::array<std::array<uint32_t,2>,7> kQueryPollYieldSites{{
    {{0x82AFA708,0x7D8802A6}}, // GetData: mflr r12
    {{0x82AFA714,0x8BC30014}}, // lbz r30,20(r3): query status flags
    {{0x82AFA738,0x81660004}}, // lwz r11,4(r6): query type
    {{0x82961994,0x7FC4F378}}, // mr r4,r30: original output pointer
    {{0x82961998,0x807D0004}}, // lwz r3,4(r29): current query object
    {{0x8296199C,0x48198D6D}}, // bl 0x82AFA708: retry, LR=0x829619A0
    {{0x829619A8,0x409AFFEC}}, // bne cr6,0x82961994
}};

enum class QueryPollDecision { Ignore, Pending, Yield, Completed };

// Epoch zero/even disables the experiment. A new odd epoch invalidates every
// guest thread's local streak without retaining pointers or taking a lock.
class QueryPollYieldPolicy {
public:
    void Reset() noexcept {epoch_=0;query_=0;pending_=0;}
    QueryPollDecision Observe(uint64_t epoch,uint32_t query,uint64_t caller,uint32_t status) noexcept {
        if(!(epoch&1)||caller!=kQueryPollRetryCaller) {Reset();return QueryPollDecision::Ignore;}
        if(epoch_!=epoch||query_!=query) {epoch_=epoch;query_=query;pending_=0;}
        if(!status) {Reset();return QueryPollDecision::Completed;}
        if(status!=1) {Reset();return QueryPollDecision::Ignore;}
        if(++pending_==kQueryPollYieldInterval) {pending_=0;return QueryPollDecision::Yield;}
        return QueryPollDecision::Pending;
    }
private:
    uint64_t epoch_=0;
    uint32_t query_=0,pending_=0;
};

// Used by the real ABI wrapper and its context-preservation oracle. The
// observer gets values only, never a mutable context or guest-memory pointer.
// An original exception propagates without invoking the observer.
template<class Context,class Original,class Observe>
void InvokeQueryPollOriginal(Context& context,uint8_t* base,Original&& original,Observe&& observe) {
    const uint32_t query=context.r3.u32;
    const uint64_t caller=context.lr;
    std::forward<Original>(original)(context,base);
    std::forward<Observe>(observe)(query,caller,context.r3.u32);
}
} // namespace gow3::renderer
