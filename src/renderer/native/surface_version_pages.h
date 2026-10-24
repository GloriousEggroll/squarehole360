#pragma once
#include "edram_extent.h"
#include <bitset>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
namespace gow3::renderer {
using NativeSurfacePageMask=std::bitset<kNativeEdramMaxTiles*5120u/4096u>;
// Same half-open byte ranges and inclusive touched endpoint page as
// RecordSurfaceWrite. No bounding-box or tile-overlap approximation.
inline NativeSurfacePageMask NativeSurfaceVersionPages(std::span<const std::pair<uint32_t,uint32_t>> ranges) {
    NativeSurfacePageMask pages;
    for(const auto& [first,last]:ranges) {
        if(first>=last||last>NativeEdramBytes())throw std::invalid_argument("Invalid native surface page range");
        for(uint32_t page=first/4096;page<=(last-1)/4096;++page)pages.set(page);
    }
    return pages;
}
}
