#pragma once
#include "cache_bookkeeping.h"
#include <array>
#include <cstring>
#include <utility>

namespace gow3::renderer {
// The ordered map remains the owner and defines eviction/tie order. The small
// index only borrows resident iterators. Erase invalidates before destruction;
// exact full key equality, never the index hash, authorizes a hit. CP-thread only.
template<class Value> class ShaderLookupCache {
  using Map=std::map<ShaderCacheIdentity,Value,ShaderCacheLess>;
 public:
  using iterator=typename Map::iterator;
  using const_iterator=typename Map::const_iterator;
  static constexpr size_t kSlots=256;
  struct Statistics {uint64_t lookups=0,hits=0,tree_lookups=0,invalidations=0;};
  explicit ShaderLookupCache(bool enabled=false):enabled_(enabled){}
  ShaderLookupCache(const ShaderLookupCache&)=delete;
  ShaderLookupCache& operator=(const ShaderLookupCache&)=delete;
  ShaderLookupCache(ShaderLookupCache&&)=delete;
  ShaderLookupCache& operator=(ShaderLookupCache&&)=delete;
  bool enabled()const noexcept{return enabled_;}
  const Statistics& statistics()const noexcept{return statistics_;}
  size_t size()const noexcept{return entries_.size();}
  bool empty()const noexcept{return entries_.empty();}
  iterator begin()noexcept{return entries_.begin();}
  iterator end()noexcept{return entries_.end();}
  const_iterator begin()const noexcept{return entries_.begin();}
  const_iterator end()const noexcept{return entries_.end();}
  template<class Key> iterator find(const Key& key) {
    if(!enabled_)return entries_.find(key);
    ++statistics_.lookups;auto& slot=slots_[Index(key)];
    if(slot.valid&&Equal(slot.position->first,key)){++statistics_.hits;return slot.position;}
    ++statistics_.tree_lookups;auto found=entries_.find(key);
    if(found!=entries_.end()){slot.position=found;slot.valid=true;}
    return found;
  }
  template<class Key> const_iterator find(const Key& key)const {
    if(!enabled_)return entries_.find(key);
    ++statistics_.lookups;const auto& slot=slots_[Index(key)];
    if(slot.valid&&Equal(slot.position->first,key)){++statistics_.hits;return slot.position;}
    // Const probes may use a warmed iterator, but never manufacture a mutable
    // iterator or change ownership to seed it. Ordinary find fills the index.
    ++statistics_.tree_lookups;return entries_.find(key);
  }
  template<class Key> bool contains(const Key& key){return find(key)!=end();}
  template<class Key> bool contains(const Key& key)const{return find(key)!=end();}
  template<class... Args> auto emplace(Args&&... args){return entries_.emplace(std::forward<Args>(args)...);}
  iterator erase(const_iterator position) {
    if(enabled_) {
      auto& slot=slots_[Index(position->first)];
      if(slot.valid&&slot.position==position){slot.valid=false;++statistics_.invalidations;}
    }
    return entries_.erase(position);
  }
  void clear()noexcept {
    if(enabled_)for(auto& slot:slots_)if(slot.valid){slot.valid=false;++statistics_.invalidations;}
    entries_.clear();
  }
 private:
  template<class Key> static size_t Index(const Key& key)noexcept {
    const std::string_view digest(key.digest);
    // At most16 digest bytes, unaligned-safe and bounded for builtin short keys.
    // Interior differences intentionally collide and still require Equal().
    uint64_t first=0,last=0;const auto bytes=std::min(digest.size(),sizeof(uint64_t));
    if(bytes){std::memcpy(&first,digest.data(),bytes);std::memcpy(&last,digest.data()+digest.size()-bytes,bytes);}
    uint64_t mixed=first^(last*0x9e3779b97f4a7c15ull)^uint64_t(digest.size());
    mixed^=uint64_t(key.variant)*0xbf58476d1ce4e5b9ull;
    mixed^=key.fragment_interlock?0x94d049bb133111ebull:0;
    mixed^=mixed>>32;mixed^=mixed>>16;mixed^=mixed>>8;
    return size_t(mixed)&(kSlots-1);
  }
  template<class Key> static bool Equal(const ShaderCacheIdentity& a,const Key& b)noexcept {
    return a.fragment_interlock==b.fragment_interlock&&a.variant==b.variant&&
      std::string_view(a.digest)==std::string_view(b.digest);
  }
  struct Slot {iterator position{};bool valid=false;};
  Map entries_;
  const bool enabled_;
  std::array<Slot,kSlots> slots_{};
  mutable Statistics statistics_{};
};
} // namespace gow3::renderer
