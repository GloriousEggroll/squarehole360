#include "renderer/native/cpu_image_slot_lookup.h"
#include "renderer/native/dormant_cache.h"

#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Check(bool ok,const char* message) {
  ++checks;if(!ok)throw std::runtime_error(message);
}
struct Value {
  DormantCacheEntry residency;
  std::unique_ptr<int> owner=std::make_unique<int>(17);
  bool binding=true;
};
using Map=std::map<CpuImageIdentity,Value,CpuImageLess>;
using Hints=BorrowedSlotLookup<Map,CpuImageEqual>;
CpuImageIdentity Key(uint64_t version=1) {
  return {{1,2,3,4,5,6},std::string("abcdefgh:middle:ijklmnop"),
      std::string("qrstuvwx:mipmap:yz012345"),version};
}
CpuImageLookup View(const CpuImageIdentity& key) {
  return {key.fetch,CpuImageDigestView(key.base),CpuImageDigestView(key.mips),key.version};
}
void ExactKeysAndSlots() {
  Map map;Hints hints(map,true);
  std::vector<CpuImageIdentity> keys{Key()};
  for(size_t word=0;word<6;++word) {auto key=Key();++key.fetch[word];keys.push_back(key);}
  auto changed=Key();++changed.version;keys.push_back(changed);
  changed=Key();changed.base->at(10)='X';keys.push_back(changed);
  changed=Key();changed.mips->at(10)='X';keys.push_back(changed);
  changed=Key();changed.base.reset();keys.push_back(changed);
  changed=Key();changed.base="";keys.push_back(changed);
  changed=Key();changed.mips.reset();keys.push_back(changed);
  changed=Key();changed.mips="";keys.push_back(changed);
  changed=Key();std::swap(changed.base,changed.mips);keys.push_back(changed);
  changed=Key();changed.base.reset();changed.mips.reset();keys.push_back(changed);
  changed=Key();changed.base="";changed.mips="";keys.push_back(changed);
  for(const auto& key:keys)map.try_emplace(key);
  // Actual production equality must agree with transparent map equivalence,
  // including null vs present-empty and interior digest bytes.
  for(const auto& a:keys)for(const auto& b:keys)
    Check(CpuImageEqual{}(a,View(b))==(!CpuImageLess{}(a,b)&&!CpuImageLess{}(b,a)),"key equivalence changed");
  for(size_t slot=0;slot<Hints::kSlots;++slot)for(const auto& key:keys) {
    auto found=hints.Find(slot,View(key));
    Check(found==map.find(View(key)),"lookaside differs from map oracle");
    auto before=hints.statistics().hits;
    Check(hints.Find(slot,View(key))==found&&hints.statistics().hits==before+1,"exact repeat must hit");
  }
  for(size_t slot:{size_t(32),size_t(255),SIZE_MAX}) {
    auto hits=hints.statistics().hits;
    Check(hints.Find(slot,View(keys.front()))==map.find(keys.front()),"out-of-range slot changed lookup");
    hints.Remember(slot,map.begin());
    Check(hints.statistics().hits==hits,"out-of-range slot borrowed iterator");
  }
  // A hinted map key owns its bytes after the originating lookup is gone.
  {auto temporary=Key();Check(hints.Find(0,View(temporary))==map.find(temporary),"temporary lookup failed");}
  auto key=Key();auto hits=hints.statistics().hits;
  Check(hints.Find(0,View(key))==map.find(key)&&hints.statistics().hits==hits+1,"lookup borrowed temporary bytes");
  auto position=hints.Find(0,View(key));const auto* owner=position->second.owner.get();
  map.try_emplace(Key(999));
  Check(hints.Find(0,View(key))->second.owner.get()==owner,"insertion changed ownership");
  for(auto reason:{Hints::ResetReason::NonDraw,Hints::ResetReason::NonCpuTexture,Hints::ResetReason::BeforeErase}) {
    hints.Reset(reason);hits=hints.statistics().hits;
    Check(hints.Find(0,View(key))==position&&hints.statistics().hits==hits,"reset retained a borrowed hint");
  }
  hints.Find(1,View(key));hints.ResetSlot(0,Hints::ResetReason::NonCpuTexture);
  hits=hints.statistics().hits;
  Check(hints.Find(1,View(key))==position&&hints.statistics().hits==hits+1,"nonCPU slot reset disturbed another slot");
  hits=hints.statistics().hits;
  Check(hints.Find(0,View(key))==position&&hints.statistics().hits==hits,"nonCPU slot reset retained its hint");
  hints.ResetSlot(SIZE_MAX,Hints::ResetReason::NonCpuTexture);
  hits=hints.statistics().hits;
  Check(hints.Find(0,View(key))==position&&hints.statistics().hits==hits+1,"invalid slot reset disturbed a valid slot");
  const auto& stats=hints.statistics();
  Check(stats.lookups==stats.hits+stats.tree_lookups,"lookup conservation failed");
  Check(stats.resets==stats.non_draw_resets+stats.non_cpu_resets+stats.erase_resets,"reset conservation failed");
  Check(stats.invalidated_slots>0&&stats.slot_bypasses==3,"coverage missing");
  Hints disabled(map);
  for(size_t slot=0;slot<34;++slot) {
    Check(disabled.Find(slot,View(key))==map.find(key),"disabled lookup differs");
    disabled.Remember(slot,position);disabled.Reset(Hints::ResetReason::BeforeErase);
  }
  Check(disabled.statistics().lookups==0&&disabled.statistics().stores==0&&disabled.statistics().resets==0,
      "default-off hint did work");
}

void DormantEviction(DormantCache::Limits limits,uint64_t epoch,bool binding,bool wrap) {
  Map map;Hints hints(map,true);DormantCache cache(limits,epoch);
  auto first=map.try_emplace(Key()).first;
  cache.Access(first->second.residency,false);cache.Created(first->second.residency,64);
  hints.Remember(3,first);
  unsigned callbacks=0;
  auto retire=[&](const Value& value) {
    Check(value.owner!=nullptr,"owner destroyed before invalidation");
    hints.Reset(Hints::ResetReason::BeforeErase);++callbacks;
  };
  if(wrap) {
    // Simulate an older retained node at the epoch wrap; the current node
    // must survive, retaining normal ownership and bookkeeping behavior.
    auto old=map.try_emplace(Key(2)).first;
    cache.Access(old->second.residency,false);cache.Created(old->second.residency,64);
    old->second.residency.last_used_epoch=epoch-1;
    hints.Remember(4,old);
    cache.EndFrame(map,retire);
    Check(map.contains(Key())&&!map.contains(Key(2)),"wrap erased wrong owner");
    Check(cache.statistics().wrap_evictions==1,"wrap path uncovered");
  } else {
    cache.EndFrame(map,retire); // used node survives the current frame
    Check(callbacks==0,"current-frame owner evicted");
    if(binding) {
      Check(cache.EvictUnusedForBindings(map,retire,[](const Value& v){return v.binding;}),"binding eviction absent");
      Check(cache.statistics().binding_evictions==1,"binding path uncovered");
    } else cache.EndFrame(map,retire);
    Check(!map.contains(Key()),"dormant owner not evicted");
  }
  Check(callbacks==1&&hints.statistics().erase_resets==1&&hints.statistics().invalidated_slots>0,
      "real dormant eviction missed invalidation");
  auto missing=wrap?Key(2):Key();
  Check(hints.Find(wrap?4:3,View(missing))==map.end(),"erased node reused through stale hint");
  auto replacement=map.try_emplace(missing).first;
  hints.Remember(wrap?4:3,replacement);
  Check(hints.Find(wrap?4:3,View(missing))==replacement,"replacement node lookup failed");
  if(!binding&&!wrap)Check(cache.statistics().age_evictions+cache.statistics().budget_evictions==1,
      "age/budget path uncovered");
}

// Force allocator reuse of the exact map-node address. A stale iterator can
// silently appear valid after reuse even under ASan, so test keys and owners.
struct ReusePool {
  void* spare=nullptr;size_t bytes=0;size_t reused=0;
  ~ReusePool(){::operator delete(spare);}
};
template<class T> struct ReuseAllocator {
  using value_type=T;ReusePool* pool;
  explicit ReuseAllocator(ReusePool& p):pool(&p){}
  template<class U>ReuseAllocator(const ReuseAllocator<U>& other):pool(other.pool){}
  T* allocate(size_t count) {
    if(pool->spare&&pool->bytes==sizeof(T)*count) {
      auto* result=pool->spare;pool->spare=nullptr;++pool->reused;return static_cast<T*>(result);
    }
    return static_cast<T*>(::operator new(sizeof(T)*count));
  }
  void deallocate(T* p,size_t count) {
    ::operator delete(pool->spare);pool->spare=p;pool->bytes=sizeof(T)*count;
  }
  template<class U>bool operator==(const ReuseAllocator<U>& other)const{return pool==other.pool;}
};
void ReusedNodeAndFailedCreation() {
  ReusePool pool;
  using ReuseMap=std::map<CpuImageIdentity,Value,CpuImageLess,ReuseAllocator<std::pair<const CpuImageIdentity,Value>>>;
  ReuseMap map{CpuImageLess{},decltype(map)::allocator_type(pool)};
  using ReuseHints=BorrowedSlotLookup<ReuseMap,CpuImageEqual>;
  ReuseHints hints(map,true);
  for(uint64_t version=1;version<=256;++version) {
    auto key=Key(version);auto first=map.try_emplace(key).first;hints.Remember(7,first);
    Check(hints.Find(7,View(key))==first,"reuse warmup failed");
    hints.Reset(ReuseHints::ResetReason::BeforeErase);map.erase(first);
    Check(hints.Find(7,View(key))==map.end(),"erased node hint survived");
    // Failed resource creation leaves the owning map node valid, as it does
    // in SceneExecutor; the hint neither fabricates ownership nor hides null.
    auto next=map.try_emplace(key).first;next->second.owner.reset();hints.Remember(7,next);
    Check(!hints.Find(7,View(key))->second.owner,"null owner incorrectly treated as initialized");
    next->second.owner=std::make_unique<int>(29);
    Check(*hints.Find(7,View(key))->second.owner==29,"resource recovery changed owning node");
    hints.Reset(ReuseHints::ResetReason::BeforeErase);map.clear();
  }
  Check(pool.reused>=255,"allocator did not reuse node storage");
}
} // namespace
int main() {
  try {
    ExactKeysAndSlots();
    DormantEviction({1024,16,0},1,false,false); // age
    DormantEviction({32,16,8},1,false,false);   // byte budget
    DormantEviction({1024,0,8},1,false,false);  // disabled entry budget
    DormantEviction({1024,16,8},1,true,false);  // binding pressure
    DormantEviction({1024,16,8},UINT64_MAX,false,true); // epoch wrap
    ReusedNodeAndFailedCreation();
    std::cout<<"PASS cpu image slot lookup: "<<checks<<" checks\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
