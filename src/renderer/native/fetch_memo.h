#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace gow3::renderer {
// Direct-mapped memo for pure functions of a texture fetch constant. A slot
// hit requires all six words to match exactly; a collision only recomputes.
template<class Value,size_t Entries=512>
class FetchMemo {
    static_assert((Entries&(Entries-1))==0,"Entries must be a power of two");
public:
    using Fetch=std::array<uint32_t,6>;
    struct Statistics {uint64_t hits=0,misses=0;};
    template<class Compute>
    const Value& Get(const Fetch& fetch,Compute&& compute) {
        uint64_t h=0x9E3779B97F4A7C15ull;
        for(const auto word:fetch)h=(h^word)*0xFF51AFD7ED558CCDull;
        auto& slot=slots_[size_t(h>>40)&(Entries-1)];
        if(slot.valid&&slot.fetch==fetch){++stats_.hits;return slot.value;}
        ++stats_.misses;
        // A throwing computation leaves the slot invalid, never half-written.
        slot.valid=false;slot.value=compute(fetch);slot.fetch=fetch;slot.valid=true;
        return slot.value;
    }
    void Clear()noexcept{for(auto& slot:slots_)slot.valid=false;}
    Statistics statistics()const noexcept{return stats_;}
private:
    struct Slot {Fetch fetch{};Value value{};bool valid=false;};
    std::array<Slot,Entries> slots_{};
    Statistics stats_;
};
}
