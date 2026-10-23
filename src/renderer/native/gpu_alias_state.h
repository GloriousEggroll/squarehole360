#pragma once
#include "footprint_ranges.h"
#include <map>
#include <iterator>
#include <span>
#include <stdexcept>

namespace gow3::renderer {
struct NativeGpuAliasState {
    bool any_defined=false,fully_defined=true;
    uint64_t version=0;
    bool operator==(const NativeGpuAliasState&) const = default;
};
struct NativeGpuAliasStateMemo {
    NativeGpuAliasState value;
    uint64_t serial=0;
    bool valid=false;
};
struct NativeGpuAliasFootprint {
    FootprintRanges ranges;
    NativeGpuAliasStateMemo memo;
};
struct NativeGpuAliasStateStatistics {
    uint64_t requests=0,hits=0,scans=0,ranges_scanned=0,pages_scanned=0,
             cache_clears=0,serial_wraps=0;
};
// The caller first performs the original full-key footprint validation. This
// copied result owns no image, byte range or container reference. All metadata
// is confined to the original command processor thread; no GPU operation runs.
template<class ReadRange>
NativeGpuAliasState ReadNativeGpuAliasStateRanges(
    NativeGpuAliasFootprint& footprint,uint64_t serial,
    const std::map<uint32_t,uint32_t>& defined,size_t page_count,ReadRange&& read_range,
    NativeGpuAliasStateStatistics& stats) {
    ++stats.requests;
    if(footprint.memo.valid&&footprint.memo.serial==serial) {
        ++stats.hits;return footprint.memo.value;
    }
    ++stats.scans;NativeGpuAliasState next;
    for(const auto& [first,last]:footprint.ranges) {
        if(first>=last||uint64_t((last-1)/4096)>=page_count)
            throw std::invalid_argument("Invalid GPU alias state footprint");
        ++stats.ranges_scanned;
        const auto it=defined.upper_bound(first);
        const bool previous=it!=defined.begin();
        const bool covered=previous&&std::prev(it)->second>=last;
        next.fully_defined&=covered;
        next.any_defined|=(previous&&std::prev(it)->second>first)||
                          (it!=defined.end()&&it->first<last);
        // Even a missing/partial first range cannot truncate the version scan.
        // Keep duplicate page visits exactly as in the original Version path.
        stats.pages_scanned+=uint64_t((last-1)/4096)-first/4096+1;
        next.version=std::max(next.version,read_range(first/4096,(last-1)/4096+1));
    }
    // Publish only after the whole scan; a validation exception preserves the
    // previous serial/value. Zero is a valid serial, hence the separate bit.
    footprint.memo.value=next;footprint.memo.serial=serial;footprint.memo.valid=true;
    return next;
}
inline NativeGpuAliasState ReadNativeGpuAliasState(
    NativeGpuAliasFootprint& footprint,uint64_t serial,
    const std::map<uint32_t,uint32_t>& defined,std::span<const uint64_t> pages,
    NativeGpuAliasStateStatistics& stats) {
    return ReadNativeGpuAliasStateRanges(footprint,serial,defined,pages.size(),
        [&](uint32_t first,uint32_t last) {
            uint64_t version=0;
            for(auto page=first;page<last;++page)version=std::max(version,pages[page]);
            return version;
        },stats);
}
// The three authoritative page/definition writers retain their original
// increment point. A wrap invalidates only memos, never ranges: Define may be
// iterating a range reference belonging to this very map.
template<class Footprints>
bool AdvanceNativeGpuAliasStateSerial(uint64_t& serial,Footprints& footprints) {
    if(++serial)return false;
    for(auto& [key,entry]:footprints)entry.memo.valid=false;
    return true;
}
}
