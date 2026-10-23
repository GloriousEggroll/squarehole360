#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
namespace gow3::renderer {
// Actual allocations remain charged while leased OR idle. Pool residency cannot
// bypass the existing retained-resource bound. No pending/fence inference here.
struct NativeRetainedOpaqueResourceBudget {
    std::atomic<uint64_t> bytes{0},peak{0};
    bool TryCharge(uint64_t size,uint64_t maximum) noexcept {
        if(!size||size>maximum)return false;
        auto held=bytes.load(std::memory_order_relaxed);
        do {if(held>maximum-size)return false;}
        while(!bytes.compare_exchange_weak(held,held+size,std::memory_order_relaxed));
        auto prior=peak.load(std::memory_order_relaxed);
        while(prior<held+size&&!peak.compare_exchange_weak(prior,held+size,std::memory_order_relaxed)){}
        return true;
    }
    void Release(uint64_t size) noexcept {
        if(!size)return;
        const auto held=bytes.fetch_sub(size,std::memory_order_relaxed);
        if(held<size)std::terminate();
    }
};
}
