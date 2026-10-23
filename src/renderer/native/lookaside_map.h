#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

namespace gow3::renderer {
// Same contract as ShaderLookupCache, for any ordered cache. The map remains
// the owner and defines eviction/tie order; the index only borrows resident
// iterators. Erase invalidates before destruction; exact key equality, never
// the index hash, authorizes a hit. CP-thread only.
template<class Key,class Value,class Less,class Index,class Equal> class LookasideMap {
  using Map=std::map<Key,Value,Less>;
 public:
  using iterator=typename Map::iterator;
  using const_iterator=typename Map::const_iterator;
  using key_type=Key;
  using mapped_type=Value;
  static constexpr size_t kSlots=1024;
  struct Statistics {uint64_t lookups=0,hits=0,tree_lookups=0,invalidations=0;};
  explicit LookasideMap(bool enabled=false):enabled_(enabled){}
  LookasideMap(const LookasideMap&)=delete;
  LookasideMap& operator=(const LookasideMap&)=delete;
  LookasideMap(LookasideMap&&)=delete;
  LookasideMap& operator=(LookasideMap&&)=delete;
  bool enabled()const noexcept{return enabled_;}
  const Statistics& statistics()const noexcept{return statistics_;}
  size_t size()const noexcept{return entries_.size();}
  bool empty()const noexcept{return entries_.empty();}
  iterator begin()noexcept{return entries_.begin();}
  iterator end()noexcept{return entries_.end();}
  const_iterator begin()const noexcept{return entries_.begin();}
  const_iterator end()const noexcept{return entries_.end();}
  template<class K> iterator find(const K& key) {
    if(!enabled_)return entries_.find(key);
    ++statistics_.lookups;auto& slot=slots_[Index{}(key)&(kSlots-1)];
    if(slot.valid&&Equal{}(slot.position->first,key)){++statistics_.hits;return slot.position;}
    ++statistics_.tree_lookups;auto found=entries_.find(key);
    if(found!=entries_.end()){slot.position=found;slot.valid=true;}
    return found;
  }
  template<class K> const_iterator find(const K& key)const {return entries_.find(key);}
  template<class K> bool contains(const K& key){return find(key)!=end();}
  template<class... Args> auto emplace(Args&&... args){return entries_.emplace(std::forward<Args>(args)...);}
  iterator erase(const_iterator position) {
    if(enabled_) {
      auto& slot=slots_[Index{}(position->first)&(kSlots-1)];
      if(slot.valid&&slot.position==position){slot.valid=false;++statistics_.invalidations;}
    }
    return entries_.erase(position);
  }
  iterator erase(iterator position){return erase(const_iterator(position));}
  void clear()noexcept {
    if(enabled_)for(auto& slot:slots_)if(slot.valid){slot.valid=false;++statistics_.invalidations;}
    entries_.clear();
  }
 private:
  struct Slot {iterator position{};bool valid=false;};
  Map entries_;
  const bool enabled_;
  std::array<Slot,kSlots> slots_{};
  Statistics statistics_{};
};
} // namespace gow3::renderer
