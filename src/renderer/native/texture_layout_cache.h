#pragma once
#include "texture_data.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace gow3::renderer {
// The optional four-way policy retains the same 1024 entries and 4 MiB level budget.
// Pure fetch metadata only: this cache never owns texture bytes, guest pointers,
// images, physical-memory versions or coherence decisions. A returned reference
// is borrowed until the next Get call and must not escape the texture callback.
class TextureLayoutCache {
public:
    static constexpr size_t kEntries=1024;
    static constexpr size_t kMaximumLevelBytes=4u*1024*1024;
    // DescribeTexture admits at most 14 mips and 64 array layers. Admission also
    // accounts vector capacity, so allocator growth does not defeat the bound.
    static constexpr size_t kMaximumLevels=14u*64;
    struct Statistics {
        uint64_t hits=0,misses=0,admissions=0,evictions=0,bypasses=0;
        size_t entries=0,level_bytes=0,peak_level_bytes=0;
    };
    explicit TextureLayoutCache(bool associative=false):associative_(associative) {
        if(associative_)for(auto& set:lru_)for(uint8_t way=0;way<kWays;++way)set[way]=way;
    }
    const TextureLayout& Get(const std::array<uint32_t,6>& fetch,TextureLayout& fallback) {
        const size_t bucket=Bucket(fetch),set=bucket&(kSets-1);
        size_t target=bucket;
        if(associative_) {
            const auto& order=lru_[set];
            target=set*kWays+order.back();
            size_t empty=kEntries;
            for(uint8_t way:order) {
                const size_t index=set*kWays+way;
                const auto& candidate=entries_[index];
                if(candidate&&candidate->fetch==fetch) {
                    ++statistics_.hits;Touch(set,way);return *candidate;
                }
                if(!candidate&&empty==kEntries)empty=index;
            }
            if(empty!=kEntries)target=empty;
        }
        auto& entry=entries_[target];
        if(!associative_&&entry&&entry->fetch==fetch) {++statistics_.hits;return *entry;}
        ++statistics_.misses;
        // Validation and allocation happen before any occupied slot is touched.
        // DescribeTexture failures retain the old entry and original exception.
        auto decoded=DescribeTexture(fetch);
        const size_t old_bytes=entry?entry->levels.capacity()*sizeof(TextureLevel):0;
        const size_t remaining=statistics_.level_bytes-old_bytes;
        if(decoded.levels.size()>kMaximumLevels||
           decoded.levels.capacity()>(kMaximumLevelBytes-remaining)/sizeof(TextureLevel)) {
            ++statistics_.bypasses;
            fallback=std::move(decoded);return fallback;
        }
        const size_t bytes=decoded.levels.capacity()*sizeof(TextureLevel);
        // Both assignments are non-allocating moves. A cached reference may be
        // invalidated here only after the replacement decoded successfully.
        if(entry)++statistics_.evictions;else ++statistics_.entries;
        entry=std::move(decoded);
        statistics_.level_bytes=remaining+bytes;
        statistics_.peak_level_bytes=std::max(statistics_.peak_level_bytes,statistics_.level_bytes);
        ++statistics_.admissions;
        if(associative_)Touch(set,uint8_t(target%kWays));
        return *entry;
    }
    const Statistics& statistics()const noexcept{return statistics_;}
    uint32_t ways()const noexcept{return associative_?kWays:1;}
    static size_t Bucket(const std::array<uint32_t,6>& fetch)noexcept {
        uint64_t hash=14695981039346656037ull;
        for(uint32_t word:fetch) {hash^=word;hash*=1099511628211ull;}
        hash^=hash>>33;hash*=0xff51afd7ed558ccdull;hash^=hash>>33;
        return size_t(hash)&(kEntries-1);
    }
private:
    static constexpr uint8_t kWays=4;
    static constexpr size_t kSets=kEntries/kWays;
    static_assert(kEntries%kWays==0&&(kSets&(kSets-1))==0);
    // MRU ordering owns no layout. It only changes after a hit or successfully
    // admitted replacement; decode/allocation/budget refusals preserve it.
    void Touch(size_t set,uint8_t way)noexcept {
        auto& order=lru_[set];
        size_t at=0;while(order[at]!=way)++at;
        while(at){order[at]=order[at-1];--at;}order[0]=way;
    }
    const bool associative_;
    std::array<std::array<uint8_t,kWays>,kSets> lru_{};
    static_assert((kEntries&(kEntries-1))==0);
    static_assert(std::is_nothrow_move_constructible_v<TextureLayout>);
    static_assert(std::is_nothrow_move_assignable_v<TextureLayout>);
    std::array<std::optional<TextureLayout>,kEntries> entries_;
    Statistics statistics_;
};
}
