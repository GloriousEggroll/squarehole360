#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gow3::renderer {

// Canonical byte ownership remains in the caller's two original regions.
// These half-open intervals contain only the page indices visited by the
// original version loop. This metadata never owns guest or GPU resources.
using NativePageVersionRanges=std::vector<std::pair<uint32_t,uint32_t>>;
using NativeTextureByteRegions=std::array<NativePageVersionRanges,2>;
inline constexpr uint32_t kNativeTextureVersionPageBytes=4096;

inline uint64_t NativeByteRangePageVisits(const NativeTextureByteRegions& regions) {
    uint64_t visits=0;
    for(const auto& region:regions)for(const auto& [first,last]:region) {
        if(first>=last)throw std::invalid_argument("Invalid texture version byte interval");
        visits+=uint64_t((last-1)/kNativeTextureVersionPageBytes)-
            first/kNativeTextureVersionPageBytes+1;
    }
    return visits;
}

inline NativePageVersionRanges BuildNativePageVersionRanges(const NativeTextureByteRegions& regions) {
    NativePageVersionRanges pages;
    for(const auto& region:regions)for(const auto& [first,last]:region) {
        if(first>=last)throw std::invalid_argument("Invalid texture version byte interval");
        pages.emplace_back(first/kNativeTextureVersionPageBytes,
            (last-1)/kNativeTextureVersionPageBytes+1);
    }
    std::sort(pages.begin(),pages.end());
    std::size_t count=0;
    for(const auto& range:pages) {
        if(count&&range.first<=pages[count-1].second)
            pages[count-1].second=std::max(pages[count-1].second,range.second);
        else pages[count++]=range;
    }
    pages.resize(count);
    // Complete the replacement before publishing immutable cache metadata.
    if(pages.capacity()>pages.size())return {pages.begin(),pages.end()};
    return pages;
}

inline uint64_t NativePageRangeVisits(const NativePageVersionRanges& pages) {
    uint64_t visits=0;
    for(const auto& [first,last]:pages)visits+=uint64_t(last)-first;
    return visits;
}

// All byte intervals have already passed the original physical-capacity
// checks; their page union is consequently in bounds for this allocation.
inline uint64_t ScanNativePageVersionRanges(const NativePageVersionRanges& pages,
                                          std::span<const uint64_t> versions) {
    uint64_t result=0;
    for(const auto& [first,last]:pages)
        for(uint32_t page=first;page<last;++page)result=std::max(result,versions[page]);
    return result;
}

// Reuse only the complete original page-writer serial. Failed scans leave the
// previous memo untouched. CPU payload identity and ownership are independent.
template<class Scanner>
uint64_t ReadNativePageVersion(uint64_t serial,uint64_t& cached_serial,
                              uint64_t& cached_version,bool& valid,Scanner&& scanner) {
    if(valid&&cached_serial==serial)return cached_version;
    const uint64_t next=std::forward<Scanner>(scanner)();
    cached_version=next;cached_serial=serial;valid=true;return next;
}

} // namespace gow3::renderer
