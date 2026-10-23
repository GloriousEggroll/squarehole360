#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace gow3::renderer {
// Replay diagnostic ownership only. Tokens are the existing Scene draw-slot
// generations; only that slot's real submission fence permits retirement.
class NativeFragmentInvocationLedger {
public:
    static constexpr uint32_t kSlots=128,kKeys=2048,kSamples=16384;
    struct Token {uint32_t slot;uint64_t generation;};
    struct Tag {
        uint64_t event=0;uint32_t frame=0,key=0,indices=0;
        uint64_t early_depth_anchor=0,early_depth_current=0;
    };
    enum class Phase:uint8_t {Free,Recording,Ended,Accepted};
    struct Slot {uint64_t generation=0;Phase phase=Phase::Free;Tag tag;};
    struct Sample {Token token;Tag tag;uint64_t submission;std::optional<uint64_t> invocations;};
    struct Totals {
        uint64_t begun=0,accepted=0,completed=0,cancelled=0,available=0,
            unavailable=0,invocations=0,pending=0;
    };
    static uint64_t Add(uint64_t a,uint64_t b) {
        if(b>std::numeric_limits<uint64_t>::max()-a)
            throw std::overflow_error("Fragment census counter overflow");
        return a+b;
    }
    explicit NativeFragmentInvocationLedger(uint32_t capacity):capacity_(capacity) {
        if(capacity!=64&&capacity!=128)throw std::invalid_argument("Fragment census capacity must be64 or128");
        samples_.reserve(kSamples);
    }
    bool BudgetAvailable() const noexcept {return totals_.begun<kSamples;}
    void Begin(Token token,Tag tag) {
        auto& slot=At(token.slot);
        if(!token.generation||token.generation<=slot.generation||slot.phase!=Phase::Free||!tag.key||tag.key>kKeys)
            throw std::logic_error("Fragment census slot is not a fresh retired generation");
        if(!BudgetAvailable())throw std::length_error("Fragment census sample budget exhausted");
        const auto begun=Add(totals_.begun,1),pending=Add(totals_.pending,1);
        slot={token.generation,Phase::Recording,tag};totals_.begun=begun;totals_.pending=pending;
    }
    void End(Token token) {Exact(token,Phase::Recording).phase=Phase::Ended;}
    void Accept(Token token) {
        auto& slot=Exact(token,Phase::Ended);const auto accepted=Add(totals_.accepted,1);
        slot.phase=Phase::Accepted;totals_.accepted=accepted;
    }
    // Call only for commands which were never accepted. GPU wrapper disables
    // recording after cancellation, rather than reusing possibly recorded IDs.
    void Cancel(Token token) {
        auto& slot=At(token.slot);
        if(slot.generation!=token.generation||(slot.phase!=Phase::Recording&&slot.phase!=Phase::Ended))
            throw std::logic_error("Fragment census cannot cancel accepted/foreign work");
        totals_.cancelled=Add(totals_.cancelled,1);--totals_.pending;slot.phase=Phase::Free;
    }
    bool Pending(Token token) const {
        const auto& slot=At(token.slot);
        if(slot.phase==Phase::Free)return false;
        if(slot.generation!=token.generation||slot.phase!=Phase::Accepted)
            throw std::logic_error("Fragment census fence has foreign/unaccepted ownership");
        return true;
    }
    void ValidateRetirement(std::span<const Token> tokens) const {
        std::array<bool,kSlots> seen{};
        for(auto token:tokens) {
            (void)Pending(token);
            if(seen[token.slot])throw std::logic_error("Fragment census fence repeats a slot");
            seen[token.slot]=true;
        }
    }
    void Retire(Token token,uint64_t submission,std::optional<uint64_t> invocations) {
        auto& slot=Exact(token,Phase::Accepted);
        if(!submission)throw std::logic_error("Fragment census requires a real nonzero submission");
        const auto completed=Add(totals_.completed,1);
        const auto available=Add(totals_.available,bool(invocations));
        const auto unavailable=Add(totals_.unavailable,!invocations);
        const auto sum=Add(totals_.invocations,invocations.value_or(0));
        if(samples_.size()>=kSamples)throw std::length_error("Fragment census sample storage exhausted");
        samples_.push_back({token,slot.tag,submission,invocations});
        totals_.completed=completed;totals_.available=available;totals_.unavailable=unavailable;totals_.invocations=sum;
        --totals_.pending;slot.phase=Phase::Free;
    }
    const Totals& totals() const noexcept {return totals_;}
    const std::vector<Sample>& samples() const noexcept {return samples_;}
private:
    Slot& At(uint32_t slot) {
        if(slot>=capacity_)throw std::out_of_range("Fragment census slot outside capacity");return slots_[slot];
    }
    const Slot& At(uint32_t slot) const {
        if(slot>=capacity_)throw std::out_of_range("Fragment census slot outside capacity");return slots_[slot];
    }
    Slot& Exact(Token token,Phase phase) {
        auto& slot=At(token.slot);
        if(slot.generation!=token.generation||slot.phase!=phase)
            throw std::logic_error("Fragment census phase/generation mismatch");return slot;
    }
    uint32_t capacity_;std::array<Slot,kSlots> slots_{};Totals totals_;
    std::vector<Sample> samples_;
};
}
