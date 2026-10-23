#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>

namespace gow3::renderer {
// CPU copies of completed, exact native uploads. A receipt says nothing about
// guest allocation lifetime or current guest bytes. Its page-version guard
// authenticates the bytes in the existing native physical buffer instead.
// Command-processor thread only; no callbacks/mutations during a forward.
class NativeCpuUploadReceipts {
public:
    static constexpr uint32_t kPageBytes=4096,kSlots=64;
    static constexpr uint64_t kPhysicalLimit=512ull*1024*1024;
    static constexpr size_t kMaximumPayloadBytes=size_t(kSlots)*kPageBytes;
    struct Copy {uint32_t first,last;uint8_t* destination;};
    struct Statistics {
        bool requested=false,enabled=false,checked_queue=false;
        uint64_t allocation_refusals=0,unchecked_queue_refusals=0,storage_bytes=0;
        uint64_t write_attempts=0,admitted_pages=0,admitted_bytes=0,
            queued_refusals=0,unsupported_refusals=0,version_errors=0,
            slot_replacements=0,retained_pages=0,serial_wrap_clears=0;
        uint64_t attempts=0,hits=0,refusals=0,pages_checked=0,pages=0,bytes=0,
            missing_pages=0,stale_pages=0,invalid_fragments=0;
    };
    void Configure(bool requested,bool checked_queue) {
        if(configured_)throw std::logic_error("CPU upload receipts must be configured once");
        configured_=true;stats_.requested=requested;stats_.checked_queue=checked_queue;
        if(!requested)return;
        if(!checked_queue){++stats_.unchecked_queue_refusals;return;}
        try {storage_=std::make_unique<Storage>();}
        catch(const std::bad_alloc&) {++stats_.allocation_refusals;return;}
        stats_.enabled=true;stats_.storage_bytes=sizeof(Storage);
    }
    // Call only AFTER the original upload/Define succeeds. Version lookup
    // must be the actual one-page native version, not the global mutation
    // serial or a guest address epoch. Queued writes never publish a receipt.
    template<class PageVersion>
    void Remember(uint32_t address,std::span<const uint8_t> bytes,bool synchronous,
                  PageVersion&& page_version) noexcept {
        if(!stats_.enabled)return;
        ++stats_.write_attempts;
        if(!synchronous){++stats_.queued_refusals;return;}
        if(address%kPageBytes||bytes.size()!=kPageBytes||!bytes.data()||
           uint64_t(address)+bytes.size()>kPhysicalLimit) {
            ++stats_.unsupported_refusals;return;
        }
        const uint32_t page=address/kPageBytes;uint64_t version;
        try {version=page_version(page);}
        catch(...) {++stats_.version_errors;return;}
        auto& entry=(*storage_)[page&(kSlots-1)];
        if(entry.valid)++stats_.slot_replacements;else ++stats_.retained_pages;
        entry.valid=false;
        std::memcpy(entry.bytes.data(),bytes.data(),kPageBytes);
        entry.page=page;entry.version=version;entry.valid=true;
        ++stats_.admitted_pages;stats_.admitted_bytes+=kPageBytes;
    }
    // The caller has already performed its original ownership/payload checks
    // and all original pre-readback flush callbacks. No destination changes
    // until EVERY original owned fragment has a current receipt. Misses keep
    // the complete original GPU readback path; no partial forwarding occurs.
    template<class PageVersion>
    bool TryForward(std::span<const Copy> copies,PageVersion&& page_version) noexcept {
        if(!stats_.enabled||copies.empty())return false;
        ++stats_.attempts;
        uint64_t pages=0,bytes=0;
        for(const auto& copy:copies) {
            if(copy.first>=copy.last||uint64_t(copy.last)>kPhysicalLimit||!copy.destination) {
                ++stats_.invalid_fragments;++stats_.refusals;return false;
            }
            for(uint32_t at=copy.first;at<copy.last;) {
                const auto page=at/kPageBytes;
                const auto& entry=(*storage_)[page&(kSlots-1)];
                ++stats_.pages_checked;
                if(!entry.valid||entry.page!=page) {
                    ++stats_.missing_pages;++stats_.refusals;return false;
                }
                uint64_t current;
                try {current=page_version(page);}
                catch(...) {++stats_.version_errors;++stats_.refusals;return false;}
                if(current!=entry.version) {
                    ++stats_.stale_pages;++stats_.refusals;return false;
                }
                at+=std::min(copy.last-at,kPageBytes-at%kPageBytes);++pages;
            }
            bytes+=uint64_t(copy.last)-copy.first;
        }
        // No version getters or external callbacks occur between admission
        // and these copies. Preserve the original fragment/destination order,
        // including base/mip aliases; never copy a CPU-owned hole or padding.
        for(const auto& copy:copies)for(uint32_t at=copy.first;at<copy.last;) {
            const auto& entry=(*storage_)[(at/kPageBytes)&(kSlots-1)];
            const uint32_t count=std::min(copy.last-at,kPageBytes-at%kPageBytes);
            std::memcpy(copy.destination+(at-copy.first),entry.bytes.data()+at%kPageBytes,count);
            at+=count;
        }
        ++stats_.hits;stats_.pages+=pages;stats_.bytes+=bytes;return true;
    }
    // Numerical page versions can repeat after serial wrap. Drop all earlier
    // receipts before publishing any new epoch's version, including zero.
    void ClearOnSerialWrap() noexcept {
        if(!stats_.enabled)return;
        for(auto& entry:*storage_)entry.valid=false;
        stats_.retained_pages=0;++stats_.serial_wrap_clears;
    }
    const Statistics& statistics()const noexcept{return stats_;}
private:
    struct Entry {
        uint64_t version=0;
        uint32_t page=0;
        bool valid=false;
        std::array<uint8_t,kPageBytes> bytes{};
    };
    using Storage=std::array<Entry,kSlots>;
    static_assert((kSlots&(kSlots-1))==0);
    static_assert(sizeof(Storage)<=264u*1024);
    bool configured_=false;
    std::unique_ptr<Storage> storage_;
    Statistics stats_;
};
} // namespace gow3::renderer
