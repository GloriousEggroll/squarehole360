#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gow3::renderer {

struct DormantCacheEntry {
  uint64_t last_used_epoch = 0;
  uint64_t allocation_bytes = 0;
};

struct DormantCacheStatistics {
  uint64_t hits = 0, misses = 0, creations = 0, dormant_hits = 0;
  uint64_t evictions = 0, budget_evictions = 0, age_evictions = 0;
  uint64_t binding_evictions = 0, wrap_evictions = 0;
  uint64_t resident_bytes = 0, peak_resident_bytes = 0;
  uint64_t peak_dormant_bytes = 0, peak_dormant_entries = 0;
};

enum class DormantCacheEvictionKind : uint8_t { Budget, DisabledBudget, Age, Bindings, Wrap };
struct DormantCacheNoEvictionObserver { static constexpr bool kEnabled=false; };

// A frame's working set keeps its existing ownership until the caller's frame
// fence. Only extra resources absent from that frame use the dormant budget.
// The map entry owns its resource and this fixed-size metadata; hits allocate
// no LRU nodes or secondary keys. Equal-age entries retire in map order.
class DormantCache {
 public:
  struct Limits {
    uint64_t bytes;
    uint32_t entries;
    uint32_t idle_epochs;
  };
  struct Occupancy {
    uint64_t resident_entries = 0, dormant_entries = 0, dormant_bytes = 0;
  };

  explicit DormantCache(Limits limits, uint64_t first_epoch = 1)
      : limits_(limits), epoch_(first_epoch) {
    if (!epoch_) throw std::invalid_argument("Dormant cache epoch must be nonzero");
  }

  void Access(DormantCacheEntry& entry, bool hit) {
    if (entry.last_used_epoch > epoch_)
      throw std::logic_error("Dormant cache entry belongs to a future epoch");
    if (hit) {
      ++statistics_.hits;
      if (ExtraDormant(entry)) ++statistics_.dormant_hits;
    } else {
      ++statistics_.misses;
    }
    // Mark before resource creation or descriptor binding so a preparing draw
    // is protected exactly like every earlier draw in this frame.
    entry.last_used_epoch = epoch_;
  }

  void Created(DormantCacheEntry& entry, uint64_t allocation_bytes) {
    if (entry.last_used_epoch != epoch_ || entry.allocation_bytes || !allocation_bytes ||
        allocation_bytes > UINT64_MAX - statistics_.resident_bytes)
      throw std::logic_error("Invalid dormant cache allocation accounting");
    entry.allocation_bytes = allocation_bytes;
    statistics_.resident_bytes += allocation_bytes;
    statistics_.peak_resident_bytes = std::max(statistics_.peak_resident_bytes,
                                             statistics_.resident_bytes);
    ++statistics_.creations;
  }

  bool Used(const DormantCacheEntry& entry) const {
    return entry.last_used_epoch == epoch_;
  }
  const DormantCacheStatistics& statistics() const { return statistics_; }
  const Limits& limits() const { return limits_; }

  template <class Cache>
  Occupancy occupancy(const Cache& cache) const {
    Occupancy result;
    result.resident_entries = cache.size();
    for (const auto& [key, entry] : cache) if (ExtraDormant(entry.residency)) {
      ++result.dormant_entries;
      result.dormant_bytes += entry.residency.allocation_bytes;
    }
    return result;
  }

  // The caller has completed the existing full frame fence. No fence or GPU
  // operation is introduced here. Current-frame entries always survive, even
  // if that already-existing working set exceeds the extra dormant budget.
  template <class Cache, class BeforeErase, class Observer=DormantCacheNoEvictionObserver>
  void EndFrame(Cache& cache, BeforeErase before_erase, Observer observer={}) {
    if (epoch_ == UINT64_MAX) {
      // Never resurrect an inactive resource or extend its TTL at wrap. The
      // completed current working set maps to epoch zero (the previous frame
      // when the next epoch becomes one).
      for (auto it = cache.begin(); it != cache.end();) {
        if (Used(it->second.residency)) {
          ++it;
        } else {
          auto retired = it++;
          EraseObserved(cache, retired, before_erase,observer,DormantCacheEvictionKind::Wrap);
          ++statistics_.wrap_evictions;
        }
      }
      for (auto& [key, entry] : cache) entry.residency.last_used_epoch = 0;
      epoch_ = 1;
      return;
    }
    uint64_t dormant_bytes = 0;
    size_t dormant_entries = 0;
    for (auto it = cache.begin(); it != cache.end();) {
      const auto& entry = it->second.residency;
      if (Used(entry)) { ++it; continue; }
      if (entry.last_used_epoch > epoch_)
        throw std::logic_error("Dormant cache frame ordering changed");
      if (epoch_ - entry.last_used_epoch > limits_.idle_epochs) {
        auto retired = it++;
        EraseObserved(cache, retired, before_erase,observer,DormantCacheEvictionKind::Age);
        ++statistics_.age_evictions;
      } else if (!limits_.bytes || !limits_.entries) {
        // With extra residency disabled every inactive owner retires. There
        // is no victim selection to perform and no reason to allocate/sort an
        // LRU scratch list. Current-frame owners remain protected above.
        auto retired = it++;
        EraseObserved(cache, retired, before_erase,observer,DormantCacheEvictionKind::DisabledBudget);
        ++statistics_.budget_evictions;
      } else {
        ++dormant_entries; dormant_bytes += entry.allocation_bytes; ++it;
      }
    }
    // Usually the extra working set already fits: no sorting, scratch vector,
    // or per-entry LRU allocation is needed on that frame's retention path.
    if (dormant_entries > limits_.entries || dormant_bytes > limits_.bytes) {
      std::vector<typename Cache::iterator> dormant;
      dormant.reserve(dormant_entries);
      for (auto it = cache.begin(); it != cache.end(); ++it)
        if (!Used(it->second.residency)) dormant.push_back(it);
      std::stable_sort(dormant.begin(), dormant.end(), [](auto a, auto b) {
        return a->second.residency.last_used_epoch < b->second.residency.last_used_epoch;
      });
      size_t first = 0;
      while (dormant_entries > limits_.entries || dormant_bytes > limits_.bytes) {
        const auto retired = dormant[first++];
        bool entry_pressure=false,byte_pressure=false;
        if constexpr(Observer::kEnabled) {
          entry_pressure=dormant_entries>limits_.entries;
          byte_pressure=dormant_bytes>limits_.bytes;
        }
        dormant_bytes -= retired->second.residency.allocation_bytes;
        EraseObserved(cache, retired, before_erase,observer,DormantCacheEvictionKind::Budget,
                      entry_pressure,byte_pressure);
        --dormant_entries; ++statistics_.budget_evictions;
      }
    }
    statistics_.peak_dormant_bytes = std::max(statistics_.peak_dormant_bytes, dormant_bytes);
    statistics_.peak_dormant_entries = std::max(statistics_.peak_dormant_entries,
                                               uint64_t(dormant_entries));
    ++epoch_;
  }

  // Reclaim inactive images when their bindings occupy the existing global
  // descriptor-cache limit. This cannot release a current/preparing-frame
  // entry; its prior use completed at the previous frame's normal fence.
  template <class Cache, class BeforeErase, class Eligible, class Observer=DormantCacheNoEvictionObserver>
  bool EvictUnusedForBindings(Cache& cache, BeforeErase before_erase, Eligible eligible,Observer observer={}) {
    auto oldest = cache.end();
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (Used(it->second.residency) || !eligible(it->second)) continue;
      if (oldest == cache.end() || it->second.residency.last_used_epoch <
                                     oldest->second.residency.last_used_epoch) oldest = it;
    }
    if (oldest == cache.end()) return false;
    EraseObserved(cache, oldest, before_erase,observer,DormantCacheEvictionKind::Bindings);
    ++statistics_.binding_evictions;
    return true;
  }
  // Repeated EvictUnusedForBindings until done() holds, with one scan: erasing
  // an entry changes no other entry's eligibility, so the victims and their
  // order (oldest first, earlier iteration first on ties) are the same.
  template <class Cache, class BeforeErase, class Eligible, class Done, class Observer=DormantCacheNoEvictionObserver>
  size_t EvictUnusedForBindingsUntil(Cache& cache, BeforeErase before_erase, Eligible eligible, Done done,
                                     Observer observer={}) {
    if (done()) return 0;
    std::vector<typename Cache::iterator> victims;
    for (auto it = cache.begin(); it != cache.end(); ++it)
      if (!Used(it->second.residency) && eligible(it->second)) victims.push_back(it);
    std::stable_sort(victims.begin(), victims.end(), [](auto a, auto b) {
      return a->second.residency.last_used_epoch < b->second.residency.last_used_epoch;
    });
    size_t erased = 0;
    for (const auto victim : victims) {
      if (done()) break;
      EraseObserved(cache, victim, before_erase,observer,DormantCacheEvictionKind::Bindings);
      ++statistics_.binding_evictions; ++erased;
    }
    return erased;
  }

 private:
  // The original erase remains the sole owner release and statistics update.
  // With the default observer, no clock, key copy or diagnostic call exists.
  template<class Cache,class BeforeErase,class Observer>
  void EraseObserved(Cache& cache,typename Cache::iterator entry,BeforeErase before_erase,
      Observer observer,DormantCacheEvictionKind kind,bool entry_pressure=false,bool byte_pressure=false) {
    if constexpr(Observer::kEnabled) {
      const auto observation=observer.Begin(entry->first,entry->second,kind,entry_pressure,byte_pressure);
      Erase(cache,entry,before_erase);
      observer.End(observation);
    } else Erase(cache,entry,before_erase);
  }
  bool ExtraDormant(const DormantCacheEntry& entry) const {
    return epoch_ > 1 && entry.last_used_epoch < epoch_ - 1;
  }

  template <class Cache, class BeforeErase>
  void Erase(Cache& cache, typename Cache::iterator entry, BeforeErase before_erase) {
    const auto bytes = entry->second.residency.allocation_bytes;
    if (bytes > statistics_.resident_bytes)
      throw std::logic_error("Dormant cache allocation accounting underflow");
    // Hooks adjust binding counts and must run before destruction. If the hook
    // rejects inconsistent ownership, this entry and its byte count stay intact.
    before_erase(entry->second);
    statistics_.resident_bytes -= bytes;
    cache.erase(entry);
    ++statistics_.evictions;
  }

  Limits limits_;
  uint64_t epoch_;
  DormantCacheStatistics statistics_;
};

}  // namespace gow3::renderer
