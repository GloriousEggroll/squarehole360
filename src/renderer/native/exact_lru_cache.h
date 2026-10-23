#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <array>
#include <map>
#include <type_traits>
#include <utility>

namespace gow3::renderer {

// Thread-confined CPU metadata. Keys are compared exactly; hashes, addresses
// and resource generations never stand in for equality. Values own their data.
// A returned reference is valid until the next cache lookup or destruction.
// Hits allocate nothing. A failed builder leaves existing values untouched.
// The builder and value destructor must not reenter this same cache.
template<class Key,class Value,size_t Capacity>
class ExactLruCache {
    static_assert(Capacity>0);
public:
    static constexpr size_t kCapacity=Capacity;
    struct Statistics {
        uint64_t hits=0,misses=0,admissions=0,evictions=0;
        size_t entries=0,peak_entries=0;
    };
    ExactLruCache()=default;
    ExactLruCache(const ExactLruCache&)=delete;
    ExactLruCache& operator=(const ExactLruCache&)=delete;
    ExactLruCache(ExactLruCache&&)=delete;
    ExactLruCache& operator=(ExactLruCache&&)=delete;

    template<class Builder>
    Value& GetOrCreate(const Key& key,Builder&& builder) {
        // Direct-mapped lookaside of map iterators. The hash only selects a
        // slot; a hit still requires exact key equality. std::map iterators stay
        // valid until their own node is erased, and the only erase (eviction)
        // clears the slot naming that node first. Hits keep the exact original
        // statistics and LRU splice, so eviction order is unchanged.
        auto& slot=lookaside_[LookasideIndex(key)];
        if(slot.valid&&slot.entry->first==key) {
            ++statistics_.hits;
            age_.splice(age_.end(),age_,slot.entry->second.age);
            return slot.entry->second.value;
        }
        if(const auto found=values_.find(key);found!=values_.end()) {
            ++statistics_.hits;
            slot={found,true};
            age_.splice(age_.end(),age_,found->second.age);
            return found->second.value;
        }
        ++statistics_.misses;
        // Validation and full range construction precede cache mutation, as in
        // the former clear-at-capacity implementation. Never retain half a value.
        Value value=std::forward<Builder>(builder)();
        if(values_.size()==Capacity) {
            const auto victim=values_.find(age_.front());
            auto& victim_slot=lookaside_[LookasideIndex(victim->first)];
            if(victim_slot.valid&&victim_slot.entry==victim)victim_slot.valid=false;
            values_.erase(victim);
            age_.pop_front();
            ++statistics_.evictions;
        }
        age_.push_back(key);
        try {
            auto stored=values_.emplace(key,Entry{std::move(value),std::prev(age_.end())}).first;
            ++statistics_.admissions;
            statistics_.peak_entries=std::max(statistics_.peak_entries,values_.size());
            slot={stored,true};
            return stored->second.value;
        } catch(...) {
            age_.pop_back();
            throw;
        }
    }
    Statistics statistics()const noexcept {
        auto result=statistics_;result.entries=values_.size();return result;
    }
private:
    using Age=std::list<Key>;
    struct Entry {Value value;typename Age::iterator age;};
    using Values=std::map<Key,Entry>;
    static constexpr size_t kLookasideSlots=512;
    struct Slot {typename Values::iterator entry{};bool valid=false;};
    // Integral keys or fixed arrays of integral words (all current users).
    static size_t LookasideIndex(const Key& key) noexcept {
        uint64_t h=0x9e3779b97f4a7c15ull;
        if constexpr(std::is_integral_v<Key>)h^=uint64_t(key);
        else for(const auto word:key)h=(h^uint64_t(word))*0xff51afd7ed558ccdull;
        h^=h>>33;h*=0xc4ceb9fe1a85ec53ull;h^=h>>33;
        return size_t(h&(kLookasideSlots-1));
    }
    Age age_;
    Values values_;
    std::array<Slot,kLookasideSlots> lookaside_{};
    Statistics statistics_;
};

} // namespace gow3::renderer
