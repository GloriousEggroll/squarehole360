#pragma once

#include "renderer/native/fsi_shader_variant.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace gow3::renderer {

// Independent of guest frame numbers: repeated Present IDs still delimit
// distinct cache-use intervals. Retire is called at the existing fenced frame
// boundary, never when work may still reference the cached resource.
class CacheFrameEpoch {
 public:
  explicit CacheFrameEpoch(uint64_t first = 1) : current_(first) {
    if (!first) throw std::invalid_argument("Cache frame epoch must be nonzero");
  }

  template <class Entry> void Mark(Entry& entry) const {
    entry.last_used_epoch = current_;
  }
  template <class Entry> bool Used(const Entry& entry) const {
    return entry.last_used_epoch == current_;
  }
  template <class Cache> void Retire(Cache& cache) const {
    std::erase_if(cache, [&](const auto& item) {
      return item.second.last_used_epoch != current_;
    });
  }
  template <class... Caches> void Advance(Caches&... caches) {
    if (current_ != std::numeric_limits<uint64_t>::max()) {
      ++current_;
      return;
    }
    // Retained entries may still carry a very old epoch 1. Reset all stamps
    // before reusing that value; this preserves the next empty frame's eviction.
    auto reset = [](auto& cache) {
      for (auto& [key, entry] : cache) entry.last_used_epoch = 0;
    };
    (reset(caches), ...);
    current_ = 1;
  }

 private:
  uint64_t current_;
};

struct PersistentCacheStatistics {
  uint64_t hits=0,misses=0,creations=0,evictions=0,pressure_drains=0,bytes=0;
};

// Entries are LRU by internal frame epoch; equal-age ties use map order. The
// caller owns the GPU fence and selected-resource predicate, so neither a
// pending draw nor a shader selected for the preparing draw can be invalidated.
template <class Cache,class Eligible>
auto OldestCacheEntry(Cache& cache,Eligible eligible) {
  auto oldest=cache.end();
  for(auto it=cache.begin();it!=cache.end();++it)
    if(eligible(it->second)&&(oldest==cache.end()||
        it->second.last_used_epoch<oldest->second.last_used_epoch))oldest=it;
  return oldest;
}

template <class Cache,class Weight,class Protected,class Drain>
void ReservePersistentCache(Cache& cache,uint64_t incoming_bytes,size_t limit,
                            uint64_t byte_limit,PersistentCacheStatistics& stats,
                            Weight weight,Protected protect,Drain drain) {
  if(!limit||incoming_bytes>byte_limit)
    throw std::runtime_error("Native persistent cache admission exceeds its bound");
  auto fits=[&]{return cache.size()<limit&&stats.bytes<=byte_limit-incoming_bytes;};
  if(fits())return;
  // Reject impossible admission before either draining or evicting anything.
  size_t pinned=0;uint64_t pinned_bytes=0;
  for(const auto& [key,entry]:cache)if(protect(entry)) {
    ++pinned;const auto bytes=weight(entry);
    if(bytes>byte_limit-pinned_bytes)
      throw std::runtime_error("Native persistent cache protected bytes exceed its bound");
    pinned_bytes+=bytes;
  }
  if(pinned>=limit||pinned_bytes>byte_limit-incoming_bytes)
    throw std::runtime_error("Native persistent cache cannot retain the preparing draw");
  drain();++stats.pressure_drains;
  while(!fits()) {
    auto oldest=OldestCacheEntry(cache,[&](const auto& entry){return !protect(entry);});
    if(oldest==cache.end())throw std::logic_error("Native persistent cache has no reclaimable entry");
    const auto bytes=weight(oldest->second);
    if(bytes>stats.bytes)throw std::logic_error("Native persistent cache byte accounting underflow");
    stats.bytes-=bytes;cache.erase(oldest);++stats.evictions;
  }
}

template <class Cache,class Weight,class Eligible>
void TrimPersistentCache(Cache& cache,size_t target,uint64_t byte_target,
                         PersistentCacheStatistics& stats,Weight weight,Eligible eligible) {
  while(cache.size()>target||stats.bytes>byte_target) {
    auto oldest=OldestCacheEntry(cache,eligible);
    if(oldest==cache.end())break; // Keep the current frame's working set.
    const auto bytes=weight(oldest->second);
    if(bytes>stats.bytes)throw std::logic_error("Native persistent cache byte accounting underflow");
    stats.bytes-=bytes;cache.erase(oldest);++stats.evictions;
  }
}

template <class String> struct ShaderCacheKey {
  String digest;
  bool fragment_interlock;
  NativeShaderVariant variant = NativeShaderVariant::Unmodified;
};
using ShaderCacheIdentity = ShaderCacheKey<std::string>;
using ShaderCacheLookup = ShaderCacheKey<std::string_view>;
struct ShaderCacheLess {
  using is_transparent = void;
  template <class A, class B>
  bool operator()(const ShaderCacheKey<A>& a, const ShaderCacheKey<B>& b) const {
    return std::tuple(std::string_view(a.digest), a.fragment_interlock, a.variant) <
           std::tuple(std::string_view(b.digest), b.fragment_interlock, b.variant);
  }
};

}  // namespace gow3::renderer
