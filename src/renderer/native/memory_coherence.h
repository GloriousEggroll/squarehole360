#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gow3::renderer {
// Page ownership only; the memory callback neither reads guest memory nor
// invokes rendering. Drain notifications at the next ordered API boundary.
class NativeMemoryCoherence {
public:
    using Ranges=std::vector<std::pair<uint32_t,uint32_t>>; // address, byte count
    static constexpr uint32_t kCapacity=512u*1024*1024,kPage=4096;
    void Watch(uint32_t address,uint32_t bytes) {
        Check(address,bytes);std::lock_guard lock(mutex_);
        for(uint32_t p=address/kPage;p<=(address+bytes-1)/kPage;++p) {
            const uint64_t mask=uint64_t{1}<<(p%64);
            if(!(watched_[p/64]&mask)) {armed_[p/64]|=mask;has_armed_=true;}
            watched_[p/64]|=mask;
        }
    }
    bool Invalidate(uint32_t address,uint32_t bytes) {
        if(!bytes||address>=kCapacity)return false;
        const uint32_t last=uint32_t(std::min<uint64_t>(uint64_t(address)+bytes,kCapacity)-1)/kPage;
        bool changed=false;std::lock_guard lock(mutex_);
        for(uint32_t p=address/kPage;p<=last;++p) {
            const uint64_t mask=uint64_t{1}<<(p%64);
            if(watched_[p/64]&mask) {
                dirty_[p/64]|=mask;watched_[p/64]&=~mask;armed_[p/64]&=~mask;
                changed=has_dirty_=true;
            }
        }
        return changed;
    }
    Ranges TakeInvalidated() {std::lock_guard lock(mutex_);return Drain(dirty_,has_dirty_);}
    Ranges TakeWatches() {std::lock_guard lock(mutex_);return Drain(armed_,has_armed_);}
    // CPU snapshots retained in native storage need the same one-shot write
    // notification as GPU output. Rearm before reading, without promoting the
    // page to GPU ownership in the planner. Never call the SDK under mutex_.
    // Caller holds the SDK global critical region through rearm, copy and
    // planner publication, and excludes overlapping external host writers.
    // A previously notified store either precedes re-protection or refaults
    // afterward. Epoch checks still fail closed for unexpected notifications;
    // an epoch alone does not prove completion of a faulting instruction.
    template<class Enable,class Epoch,class Read>
    std::pair<uint64_t,std::vector<uint8_t>> CaptureCpuSnapshot(
        uint32_t address,uint32_t bytes,Enable&& enable,Epoch&& epoch,Read&& read) {
        const uint64_t captured_epoch=epoch();
        Watch(address,bytes);
        for(const auto& [first,count]:TakeWatches())enable(first,count);
        const auto armed_epoch=epoch();
        if(armed_epoch!=captured_epoch)
            throw std::runtime_error(Conflict("rearming",address,bytes,captured_epoch,armed_epoch));
        auto data=read();
        const auto copied_epoch=epoch();
        if(copied_epoch!=captured_epoch)
            throw std::runtime_error(Conflict("capturing",address,bytes,captured_epoch,copied_epoch));
        if(data.size()!=bytes)throw std::runtime_error("Invalid native coherence snapshot extent");
        return {captured_epoch,std::move(data)};
    }
    static std::string Conflict(const char* phase,uint32_t address,uint32_t bytes,
                                uint64_t expected,uint64_t observed) {
        return std::string("CPU memory changed while ")+phase+" native coherence snapshot"+
            " (address="+std::to_string(address)+", bytes="+std::to_string(bytes)+
            ", page="+std::to_string(address/kPage)+", expected_epoch="+std::to_string(expected)+
            ", observed_epoch="+std::to_string(observed)+")";
    }
private:
    using Pages=std::array<uint64_t,kCapacity/kPage/64>;
    Pages watched_{},dirty_{},armed_{};
    bool has_dirty_=false,has_armed_=false;
    std::mutex mutex_;
    static void Check(uint32_t a,uint32_t n) {
        if(!n||uint64_t(a)+n>kCapacity)throw std::runtime_error("Invalid native memory watch range");
    }
    static Ranges Drain(Pages& pages,bool& any) {
        Ranges ranges;if(!any)return ranges;any=false;
        for(uint32_t word=0;word<pages.size();++word) {
            auto bits=std::exchange(pages[word],0);
            while(bits) {
                const uint32_t first=(word*64+std::countr_zero(bits))*kPage;
                bits&=bits-1;
                if(!ranges.empty()&&ranges.back().first+ranges.back().second==first)
                    ranges.back().second+=kPage;
                else ranges.emplace_back(first,kPage);
            }
        }
        return ranges;
    }
};
}
