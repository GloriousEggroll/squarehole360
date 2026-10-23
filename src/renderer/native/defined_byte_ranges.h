#pragma once
#include <algorithm>
#include <cstdint>
#include <map>

namespace gow3::renderer {
// Byte coverage only, independent of writer versions and GPU lifetime. Callers
// must still update versions on every write, even when coverage is unchanged.
// Ranges are nonempty, within the backing allocation, and use exclusive ends.
inline bool DefineNativeByteRange(std::map<uint32_t,uint32_t>& defined,
                                 uint32_t first,uint32_t last,bool reuse_covered) {
    auto it=defined.upper_bound(first);
    if(it!=defined.begin()) {
        auto prev=std::prev(it);
        if(reuse_covered&&prev->second>=last)return true;
        if(prev->second>=first) {
            first=prev->first;last=std::max(last,prev->second);it=defined.erase(prev);
        }
    }
    while(it!=defined.end()&&it->first<=last) {last=std::max(last,it->second);it=defined.erase(it);}
    defined[first]=last;
    return false;
}
} // namespace gow3::renderer
