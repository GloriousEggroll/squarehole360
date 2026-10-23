#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace gow3::renderer {

// Stored keys own their strings; an operation's lookup borrows only for the
// duration of Find. Preserve the ordered map's existing key and tie order.
template<class String> struct CpuImageKey {
  std::array<uint32_t,6> fetch;
  std::optional<String> base,mips;
  uint64_t version;
};
template<class String>
std::optional<std::string_view> CpuImageDigestView(const std::optional<String>& digest) {
  if(!digest)return std::nullopt;
  return std::string_view(*digest);
}
struct CpuImageLess {
  using is_transparent=void;
  template<class String> static auto View(const CpuImageKey<String>& key) {
    return std::tuple<const std::array<uint32_t,6>&,std::optional<std::string_view>,
        std::optional<std::string_view>,uint64_t>(key.fetch,CpuImageDigestView(key.base),
        CpuImageDigestView(key.mips),key.version);
  }
  template<class A,class B> bool operator()(const CpuImageKey<A>& a,const CpuImageKey<B>& b)const {
    return View(a)<View(b);
  }
};
struct CpuImageEqual {
  template<class A,class B> bool operator()(const CpuImageKey<A>& a,const CpuImageKey<B>& b)const {
    return a.version==b.version&&a.fetch==b.fetch&&
        CpuImageDigestView(a.base)==CpuImageDigestView(b.base)&&
        CpuImageDigestView(a.mips)==CpuImageDigestView(b.mips);
  }
};
using CpuImageIdentity=CpuImageKey<std::string>;
using CpuImageLookup=CpuImageKey<std::string_view>;

// CP-thread-only borrowed hints. The caller's map remains the sole owner and
// must outlive this helper, remain at the same address, and clear hints BEFORE
// any erase/clear/extract/swap. Insertions do not invalidate std::map iterators.
// No key, resource, or additional map node is copied or allocated here.
template<class Map,class Equal> class BorrowedSlotLookup {
 public:
  using iterator=typename Map::iterator;
  static constexpr size_t kSlots=32;
  enum class ResetReason { NonDraw, NonCpuTexture, BeforeErase };
  struct Statistics {
    uint64_t lookups=0,hits=0,tree_lookups=0,slot_bypasses=0,stores=0;
    uint64_t resets=0,invalidated_slots=0,non_draw_resets=0,non_cpu_resets=0,erase_resets=0;
  };
  explicit BorrowedSlotLookup(Map& entries,bool enabled=false):entries_(entries),enabled_(enabled){}
  BorrowedSlotLookup(const BorrowedSlotLookup&)=delete;
  BorrowedSlotLookup& operator=(const BorrowedSlotLookup&)=delete;
  BorrowedSlotLookup(BorrowedSlotLookup&&)=delete;
  BorrowedSlotLookup& operator=(BorrowedSlotLookup&&)=delete;
  const Statistics& statistics()const noexcept{return statistics_;}
  template<class Key> iterator Find(size_t slot,const Key& key) {
    if(!enabled_)return entries_.find(key);
    ++statistics_.lookups;
    if(slot<kSlots) {
      if((valid_mask_&(uint32_t{1}<<slot))&&Equal{}(positions_[slot]->first,key)) {
        ++statistics_.hits;return positions_[slot];
      }
    } else ++statistics_.slot_bypasses;
    ++statistics_.tree_lookups;
    auto found=entries_.find(key);
    Remember(slot,found);
    return found;
  }
  void Remember(size_t slot,iterator found)noexcept {
    if(!enabled_||slot>=kSlots)return;
    if(found==entries_.end()) {valid_mask_&=~(uint32_t{1}<<slot);return;}
    positions_[slot]=found;valid_mask_|=uint32_t{1}<<slot;++statistics_.stores;
  }
  void Reset(ResetReason reason)noexcept {
    if(!enabled_)return;
    CountReset(reason);
    statistics_.invalidated_slots+=std::popcount(valid_mask_);
    valid_mask_=0;
  }
  void ResetSlot(size_t slot,ResetReason reason)noexcept {
    if(!enabled_)return;
    CountReset(reason);
    if(slot<kSlots) {
      const auto bit=uint32_t{1}<<slot;
      statistics_.invalidated_slots+=bool(valid_mask_&bit);valid_mask_&=~bit;
    }
  }
 private:
  void CountReset(ResetReason reason)noexcept {
    ++statistics_.resets;
    switch(reason) {
      case ResetReason::NonDraw:++statistics_.non_draw_resets;break;
      case ResetReason::NonCpuTexture:++statistics_.non_cpu_resets;break;
      case ResetReason::BeforeErase:++statistics_.erase_resets;break;
    }
  }
  Map& entries_;
  const bool enabled_;
  std::array<iterator,kSlots> positions_{};
  uint32_t valid_mask_=0;
  Statistics statistics_{};
};
} // namespace gow3::renderer
