#include "renderer/native/cache_bookkeeping.h"
#include "renderer/native/shader_lookup_cache.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
bool count_allocations = false;
size_t allocations = 0;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Entry {
  uint64_t last_used_epoch = 0;
  uint32_t identity = 0;
  uint64_t bytes = 0;
};
struct OracleCache {
  std::map<uint32_t, uint32_t> old_entries;
  std::set<uint32_t> old_used;
  std::map<uint32_t, Entry> entries;
  void Touch(uint32_t key, const gow3::renderer::CacheFrameEpoch& epoch) {
    old_entries.try_emplace(key, key);
    old_used.insert(key);
    auto [it, inserted] = entries.try_emplace(key, Entry{0, key});
    epoch.Mark(it->second);
  }
  size_t End(const gow3::renderer::CacheFrameEpoch& epoch) {
    // Original implementation: erase unused entries using a separate used set.
    std::erase_if(old_entries, [&](const auto& entry) {
      return !old_used.contains(entry.first);
    });
    old_used.clear();
    epoch.Retire(entries);
    Check(entries.size() == old_entries.size(), "Cache eviction count differs");
    for (const auto& [key, identity] : old_entries) {
      Check(entries.contains(key), "Epoch cache evicted a used key");
      Check(entries.at(key).identity == identity, "Epoch cache changed resource identity");
    }
    return entries.size();
  }
};

size_t EpochOracle(uint64_t first) {
  gow3::renderer::CacheFrameEpoch epoch(first);
  std::array<OracleCache, 3> caches;
  uint32_t random = 0x6d2b79f5;
  size_t comparisons = 0;
  // Repeated and decreasing guest IDs deliberately do not control the epoch.
  constexpr std::array<uint32_t, 8> guest_frames{7, 7, 8, 0, 0, 9, 2, 2};
  for (size_t frame = 0; frame < 512; ++frame) {
    const auto guest_frame = guest_frames[frame % guest_frames.size()];
    for (size_t i = 0; i < caches.size(); ++i) {
      auto& cache = caches[i];
      // Empty intervals, existing hits, repeated hits, new resources and
      // independently used image/pipeline/shader caches must retire identically.
      const size_t touches = (frame + i) % 13 == 0 ? 0 : (frame + guest_frame + i) % 37;
      for (size_t j = 0; j < touches; ++j) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const uint32_t key = j % 3 ? random % 257 : 11;
        cache.Touch(key, epoch);
        cache.Touch(key, epoch);
      }
      comparisons += cache.End(epoch) + 1;
    }
    epoch.Advance(caches[0].entries, caches[1].entries, caches[2].entries);
  }
  // One final empty interval must evict everything, including after wrap.
  for (auto& cache : caches) {
    comparisons += cache.End(epoch) + 1;
    Check(cache.entries.empty(), "Empty frame retained stale resources");
  }
  return comparisons;
}

size_t ShaderKeys() {
  using namespace gow3::renderer;
  std::vector<ShaderCacheIdentity> keys;
  for (char digit : std::string_view("0123456789abcdef")) {
    std::string digest(64, 'a');
    digest[63] = digit;
    keys.push_back({digest, false});
    keys.push_back({digest, true});
  }
  keys.push_back({"depth-only", true});
  std::map<std::string, size_t> old;
  std::map<ShaderCacheIdentity, size_t, ShaderCacheLess> typed;
  for (size_t i = 0; i < keys.size(); ++i) {
    const auto& key = keys[i];
    old.emplace(key.digest + (key.fragment_interlock ? ".fsi" : ""), i);
    typed.emplace(key, i);
  }
  Check(old.size() == typed.size(), "Shader key domains differ");
  const ShaderCacheLess less;
  size_t comparisons = 0;
  for (const auto& a : keys) for (const auto& b : keys) {
    const auto old_a = a.digest + (a.fragment_interlock ? ".fsi" : "");
    const auto old_b = b.digest + (b.fragment_interlock ? ".fsi" : "");
    const ShaderCacheLookup borrowed{b.digest, b.fragment_interlock};
    Check((old_a == old_b) == (!less(a, borrowed) && !less(borrowed, a)),
          "Shader key equality differs from original suffix key");
    ++comparisons;
  }
  allocations = 0;
  count_allocations = true;
  for (size_t iteration = 0; iteration < 100; ++iteration) {
    for (size_t i = 0; i < keys.size(); ++i) {
      const ShaderCacheLookup key{keys[i].digest, keys[i].fragment_interlock};
      const auto found = typed.find(key);
      Check(found != typed.end() && found->second == i, "Borrowed shader lookup failed");
    }
  }
  count_allocations = false;
  Check(!allocations, "Shader cache hits allocated temporary keys");
  return comparisons;
}

void PersistentCaches() {
  using namespace gow3::renderer;
  CacheFrameEpoch epoch;
  PersistentCacheStatistics stats;
  std::map<uint32_t,Entry> cache;
  unsigned drains=0;
  auto weight=[](const Entry& entry){return entry.bytes;};
  auto use=[&](uint32_t key,uint64_t bytes,size_t limit,uint64_t byte_limit,uint32_t pinned) {
    auto found=cache.find(key);
    if(found==cache.end()) {
      ++stats.misses;
      ReservePersistentCache(cache,bytes,limit,byte_limit,stats,weight,
          [&](const Entry& entry){return entry.identity==pinned;},[&]{++drains;});
      found=cache.emplace(key,Entry{0,key,bytes}).first;
      stats.bytes+=bytes;++stats.creations;
    } else ++stats.hits;
    epoch.Mark(found->second);
  };
  auto end=[&](size_t target,uint64_t bytes) {
    TrimPersistentCache(cache,target,bytes,stats,weight,[&](const Entry& e){return !epoch.Used(e);});
    epoch.Advance(cache);
  };
  use(1,40,4,100,0);end(3,75);
  use(2,40,4,100,0);end(3,100);
  use(1,40,4,100,0);
  Check(stats.creations==2&&stats.hits==1&&cache.size()==2,
        "A/B/A persistent reuse recreated an absent-frame entry");
  end(3,100);
  // The least-recent entry is protected by the preparing draw. Byte pressure
  // must fence, evict the other entry, and keep the selected shader identity.
  use(3,50,4,100,2);
  Check(drains==1&&stats.pressure_drains==1&&cache.contains(2)&&!cache.contains(1)&&
        stats.bytes==90&&stats.evictions==1,"Byte pressure lost a protected preparing shader");
  const auto old_bytes=stats.bytes,old_evictions=stats.evictions;
  bool rejected=false;
  try { use(4,70,4,100,2); } catch(const std::runtime_error&) { rejected=true; }
  Check(rejected&&drains==1&&stats.bytes==old_bytes&&stats.evictions==old_evictions&&cache.size()==2,
        "Impossible pinned admission changed cache accounting or drained");
  rejected=false;
  try { use(4,101,4,100,0); } catch(const std::runtime_error&) { rejected=true; }
  Check(rejected&&drains==1&&stats.bytes==old_bytes,"Oversized cache admission changed residency");
  // Count pressure is independent of byte pressure. The protected entry stays
  // resident even when it is older and therefore the natural LRU candidate.
  use(4,1,2,100,2);
  Check(drains==2&&cache.contains(2)&&cache.contains(4)&&!cache.contains(3)&&stats.bytes==41,
        "Count pressure evicted the selected shader or exceeded its bound");
  // Soft trimming retains current working entries even above the target, then
  // a fully idle frame can reclaim them. Epoch wrap remains a valid age reset.
  epoch.Mark(cache.at(2));end(0,0);
  Check(cache.size()==2,"Soft retention evicted the current frame working set");
  end(0,0);Check(cache.empty()&&stats.bytes==0,"Idle trim retained excess stale entries");
  CacheFrameEpoch wrapping(UINT64_MAX);
  cache.emplace(9,Entry{0,9,7});stats.bytes=7;wrapping.Mark(cache.at(9));wrapping.Advance(cache);
  TrimPersistentCache(cache,0,0,stats,weight,[&](const Entry& e){return !wrapping.Used(e);});
  Check(cache.empty()&&stats.bytes==0,"Wrapped epoch made a stale persistent entry current");
}
size_t ShaderLookupOracle() {
  using namespace gow3::renderer;
  size_t checks=0;
  const auto variant=NativeShaderVariant::ImplicitCoherentInterlockV1;
  // Same prefix/suffix/length => forced index collisions, but distinct full keys.
  std::vector<ShaderCacheIdentity> keys;
  for(unsigned i=0;i<48;++i) {
    std::string digest(64,'a');digest[20]=char('A'+i/26);digest[21]=char('A'+i%26);
    keys.push_back({digest,false});keys.push_back({digest,true});keys.push_back({digest,true,variant});
  }
  keys.push_back({"depth-only",true});keys.push_back({"depth-only",true,variant});
  keys.push_back({"",false});keys.push_back({std::string("a\0b",3),true});
  for(size_t n:{1u,7u,8u,9u})keys.push_back({std::string(n,'b'),false});
  for(unsigned v=0;v<=unsigned(NativeShaderVariant::ConservativeEarlyDepthCatalogV2);++v)
    keys.push_back({std::string(64,'c'),true,NativeShaderVariant(v)});
  for(bool enabled:{false,true}) {
    ShaderLookupCache<Entry> cache(enabled);
    std::map<ShaderCacheIdentity,Entry,ShaderCacheLess> oracle;
    PersistentCacheStatistics actual_stats,oracle_stats;
    CacheFrameEpoch epoch;unsigned actual_drains=0,oracle_drains=0;
    auto weight=[](const Entry& e){return e.bytes;};
    auto same=[&] {
      Check(cache.size()==oracle.size(),"Lookaside changed map size");
      auto actual=cache.begin();
      for(const auto& expected:oracle) {
        Check(actual!=cache.end()&&actual->first.digest==expected.first.digest&&
          actual->first.fragment_interlock==expected.first.fragment_interlock&&actual->first.variant==expected.first.variant&&
          actual->second.identity==expected.second.identity&&actual->second.last_used_epoch==expected.second.last_used_epoch,
          "Lookaside changed full-key ordering/resource identity/epoch");++actual;++checks;
      }
      Check(actual_stats.bytes==oracle_stats.bytes&&actual_stats.evictions==oracle_stats.evictions&&
        actual_drains==oracle_drains,"Lookaside changed eviction bytes or drains");
    };
    for(size_t step=0;step<1200;++step) {
      const auto index=(step*37+step/7)%keys.size();const auto& key=keys[index];
      const ShaderCacheLookup lookup{key.digest,key.fragment_interlock,key.variant};
      auto expected=oracle.find(lookup),actual=cache.find(lookup);
      Check((actual==cache.end())==(expected==oracle.end()),"Lookaside returned stale/false hit");++checks;
      if(expected==oracle.end()) {
        const uint64_t bytes=1+index%9;
        const uint32_t pinned=oracle.empty()?UINT32_MAX:oracle.begin()->second.identity;
        auto protect=[&](const Entry& e){return e.identity==pinned;};
        ReservePersistentCache(cache,bytes,11,67,actual_stats,weight,protect,[&]{++actual_drains;});
        ReservePersistentCache(oracle,bytes,11,67,oracle_stats,weight,protect,[&]{++oracle_drains;});
        actual=cache.emplace(key,Entry{0,uint32_t(index),bytes}).first;
        expected=oracle.emplace(key,Entry{0,uint32_t(index),bytes}).first;
        actual_stats.bytes+=bytes;oracle_stats.bytes+=bytes;
      }
      epoch.Mark(actual->second);epoch.Mark(expected->second);
      Check(cache.find(lookup)->second.identity==expected->second.identity,"Lookaside normal hit differs");
      const auto& frozen=cache;
      Check(frozen.find(lookup)->second.identity==expected->second.identity&&frozen.contains(lookup),"Const lookaside differs");
      // Borrowed lookup storage may disappear/change immediately after find.
      auto borrowed=key.digest;const ShaderCacheLookup temporary{borrowed,key.fragment_interlock,key.variant};
      Check(cache.find(temporary)->second.identity==expected->second.identity,"Borrowed key lookup differs");borrowed.assign(4096,'z');
      if(step%17==0) {
        auto eligible=[&](const Entry& e){return !epoch.Used(e);};
        TrimPersistentCache(cache,3,20,actual_stats,weight,eligible);
        TrimPersistentCache(oracle,3,20,oracle_stats,weight,eligible);epoch.Advance(cache,oracle);
      }
      same();
      if(step%101==0) {
        // Explicitly warm, erase and recreate the exact key. New value must win
        // even when a node allocator reuses the same address (ABA).
        auto found=cache.find(lookup),reference=oracle.find(lookup);
        if(found!=cache.end()) {
          const auto bytes=found->second.bytes;cache.erase(found);oracle.erase(reference);
          Check(!cache.contains(lookup)&&!frozen.contains(lookup),"Erased iterator escaped invalidation");
          cache.emplace(key,Entry{0,uint32_t(index)+1000,bytes});oracle.emplace(key,Entry{0,uint32_t(index)+1000,bytes});
          Check(cache.find(lookup)->second.identity==index+1000,"Recreated key retained stale resource");same();
        }
      }
    }
    const auto stable=keys[0];cache.clear();oracle.clear();
    Check(cache.find(ShaderCacheLookup{stable.digest,stable.fragment_interlock,stable.variant})==cache.end(),"Clear retained iterator");
    cache.emplace(stable,Entry{0,991,1});
    const ShaderCacheLookup lookup{stable.digest,stable.fragment_interlock,stable.variant};
    Check(cache.find(lookup)->second.identity==991,"Reinsert after clear failed");
    allocations=0;count_allocations=true;
    for(size_t i=0;i<1000;++i)Check(cache.find(lookup)->second.identity==991,"Warm lookaside identity differs");
    count_allocations=false;Check(!allocations,"Warm lookaside allocated");
    const auto& stats=cache.statistics();
    Check(enabled?(stats.lookups==stats.hits+stats.tree_lookups&&stats.hits>=1000&&stats.invalidations>0):
      (!stats.lookups&&!stats.hits&&!stats.tree_lookups&&!stats.invalidations),"Lookaside mechanism/OFF conservation failed");
  }
  return checks;
}
}  // namespace

void* operator new(std::size_t size) {
  if (count_allocations) ++allocations;
  if (void* result = std::malloc(size ? size : 1)) return result;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
  try {
    const auto ordinary = EpochOracle(1);
    const auto wrapped = EpochOracle(UINT64_MAX - 2);
    Check(ordinary == wrapped, "Epoch wrap changed cache decisions");
    bool rejected = false;
    try { gow3::renderer::CacheFrameEpoch invalid(0); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Zero epoch was accepted");
    const auto shaders = ShaderKeys();
    PersistentCaches();
    const auto lookaside=ShaderLookupOracle();
    std::cout << "Shader lookaside oracle passed " << lookaside << " full-key/lifetime checks; OFF/ON collision, variant, pressure, trim, ABA, const and allocation-free hits\n";
    std::cout << "Cache epoch oracle passed " << ordinary + wrapped
              << " retained-entry/frame checks and " << shaders
              << " shader key comparisons; 3300 borrowed hits allocated zero keys; persistent A/B/A, byte/count pressure, pinned admission and trim passed\n";
    return 0;
  } catch (const std::exception& error) {
    count_allocations = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
