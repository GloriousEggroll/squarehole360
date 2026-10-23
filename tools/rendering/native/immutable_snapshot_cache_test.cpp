#include "renderer/native/immutable_snapshots.h"
#include <iostream>
#include <stdexcept>
using Cache=gow3::renderer::ImmutableSnapshots;
namespace {
void Check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
uint64_t observations=0;
Cache::MetadataEviction observed;
void Observe(const Cache::MetadataEviction& event)noexcept{++observations;observed=event;}
void Metadata(bool lru,bool empty){
  Cache cache(16384);cache.ConfigureMetadataLru(lru);
  std::vector<uint8_t> bytes(empty?0:1,0x31);
  std::shared_ptr<const Cache::Bytes> first,second;
  for(uint32_t i=0;i<4096;++i){
    auto owner=cache.Get(i*4096,bytes);if(!i)first=owner;if(i==1)second=owner;
  }
  Check(cache.cache_stats().entries==4096,"metadata bound setup");
  Check(cache.Get(0,bytes)==first,"hot owner refresh changed identity");
  Check(observations==0,"disabled or metadata-hit observer was invoked");
  cache.ConfigureMetadataEvictionObserver(Observe);
  Check(cache.Get(0,bytes)==first&&observations==0,"metadata hit invoked observer");
  cache.Get(4096*4096,bytes);
  Check(observations==1&&observed.address==4096*4096&&observed.requested_bytes==bytes.size()&&
        observed.entries==4096&&observed.retained_bytes==(empty?0u:4096u)&&observed.lru==lru,
        "metadata observer did not report exact pre-eviction state");
  cache.ConfigureMetadataEvictionObserver(nullptr);observations=0;
  auto s=cache.cache_stats();
  Check(s.entries==(lru?4096u:1u)&&s.peak_entries==4096,"metadata policy exceeded bound or cleared unexpectedly");
  Check(s.metadata_lru_evictions==(lru?1u:0u)&&s.metadata_clears==(lru?0u:1u),"metadata evidence counters");
  Check(s.metadata_clear_entries==(lru?0u:4096u)&&!s.budget_evictions,"metadata eviction mislabeled as byte budget");
  auto kept=cache.Get(0,bytes);Check((kept==first)==lru,"metadata LRU did not retain the refreshed owner");
  auto replacement=cache.Get(4096,bytes);Check(replacement!=second&&*replacement==*second,"eviction reused a stale owner or changed immutable bytes");
  Check(*first==bytes&&*second==bytes,"eviction destroyed an in-flight immutable snapshot");
  Check(cache.retained_bytes()<=(empty?0u:4096u),"metadata eviction byte accounting");
}
void Bytes(){
  Cache cache(8);cache.ConfigureMetadataLru(true);std::vector<uint8_t> data(2,0x55);
  auto retained=cache.Get(0,data);for(uint32_t i=1;i<4;++i)cache.Get(i,data);
  Check(cache.Get(0,data)==retained,"byte LRU refresh identity");
  std::vector<uint8_t> larger(4,0x66);cache.Get(9,larger);
  auto s=cache.cache_stats();Check(s.budget_evictions==2&&!s.metadata_lru_evictions&&!s.metadata_clears,"byte pressure counters/order");
  Check(cache.Get(0,data)==retained&&retained->at(0)==0x55,"byte eviction lost refreshed retained owner");
  data[0]=0x77;auto changed=cache.Get(0,data);
  Check(changed!=retained&&changed->at(0)==0x77&&retained->at(0)==0x55,"mutation did not retain exact immutable history");
  cache.Clear();Check(cache.cache_stats().entries==0&&cache.retained_bytes()==0&&retained->at(0)==0x55,"explicit teardown leaked or changed owner");
}
}
int main(){try{Metadata(false,false);Metadata(true,false);Metadata(false,true);Metadata(true,true);Bytes();
  std::cout<<"PASS:4096 metadata pressure preserves optional LRU, legacy clear, zero-length bounds, exact byte pressure, refresh order and retained immutable ownership\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
