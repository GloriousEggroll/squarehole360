#include <rex/system/physical_snapshot.h>
#include "renderer/native/immutable_snapshots.h"
#include <iostream>
#include <type_traits>
#include <stdexcept>
using Tracker=rex::memory::PhysicalSnapshotTracker;
using Snapshot=rex::memory::CertifiedPhysicalSnapshotPtr;
using Access=Tracker::Access;
static_assert(!std::is_copy_constructible_v<Tracker>&&!std::is_move_constructible_v<Tracker>);
static_assert(!std::is_copy_constructible_v<rex::memory::CertifiedPhysicalSnapshot>&&
              !std::is_move_constructible_v<rex::memory::CertifiedPhysicalSnapshot>);
namespace {
void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
bool test_batch=false;
struct Fake {
  static constexpr uint32_t address=Tracker::kFirst+4096;
  std::vector<uint8_t> bytes=std::vector<uint8_t>(16*4096,0x37);
  std::array<uintptr_t,4> aliases{};
  std::array<std::array<Access,16>,4> access{};
  unsigned calls=0,fail=0,fail_until=0,queries=0,query_fail=0;
  bool partial_failure=false,query_throw=false;
  std::unique_ptr<Tracker> tracker;
  explicit Fake(size_t metadata_limit=rex::memory::PhysicalSnapshotMetadata::kLimit){
    aliases={reinterpret_cast<uintptr_t>(bytes.data())-Tracker::kFirst,
             0x1000000000ull,0x2000000000ull,0x3000000000ull};
    for(auto& a:access)a.fill(Access::ReadWrite);
    Tracker::Host host{this,[](void* context,uintptr_t p,size_t& size,Access& out){
      auto& f=*static_cast<Fake*>(context);++f.queries;
      if(f.queries==f.query_fail)return false;
      auto [a,page]=f.Locate(p);out=f.access[a][page];size=4096;
      while(page+size/4096<16&&f.access[a][page+size/4096]==out)size+=4096;
      return true;
    },[](void* context,uintptr_t p,size_t size,Access value) noexcept {
      auto& f=*static_cast<Fake*>(context);++f.calls;
      auto [a,page]=f.Locate(p);
      if(f.calls==f.fail || (f.fail_until&&f.calls>=f.fail&&f.calls<=f.fail_until)) {
        if(f.partial_failure)f.access[a][page]=value;
        return false;
      }
      for(size_t i=0;i<size/4096;++i)f.access[a][page+i]=value;
      return true;
    }};
    host.query_pages=[](void* context,std::span<const Tracker::PageQuery> requests) {
      auto& f=*static_cast<Fake*>(context);++f.queries;
      if(f.query_throw)throw std::runtime_error("optional query failure");
      for(const auto& request:requests)for(size_t i=0;i<request.pages.size();++i) {
        auto [a,page]=f.Locate(request.address+i*4096);request.pages[i]=f.access[a][page];
        if(f.queries==f.query_fail)return false; // Deliberately partial output.
      }
      return true;
    };
    tracker=std::make_unique<Tracker>(host,aliases,metadata_limit);
    tracker->SetBatchQueriesEnabled(test_batch);
  }
  std::pair<unsigned,unsigned> Locate(uintptr_t host) noexcept {
    for(unsigned a=0;a<4;++a)if(host>=aliases[a]+Tracker::kFirst&&host<aliases[a]+Tracker::kFirst+bytes.size())
      return {a,unsigned((host-aliases[a]-Tracker::kFirst)/4096)};
    std::terminate();
  }
  uintptr_t Host(unsigned alias,uint32_t p=address){return aliases[alias]+p;}
  Snapshot Get(const Snapshot& previous={}){return tracker->Capture(address,8192,previous);}
  void Write(unsigned alias,uint8_t value,uint32_t offset=0){
    const auto p=address+offset;auto [a,page]=Locate(Host(alias,p));
    if(access[a][page]!=Access::ReadWrite)
      Check(tracker->HandleFault(Host(alias,p),true)==Tracker::Fault::Handled,"owned fault did not restore exact alias");
    bytes[p-Tracker::kFirst]=value;
  }
};
void AliasesAndLifetime(){
  Fake f;Check(!f.Get(),"Disabled tracker minted token");f.tracker->Enable();
  auto initial=f.Get();Check(f.tracker->OwnsFault(f.Host(0),true)&&!f.tracker->OwnsFault(f.Host(0),false),"exact owned write-only proof");Check(initial&&*initial->bytes()==std::vector<uint8_t>(8192,0x37),"initial bytes");
  Check(f.Get(initial)==initial&&f.tracker->stats().hits==1,"unchanged hit");
  auto previous=initial;
  for(unsigned a=0;a<4;++a)for(unsigned pass=0;pass<2;++pass){
    f.Write(a,uint8_t(10+a*2+pass),19+a);auto next=f.Get(previous);
    Check(next&&next!=previous&&next->bytes()->at(19+a)==10+a*2+pass,"alias write missed");
    Check(f.Get(next)==next,"rearm failed");previous=next;
  }
  Check(initial->bytes()->at(19)==0x37,"immutable owner was modified");
  const auto retained=f.tracker->stats().metadata_bytes;
  auto survivor=previous;previous.reset();initial.reset();Check(f.tracker->stats().metadata_bytes<retained,"token metadata leaked");
  Check(f.tracker->Disable(),"disable failed");Check(!f.Get(survivor),"disabled old token accepted");
  f.tracker->Enable();Check(f.Get(survivor)!=survivor,"lifecycle epoch reused");
  auto owner=survivor->bytes();f.tracker.reset();Check(owner->at(19)==10+1,"owner did not outlive tracker");
}
void PermissionsAndRetry(){
  Fake f;f.access[2][1]=Access::ReadOnly;f.access[3][1]=Access::NoAccess;f.tracker->Enable();
  auto first=f.Get();Check(bool(first),"preexisting readonly alias must remain certifiable");
  Check(!f.tracker->OwnsFault(f.Host(2),true)&&!f.tracker->OwnsFault(f.Host(3),true),"ownership query claimed genuine RO/NoAccess");
  Check(f.tracker->HandleFault(f.Host(2),true)==Tracker::Fault::NotOwned,"claimed genuine readonly restriction");
  Check(f.tracker->HandleFault(f.Host(0),true)==Tracker::Fault::Handled,"first paused fault");
  auto rearmed=f.Get(first);Check(rearmed&&rearmed!=first&&rearmed->bytes()==first->bytes(),"paused fault capture changed exact owner");
  const auto faults=f.tracker->stats().faults;f.Write(0,0x91);
  Check(f.tracker->stats().faults==faults+1,"retry after rearm did not fault again");
  Check(f.Get(rearmed)->bytes()->front()==0x91,"pending retry write was missed");
  Check(f.tracker->Disable(),"permission restore failed");
  Check(f.access[2][1]==Access::ReadOnly&&f.access[3][1]==Access::NoAccess,"disable relaxed existing protection");
  for(unsigned a:{0u,1u})Check(f.access[a][1]==Access::ReadWrite,"own restriction not restored");
}
void MutationAndKernel(){
  Fake f;f.tracker->Enable();auto first=f.Get();
  Check(f.tracker->BeforeHostMutation(f.Host(1),3*4096),"legacy expanded unprotect rejected");
  f.access[1][1]=Access::ReadWrite;f.access[1][2]=Access::ReadWrite;
  auto second=f.Get(first);Check(second!=first&&second->bytes()==first->bytes(),"expanded unprotect did not revoke token");
  Check(f.tracker->BeginExternalWrite(f.Host(0)+17,8190),"kernel guard");
  Check(f.tracker->BeginExternalWrite(f.Host(1)+4090,16),"overlap guard");
  Check(!f.Get(second),"certified in-flight kernel destination");
  f.bytes[4096+33]=0x21;f.tracker->EndExternalWrite(f.Host(0)+17,8190);
  Check(!f.Get(second),"certified overlapping remaining writer");
  f.tracker->EndExternalWrite(f.Host(1)+4090,16);
  Check(f.Get(second)->bytes()->at(33)==0x21,"partial/error kernel write missed");
  auto current=f.Get();f.tracker->InvalidateHost(f.Host(3),8192);
  Check(f.Get(current)!=current,"metadata-only release did not revoke");
}
void Failures(){
  for(unsigned alias=1;alias<=4;++alias){
    Fake f;f.tracker->Enable();f.fail=alias;
    Check(!f.Get(),"protection failure minted certificate");f.fail=0;
    Check(f.Get()!=nullptr,"protection failure permanently poisoned fallback");
    Check(f.tracker->Disable(),"repaired disable");
  }
  Fake f;f.tracker->Enable();f.fail=2;f.fail_until=8;
  Check(!f.Get(),"failed arm/rollback minted certificate");f.fail=f.fail_until=0;
  Check(f.tracker->OwnsFault(f.Host(0),true),"failed restore lost early repair authority");
  Check(f.tracker->HandleFault(f.Host(0),true)==Tracker::Fault::Handled,"rollback failure lost repair ownership");
  Check(!f.tracker->OwnsFault(f.Host(0),true),"restored alias retained stale repair authority");
  auto fresh=f.Get();Check(bool(fresh),"repair could not recapture");
  f.query_fail=f.queries+1;f.tracker->InvalidateHost(f.Host(0),8192);
  Check(!f.Get(fresh),"failed host permission query accepted token");
  f.query_fail=0;auto other=f.Get();
  Check(!f.tracker->Capture(0x1000,8192,other),"excluded extra alias accepted");
  Check(!f.tracker->Capture(Tracker::kEnd-4096,8192,other),"excluded tail accepted");
}
void MetadataBound(){
  Fake f(512);f.tracker->Enable();std::vector<Snapshot> held;
  for(unsigned i=0;i<20;++i){auto next=f.Get();if(next)held.push_back(next);}
  Check(!held.empty()&&held.size()<20,"metadata budget did not bound admission");
  Check(f.tracker->stats().metadata_bytes<=512&&f.tracker->stats().peak_metadata_bytes<=512,"metadata accounting exceeded bound");
  held.clear();Check(f.tracker->stats().metadata_bytes==0&&bool(f.Get()),"metadata did not release for readmission");
  auto empty=std::make_shared<rex::memory::CertifiedPhysicalSnapshot>();
  Check(bool(f.Get(empty)),"default unassigned previous token did not degrade to cold capture");
}
void ExactReuse(){
  Fake f;f.tracker->Enable();auto current=f.Get();
  const auto calls=f.calls,queries=f.queries;const auto before=f.tracker->stats();
  Check(f.tracker->TryReuse(Fake::address,8192,current)==current,"exact token reuse failed");
  Check(f.calls==calls&&f.queries==queries,"valid reuse touched host permission API");
  Check(f.tracker->stats().captures==before.captures&&f.tracker->stats().hits==before.hits+1,"reuse counters duplicate capture/hit");
  Check(!f.tracker->TryReuse(Fake::address+1,8192,current),"reuse changed exact address");
  Check(!f.tracker->TryReuse(Fake::address,4096,current),"reuse changed exact length");
  Fake different;different.tracker->Enable();Check(!different.tracker->TryReuse(Fake::address,8192,current),"reuse accepted another Memory identity");
  Check(f.tracker->BeginExternalWrite(f.Host(0),8192),"reuse writer fixture");
  Check(!f.tracker->TryReuse(Fake::address,8192,current),"reuse accepted in-flight OS writer");
  f.tracker->EndExternalWrite(f.Host(0),8192);
  Check(!f.tracker->TryReuse(Fake::address,8192,current),"reuse survived writer completion");
  current=f.Get();f.tracker->InvalidateHost(f.Host(2),8192);
  Check(!f.tracker->TryReuse(Fake::address,8192,current),"reuse survived canonical/protection mutation");
  current=f.Get();f.tracker->Disable();Check(!f.tracker->TryReuse(Fake::address,8192,current),"reuse accepted disabled tracker");
  f.tracker->Enable();Check(!f.tracker->TryReuse(Fake::address,8192,current),"reuse survived lifecycle epoch");
}
void BatchPreflight(){
  if(!test_batch)return;
  // Four aliases, mixed permissions on adjacent pages. Raw NoAccess rejects;
  // virtual NoAccess and genuine ReadOnly must never become repair-owned.
  for(unsigned pattern=0;pattern<81;++pattern) {
    Fake f;auto n=pattern;
    for(unsigned a=0;a<4;++a){f.access[a][1]=Access(n%3);n/=3;}
    const auto original=f.access;f.tracker->Enable();auto snapshot=f.Get();
    Check(bool(snapshot)==(original[0][1]!=Access::NoAccess),"mixed batch admissibility");
    if(!snapshot)Check(f.calls==0,"failed preflight armed a page");
    Check(f.tracker->Disable(),"mixed batch disable");
    Check(f.access==original,"batch relaxed a genuine protection");
  }
  Fake f;f.tracker->Enable();auto old=f.Get();const auto protection=f.access;
  const auto calls=f.calls;
  f.query_fail=f.queries+1;
  Check(!f.Get(),"partial batch proof accepted");
  Check(f.calls==calls&&f.access==protection,"failed batch changed older restrictions");
  Check(f.tracker->TryReuse(Fake::address,8192,old)==old,"failed preflight invalidated unchanged old certificate");
  f.query_fail=0;f.query_throw=true;bool caught=false;
  try{f.Get();}catch(const std::runtime_error&){caught=true;}
  Check(caught&&f.calls==calls&&f.tracker->TryReuse(Fake::address,8192,old)==old,"throwing preflight changed old authority");
  f.query_throw=false;
  Check(f.tracker->BeforeHostMutation(f.Host(3),4096),"unsupported mutation preparation");
  f.access[3][1]=Access::Unsupported;const auto before=f.calls;
  Check(!f.Get()&&f.calls==before,"unsupported batch armed before complete validation");
  f.access[3][1]=Access::ReadWrite;
  auto fresh=f.Get();auto counters=f.tracker->stats();
  Check(counters.batch_queries&&counters.batch_query_failures==3&&counters.query_calls==0,"batch evidence counters");
  Check(counters.cold_ns==0&&counters.query_ns==0&&counters.copy_ns==0,"disabled diagnostics touched phase clocks");
  f.tracker->SetDiagnosticsEnabled(true);fresh=f.Get();counters=f.tracker->stats();
  Check(counters.cold_ns&&counters.query_ns&&counters.copy_ns,"enabled cold diagnostics missing");
  Check(f.Get(fresh)==fresh,"diagnostic hit identity");
  auto hit=f.tracker->stats();Check(hit.cold_ns==counters.cold_ns&&hit.query_ns==counters.query_ns&&hit.copy_ns==counters.copy_ns,
      "exact hit ran diagnostic clocks");
  // A failed arm and failed rollback still retain repair ownership, never a
  // cached claim that the alias is currently RO. The next batch queries anew.
  Fake repair;repair.tracker->Enable();repair.fail=2;repair.fail_until=8;repair.partial_failure=true;
  Check(!repair.Get(),"failed batched arm/repair accepted");
  repair.fail=repair.fail_until=0;
  auto repaired=repair.Get();Check(bool(repaired),"fresh batch failed after partial arm/rollback");
  repair.Write(0,0xAC);Check(repair.Get(repaired)->bytes()->front()==0xAC,"repair-only ownership was reused as RO proof");
}
void Consumer(){
  Fake f;f.tracker->Enable();gow3::renderer::ImmutableSnapshots snapshots(32768);
  snapshots.ConfigureCertifiedProvider([&](uint32_t a,size_t n,const std::shared_ptr<const void>& previous){
    auto old=std::static_pointer_cast<const rex::memory::CertifiedPhysicalSnapshot>(previous);
    auto result=f.tracker->Capture(a,uint32_t(n),old);
    return result?gow3::renderer::ImmutableSnapshots::Certified{result->bytes(),result}:
                  gow3::renderer::ImmutableSnapshots::Certified{};
  });
  auto data=std::span<const uint8_t>(f.bytes).subspan(4096,8192);
  auto old=snapshots.Get(Fake::address,data);Check(snapshots.Get(Fake::address,data)==old,"consumer lost certified identity");
  f.Write(3,0x92,3);auto changed=snapshots.Get(Fake::address,data);
  Check(changed!=old&&changed->at(3)==0x92&&old->at(3)==0x37,"consumer missed mutation");
  f.tracker->Disable();f.bytes[4096+3]=0x17;auto fallback=snapshots.Get(Fake::address,data);
  Check(fallback->at(3)==0x17&&snapshots.certificate_stats().fallback==1,"unavailable must use exact fallback");
  snapshots.Clear();Check(changed->at(3)==0x92,"eviction changed retained owner");
}
// Test the threshold through the same owner-bound all-alias tracker as Consumer.
// A policy skip must neither call that provider nor trust an address/epoch hint.
void MinimumConsumer(){
  using Cache=gow3::renderer::ImmutableSnapshots;
  Fake f;f.tracker->Enable();Cache snapshots(128*1024);unsigned provider_calls=0;
  auto provider=[&](uint32_t a,size_t n,const std::shared_ptr<const void>& previous){
    ++provider_calls;
    auto old=std::static_pointer_cast<const rex::memory::CertifiedPhysicalSnapshot>(previous);
    auto result=f.tracker->Capture(a,uint32_t(n),old);
    return result?Cache::Certified{result->bytes(),result}:Cache::Certified{};
  };
  snapshots.ConfigureCertifiedProvider(provider,8192);
  auto data=std::span<const uint8_t>(f.bytes).subspan(4096,8193);
  auto small=snapshots.Get(Fake::address,data.first(8191));
  Check(!provider_calls&&!f.calls&&!f.queries,"below-minimum capture touched certificate provider or protection");
  Check(snapshots.Get(Fake::address,data.first(8191))==small,"below-minimum exact hit identity");
  // No page was protected by the skipped path. Change a middle byte directly;
  // the exact comparison must detect it without any write notification.
  f.bytes[4096+4077]=0x98;
  auto small_changed=snapshots.Get(Fake::address,data.first(8191));
  Check(small_changed!=small&&small_changed->at(4077)==0x98&&small->at(4077)==0x37,
      "below-minimum unnotified write bypassed exact comparison");
  Check(!provider_calls&&!f.calls&&!f.queries,"below-minimum hit armed SDK pages");
  auto boundary=snapshots.Get(Fake::address,data.first(8192));
  Check(provider_calls==1&&f.calls&&f.queries,"exact minimum was not eligible");
  Check(snapshots.Get(Fake::address,data.first(8192))==boundary&&provider_calls==2,
      "boundary certificate lost exact owner hit");
  for(unsigned alias=0;alias<4;++alias){
    f.Write(alias,uint8_t(0x41+alias),4191+alias);
    auto next=snapshots.Get(Fake::address,data.first(8192));
    Check(next!=boundary&&next->at(4191+alias)==uint8_t(0x41+alias),
        "minimum gate changed all-alias revocation");boundary=next;
  }
  auto above=snapshots.Get(Fake::address,data);
  Check(above->size()==8193&&snapshots.Get(Fake::address,data)==above,"above-minimum certificate hit");
  const auto calls_before_skip=provider_calls;
  f.Write(2,0xA7,4077);
  auto small_again=snapshots.Get(Fake::address,data.first(8191));
  Check(provider_calls==calls_before_skip&&small_again->at(4077)==0xA7&&small_changed->at(4077)==0x98,
      "small snapshot overlapping certified pages reused stale content or invoked provider");
  const auto policy=snapshots.certificate_stats();
  Check(policy.minimum_size_bypasses==4&&policy.minimum_size_bypassed_bytes==4*8191,
      "minimum gate bypass counts disagree with exact calls");
  Check(policy.attempts==provider_calls&&policy.accepted==provider_calls&&!policy.fallback,
      "minimum gate mislabeled provider attempts");
  Check(f.tracker->Disable(),"minimum consumer disable failed");
  f.bytes[4096+3077]=0xE5;
  auto fallback=snapshots.Get(Fake::address,data.first(8192));
  Check(fallback->at(3077)==0xE5&&boundary->at(3077)==0x37&&snapshots.certificate_stats().fallback==1,
      "unavailable boundary certificate missed exact fallback");
  // Replacing policy clears both owner entries and opaque provider tokens.
  // Zero restores the existing full-certification eligibility policy.
  f.tracker->Enable();snapshots.ConfigureCertifiedProvider(provider);
  const auto before_default=provider_calls;
  auto default_small=snapshots.Get(Fake::address,data.first(4096));
  Check(provider_calls==before_default+1&&default_small->size()==4096,"default-zero changed full-certification behavior");
  snapshots.ConfigureCertifiedProvider({});
  f.tracker->Disable();f.bytes[4096+3077]=0x1D;
  auto disabled=snapshots.Get(Fake::address,data.first(4096));
  Check(provider_calls==before_default+1&&disabled->at(3077)==0x1D&&default_small->at(3077)==0xE5,
      "disabled provider changed exact path or retained stale owner");
}

// This fixture drives the all-alias tracker, not a frame/dirty-epoch model.
// Expected warm counts are independent fixed values:32,64,128,...4096.
void StableConsumer(){
  using Cache=gow3::renderer::ImmutableSnapshots;
  Fake f;f.tracker->Enable();Cache snapshots(128*1024);
  unsigned captures=0,validations=0;bool throw_capture=false,throw_validate=false,mismatched_owner=false;
  auto capture=[&](uint32_t a,size_t n,const std::shared_ptr<const void>& previous){
    ++captures;if(throw_capture)throw std::runtime_error("optional arm failure");
    auto old=std::static_pointer_cast<const rex::memory::CertifiedPhysicalSnapshot>(previous);
    auto result=f.tracker->Capture(a,uint32_t(n),old);
    return result?Cache::Certified{result->bytes(),result}:Cache::Certified{};
  };
  auto validate=[&](uint32_t a,size_t n,const std::shared_ptr<const void>& previous){
    ++validations;if(throw_validate)throw std::runtime_error("optional validation failure");
    auto old=std::static_pointer_cast<const rex::memory::CertifiedPhysicalSnapshot>(previous);
    auto result=f.tracker->TryReuse(a,uint32_t(n),old);
    if(!result)return Cache::Certified{};
    if(mismatched_owner)return Cache::Certified{std::make_shared<const Cache::Bytes>(*result->bytes()),result};
    return Cache::Certified{result->bytes(),result};
  };
  snapshots.ConfigureStableCertifiedProvider(capture,validate,8192);
  auto data=std::span<const uint8_t>(f.bytes).subspan(4096,8192);
  auto warm=[&](unsigned exact_count){
    const auto before=captures;auto owner=snapshots.Get(Fake::address,data);
    for(unsigned i=1;i<exact_count;++i)Check(snapshots.Get(Fake::address,data)==owner,"warming changed exact owner");
    Check(captures==before,"warming armed before exact streak was complete");
  };
  auto initial=snapshots.Get(Fake::address,data);
  Check(!f.calls&&!f.queries&&!captures&&!validations,"new entry armed before exact equality history");
  warm(32);Check(snapshots.stable_certificate_stats().exact_hits==32,"initial exact streak length");
  auto certified=snapshots.Get(Fake::address,data);Check(captures==1&&f.calls&&f.queries,"stable initial arm missing");
  Check(*certified==*initial,"fresh capture changed equal bytes");
  const auto calls=f.calls,queries=f.queries;
  Check(snapshots.Get(Fake::address,data)==certified&&validations==1,"stable reuse did not preserve certified owner");
  Check(f.calls==calls&&f.queries==queries,"successful validation re-armed or queried permissions");
  unsigned required=64;
  for(unsigned pass=0;pass<9;++pass){
    const auto previous=certified;f.Write(pass%4,uint8_t(0x81+pass),4077);
    const auto before_calls=f.calls,before_queries=f.queries,before_captures=captures;
    certified=snapshots.Get(Fake::address,data);
    Check(certified!=previous&&certified->at(4077)==0x81+pass&&previous->at(4077)!=(0x81+pass),"stable alias invalidation missed interior byte");
    Check(captures==before_captures&&f.calls==before_calls&&f.queries==before_queries,
          "invalid validation immediately recaptured or re-armed");
    warm(required);
    certified=snapshots.Get(Fake::address,data);Check(captures==before_captures+1,"cooldown exact streak did not permit fresh arm");
    Check(snapshots.Get(Fake::address,data)==certified,"rearmed exact owner validation failed");
    required=std::min(4096u,required*2);
  }
  Check(initial->at(4077)==0x37,"retained ordinary initial owner changed");
  // Invalid external writer may still be in progress. Validation must not
  // inspect permissions/re-arm; ordinary exact-copy behavior remains unchanged.
  auto old=certified;const auto before=captures;
  Check(f.tracker->BeginExternalWrite(f.Host(2)+11,8190),"stable OS writer admission");
  f.bytes[4096+4011]=0xA9;
  const auto writer_calls=f.calls,writer_queries=f.queries;
  auto writer=snapshots.Get(Fake::address,data);
  Check(writer->at(4011)==0xA9&&old->at(4011)!=0xA9&&captures==before&&f.calls==writer_calls&&f.queries==writer_queries,
        "stable active OS writer triggered recapture or stale owner");
  f.tracker->EndExternalWrite(f.Host(2)+11,8190);
  warm(4096);certified=snapshots.Get(Fake::address,data);Check(captures==before+1,"OS completion did not permit delayed arm");
  mismatched_owner=true;const auto mismatch_calls=f.calls,mismatch_queries=f.queries;
  Check(snapshots.Get(Fake::address,data)==certified&&snapshots.stable_certificate_stats().owner_refusals==1,
        "mismatched validation owner accepted as a byte certificate");
  Check(f.calls==mismatch_calls&&f.queries==mismatch_queries&&captures==before+1,"owner refusal re-armed");
  mismatched_owner=false;
  // The invalidation call itself performed one exact equal read.
  warm(4095);certified=snapshots.Get(Fake::address,data);
  throw_validate=true;const auto captures_before_throw=captures;
  Check(snapshots.Get(Fake::address,data)==certified&&captures==captures_before_throw,"throwing validator lost exact fallback or rearmed");
  throw_validate=false;warm(4095);throw_capture=true;
  Check(snapshots.Get(Fake::address,data)==certified&&snapshots.stable_certificate_stats().arm_refusals==1,
        "throwing arm did not preserve ordinary owner");
  throw_capture=false;
  // A content change during cooldown resets the history but carries the capped
  // requirement across the newly allocated owner;31 exact hits cannot rearm.
  f.bytes[4096+4007]=0xE3;auto changed=snapshots.Get(Fake::address,data);
  const auto after_failure=captures;warm(31);
  Check(captures==after_failure&&changed->at(4007)==0xE3,"owner replacement reset cooldown to initial32");
  auto small_data=data.first(4096);auto small=snapshots.Get(Fake::address,small_data);
  for(unsigned i=0;i<40;++i)Check(snapshots.Get(Fake::address,small_data)==small,"below-minimum ordinary identity");
  f.bytes[4096+2077]=0xC3;
  Check(snapshots.Get(Fake::address,small_data)->at(2077)==0xC3&&captures==after_failure,
        "below-minimum stable path missed unnotified write or invoked provider");
  const auto stats=snapshots.stable_certificate_stats();
  Check(stats.safe_hits>=10&&stats.invalidations>=12&&stats.exact_changes>=11&&stats.provider_errors==2&&stats.cooldown_bypasses,
        "stable proof/cooldown/error counters missing");
  snapshots.ConfigureCertifiedProvider(capture);const auto legacy_before=captures;
  auto legacy=snapshots.Get(Fake::address,small_data);
  Check(captures==legacy_before+1&&!snapshots.stable_certificate_stats().enabled,"legacy provider retained stable policy");
  Check(snapshots.Get(Fake::address,small_data)==legacy&&captures==legacy_before+2,"legacy immediate certificate behavior changed");
  snapshots.ConfigureCertifiedProvider({});Check(f.tracker->Disable(),"stable tracker disable");
  f.bytes[4096+2077]=0x17;
  Check(snapshots.Get(Fake::address,small_data)->at(2077)==0x17&&small->at(2077)==0x37,
        "disabled stable provider retained stale token or changed immutable lifetime");
}
void StableBounds(){
  using Cache=gow3::renderer::ImmutableSnapshots;
  unsigned captures=0;auto refuse=[&](uint32_t,size_t,const std::shared_ptr<const void>&){++captures;return Cache::Certified{};};
  Cache unretained(7);unretained.ConfigureStableCertifiedProvider(refuse,refuse);
  std::array<uint8_t,8> bytes{};
  for(unsigned i=0;i<80;++i)Check(*unretained.Get(0x1000,bytes)==Cache::Bytes(8),"unretained bytes");
  Check(!captures&&!unretained.retained_bytes()&&!unretained.cache_stats().entries,"uncached snapshot accumulated certificate history");
  for(bool lru:{false,true}){
    bytes.fill(0); // Each independent policy starts with the asserted retained seed.
    Cache bounded(128*1024);bounded.ConfigureMetadataLru(lru);bounded.ConfigureStableCertifiedProvider(refuse,refuse);
    auto retained=bounded.Get(0x1000,bytes);
    for(uint32_t i=0;i<5000;++i){bytes[0]=uint8_t(i);bounded.Get(0x2000+i,bytes);}
    Check(bounded.retained_bytes()<=128*1024&&bounded.cache_stats().entries<=4096&&retained->front()==0,
          "stable policy exceeded payload/metadata bounds or modified retained owner");
    bounded.Clear();Check(!bounded.cache_stats().entries&&!bounded.retained_bytes(),"stable clear leaked cache owner");
  }
}

}
int main(){try{for(bool batch:{false,true}){test_batch=batch;AliasesAndLifetime();PermissionsAndRetry();MutationAndKernel();Failures();MetadataBound();ExactReuse();Consumer();MinimumConsumer();StableConsumer();StableBounds();BatchPreflight();}
  std::cout<<"PASS: all-alias invalidation, exact owner reuse/lifetime, genuine protections, paused retry, expanded mutation, overlapping kernel writers, partial/failed arm repair, lifecycle, consumer fallback, minimum-size exact policy, stable exact streak/cooldown, reuse-only refusal/errors and bounded immutable owners\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
