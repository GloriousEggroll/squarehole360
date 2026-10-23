#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace gow3::renderer {
using FootprintRanges=std::vector<std::pair<uint32_t,uint32_t>>;
// A touching/overlapping consecutive interval may join the preceding temporary
// run in either address direction. This preserves their exact union; callers
// validate every emitted interval (pointwise or by an exact layout proof)
// and perform the final sorted union.
inline void AppendFootprintRange(FootprintRanges& ranges,uint32_t first,uint32_t last) {
    if(!ranges.empty()&&first<=last&&ranges.back().first<=ranges.back().second&&
       first<=ranges.back().second&&last>=ranges.back().first) {
        ranges.back().first=std::min(ranges.back().first,first);
        ranges.back().second=std::max(ranges.back().second,last);
    } else ranges.emplace_back(first,last);
}
// Keep cached metadata proportional to the final exact union. Construct the
// replacement before swapping so allocation failure leaves the old list intact.
inline void ReleaseFootprintRangeCapacity(FootprintRanges& ranges) {
    if(ranges.capacity()>ranges.size()) {
        FootprintRanges exact(ranges.begin(),ranges.end());
        ranges.swap(exact);
    }
}
}
