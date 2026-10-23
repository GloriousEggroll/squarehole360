#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace gow3::renderer {
// CPU metadata only. A stamp certifies absence of writes, never a page value.
// Coarse groups conservatively invalidate nearby untouched pages as well.
// Tokens belong to one tracker lifetime; reconstruct clients with the tracker.
template<class Counter=uint64_t>
class NativePageChangeTracker {
    static_assert(std::is_unsigned_v<Counter>);
public:
    using Range=std::pair<uint32_t,uint32_t>;
    static constexpr uint32_t kGroupPages=64;
    explicit NativePageChangeTracker(uint32_t pages=0)
        :pages_(pages),groups_((uint64_t(pages)+kGroupPages-1)/kGroupPages) {}
    Counter revision()const noexcept{return revision_;}
    void Write(uint32_t first,uint32_t end) {
        Check(first,end);
        if(first==end||!revision_)return;
        // Never permit a wrapped stamp to authenticate an old memo. Reuse
        // stays disabled after exhaustion, including for same-serial reads.
        if(revision_==std::numeric_limits<Counter>::max()) {revision_=0;return;}
        ++revision_;
        std::fill(groups_.begin()+first/kGroupPages,
                  groups_.begin()+(end-1)/kGroupPages+1,revision_);
    }
    bool UnchangedSince(Counter previous,std::span<const Range> ranges)const {
        for(const auto& [first,end]:ranges)Check(first,end);
        if(!previous||!revision_||previous>revision_)return false;
        if(previous==revision_)return true;
        for(const auto& [first,end]:ranges)if(first!=end)
            for(uint32_t i=first/kGroupPages,last=(end-1)/kGroupPages;i<=last;++i)
                if(groups_[i]>previous)return false;
        return true;
    }
private:
    void Check(uint32_t first,uint32_t end)const {
        if(first>end||end>pages_)throw std::out_of_range("Invalid page change range");
    }
    uint32_t pages_;
    Counter revision_=1;
    std::vector<Counter> groups_;
};
} // namespace gow3::renderer
