#include <rex/system/xmemory.h>
#include <rex/filesystem.h>
#include <rex/stream.h>
#include <iostream>
#include <stdexcept>
#include <atomic>
using namespace rex::memory;
namespace {
void Check(bool v,const char* text){if(!v)throw std::runtime_error(text);}
constexpr uint32_t address=0x02000000,guest=0xA2000000,length=0x10000;
constexpr uint32_t rw=kMemoryProtectRead|kMemoryProtectWrite;
constexpr uint32_t commit=kMemoryAllocationReserve|kMemoryAllocationCommit;
// Reuse-only API proof: successful and refused validation must leave every
// cold capture/protection/query clock and counter unchanged. Hit counters alone
// may advance on an exact valid token.
void NoColdChange(const PhysicalSnapshotTracker::Stats& a,const PhysicalSnapshotTracker::Stats& b){
  Check(a.captures==b.captures&&a.capture_bytes==b.capture_bytes&&a.protect_calls==b.protect_calls&&
        a.protect_failures==b.protect_failures&&a.query_calls==b.query_calls&&a.batch_queries==b.batch_queries&&
        a.batch_query_failures==b.batch_query_failures&&a.queried_pages==b.queried_pages&&
        a.armed_pages==b.armed_pages&&a.cold_ns==b.cold_ns&&a.metadata_ns==b.metadata_ns&&
        a.query_ns==b.query_ns&&a.capture_protect_ns==b.capture_protect_ns&&a.copy_ns==b.copy_ns,
        "reuse-only validation captured, queried, armed or ran cold clocks");
}

void Run(bool early,bool batch){
  CertifiedPhysicalSnapshotPtr surviving;
  {
    Memory memory;
    Check(!memory.SetCertifiedPhysicalSnapshotsEnabled(true),"enabled uninitialized Memory");
    Check(memory.Initialize(),"Memory Initialize");
    memory.SetCertifiedPhysicalFaultFastPath(early);
    memory.SetCertifiedPhysicalSnapshotBatchQueries(batch);
    memory.SetCertifiedPhysicalSnapshotDiagnostics(batch);
    auto* heap=memory.LookupHeap(guest);
    Check(heap&&heap->AllocFixed(guest,length,length,commit,rw),"physical allocation");
    auto* raw=memory.TranslatePhysical(address);std::memset(raw,0x44,length);
    {
      // This models an OS operation admitted before feature configuration. No
      // snapshot may be enabled while this previously untracked writer lives.
      auto before_enable=memory.BeginExternalHostWrite(raw,8192);
      Check(bool(before_enable),"pre-install external write admission");
      Check(!memory.SetCertifiedPhysicalSnapshotsEnabled(true),"enabled across pre-install OS writer");
      std::memset(raw,0x45,8192);
    }
    Check(memory.SetCertifiedPhysicalSnapshotsEnabled(true),"enable after OS guard retirement");
    Check(memory.GetPhysicalHeap()->IsRangeCommittedReadable(address+31,address+8192),"bounded readable metadata query");
    Check(!memory.GetPhysicalHeap()->IsRangeCommittedReadable(address+length,address+length+4095),"bounded query accepted neighboring unallocated region");
    auto first=memory.TryGetCertifiedPhysicalSnapshot(address,8192);
    Check(first&&first->bytes()->front()==0x45,"first actual Memory certificate");
    const auto before_hit=memory.certified_physical_snapshot_stats();
    Check(memory.TryGetCertifiedPhysicalSnapshot(address,8192,first)==first,"actual unchanged hit");
    const auto after_hit=memory.certified_physical_snapshot_stats();
    auto before_validate=after_hit;
    Check(memory.ValidatePhysicalSnapshot(address,8192,first)==first,"Memory reuse-only valid token identity");
    auto after_validate=memory.certified_physical_snapshot_stats();NoColdChange(before_validate,after_validate);
    Check(after_validate.hits==before_validate.hits+1,"reuse-only valid hit accounting");
    before_validate=after_validate;
    Check(!memory.ValidatePhysicalSnapshot(address+1,8192,first)&&
          !memory.ValidatePhysicalSnapshot(address,4096,first)&&
          !memory.ValidatePhysicalSnapshot(address,8192,{}),"reuse-only accepted wrong address, length or null token");
    NoColdChange(before_validate,memory.certified_physical_snapshot_stats());

    Check(before_hit.cold_ns==after_hit.cold_ns&&before_hit.metadata_ns==after_hit.metadata_ns&&before_hit.query_ns==after_hit.query_ns,
          "actual exact hit changed cold diagnostics");
    for(uint32_t alias:{address,0xA0000000u+address,0xC0000000u+address,0xDFFFF000u+address}){
      auto* destination=alias==address?raw:memory.TranslateVirtual(alias);
      *static_cast<volatile uint8_t*>(destination)=uint8_t(alias>>24);
      const auto invalid_before=memory.certified_physical_snapshot_stats();
      Check(!memory.ValidatePhysicalSnapshot(address,8192,first),"reuse-only accepted all-alias byte mutation");
      NoColdChange(invalid_before,memory.certified_physical_snapshot_stats());

      auto next=memory.TryGetCertifiedPhysicalSnapshot(address,8192,first);
      Check(next&&next!=first&&next->bytes()->front()==uint8_t(alias>>24),"Memory alias callback missed changed byte");
      first=next;
    }
    uint32_t old=0;
    Check(heap->Protect(guest,4096,kMemoryProtectRead,&old),"normal read-only Protect");
    Check((old&kMemoryProtectWrite)!=0,"certificate changed reported old protection");

    const auto protected_before=memory.certified_physical_snapshot_stats();
    Check(!memory.ValidatePhysicalSnapshot(address,8192,first),"reuse-only accepted Protect-revoked token");
    NoColdChange(protected_before,memory.certified_physical_snapshot_stats());
    auto readonly=memory.TryGetCertifiedPhysicalSnapshot(address,8192,first);
    Check(readonly&&readonly!=first,"normal Protect did not invalidate old token");
    Check(heap->Protect(guest,4096,rw),"normal writable Protect");
    raw[0]=0x71;
    Check(memory.TryGetCertifiedPhysicalSnapshot(address,8192,readonly)->bytes()->front()==0x71,"normal protection restoration lost raw write");
    // Exact unaligned metadata boundary: the second canonical page becomes
    // inaccessible. QueryRegionInfo's whole first page must not skip into it.
    Check(heap->Protect(guest,0x10000,rw),"restore whole allocation");
    Check(memory.GetPhysicalHeap()->Protect(address+4096,4096,kMemoryProtectNoAccess),"canonical no-access boundary");
    Check(!memory.GetPhysicalHeap()->IsRangeCommittedReadable(address+31,address+8192),"bounded query skipped unaligned inaccessible edge");
    Check(!memory.TryGetCertifiedPhysicalSnapshot(address+31,8192),"unaligned capture skipped inaccessible metadata page");
    Check(memory.GetPhysicalHeap()->Protect(address+4096,4096,rw),"canonical restore boundary");
    auto before_release=memory.TryGetCertifiedPhysicalSnapshot(address,8192);
    Check(heap->Release(guest),"normal Release");
    Check(!memory.TryGetCertifiedPhysicalSnapshot(address,8192,before_release),"released canonical range certified");
    const auto released_before=memory.certified_physical_snapshot_stats();
    Check(!memory.ValidatePhysicalSnapshot(address,8192,before_release),"reuse-only accepted released allocation");
    NoColdChange(released_before,memory.certified_physical_snapshot_stats());

    Check(heap->AllocFixed(guest,length,length,commit,rw),"same-address reallocation");
    std::memset(raw,0x82,8192);

    const auto reused_before=memory.certified_physical_snapshot_stats();
    Check(!memory.ValidatePhysicalSnapshot(address,8192,before_release),"reuse-only survived same-address reallocation");
    NoColdChange(reused_before,memory.certified_physical_snapshot_stats());
    auto reused=memory.TryGetCertifiedPhysicalSnapshot(address,8192,before_release);
    Check(reused&&reused!=before_release&&reused->bytes()->front()==0x82,"reallocated address reused stale token");
    Check(before_release->bytes()->front()==0x71,"old owner changed after free/reuse");
    std::atomic<unsigned> gpu_notifications=0;
    auto callback=memory.RegisterPhysicalMemoryInvalidationCallback(
      [](void* context,uint32_t,uint32_t,bool){++*static_cast<std::atomic<unsigned>*>(context);return std::pair<uint32_t,uint32_t>{0,UINT32_MAX};},&gpu_notifications);
    memory.EnablePhysicalMemoryAccessCallbacks(address,length,true,false);
    auto neighbor=memory.TryGetCertifiedPhysicalSnapshot(address+8192,4096);
    Check(bool(neighbor),"watched neighbor capture");
    *static_cast<volatile uint8_t*>(memory.TranslateVirtual(guest))=0x91;
    Check(gpu_notifications>0,"legacy virtual GPU notification lost");
    auto neighbor_again=memory.TryGetCertifiedPhysicalSnapshot(address+8192,4096,neighbor);
    Check(neighbor_again&&neighbor_again!=neighbor,"expanded legacy unprotect left neighboring token valid");
    auto raw_snapshot=memory.TryGetCertifiedPhysicalSnapshot(address,4096);
    const auto before_raw=gpu_notifications.load();raw[1]=0x92;
    Check(memory.TryGetCertifiedPhysicalSnapshot(address,4096,raw_snapshot)->bytes()->at(1)==0x92,"raw publication missed byte invalidation");
    Check(gpu_notifications==before_raw,"raw byte fault changed GPU ownership");
    memory.UnregisterPhysicalMemoryInvalidationCallback(callback);
    auto external_old=memory.TryGetCertifiedPhysicalSnapshot(address,8192);
    {
      auto external=memory.BeginExternalHostWrite(raw+17,8192);
      Check(bool(external)&&!memory.TryGetCertifiedPhysicalSnapshot(address,8192,reused),"in-flight installed guard accepted certificate");

      const auto writer_before=memory.certified_physical_snapshot_stats();
      Check(!memory.ValidatePhysicalSnapshot(address,8192,external_old),"reuse-only accepted active external writer");
      NoColdChange(writer_before,memory.certified_physical_snapshot_stats());
      raw[19]=0xA5;
    }

    const auto completed_before=memory.certified_physical_snapshot_stats();
    Check(!memory.ValidatePhysicalSnapshot(address,8192,external_old),"reuse-only survived external writer completion");
    NoColdChange(completed_before,memory.certified_physical_snapshot_stats());
    surviving=memory.TryGetCertifiedPhysicalSnapshot(address,8192);
    Check(surviving&&surviving->bytes()->at(19)==0xA5,"guard completion did not permit fresh capture");
    // Exercise the real heap save/restore temporary-RW path, then complete
    // Memory reset; these are independent of ordinary allocation callbacks.
    std::vector<uint8_t> saved(1024*1024);
    rex::stream::ByteStream output(saved.data(),saved.size());
    Check(heap->Save(&output),"physical heap Save");
    auto after_save=memory.TryGetCertifiedPhysicalSnapshot(address,8192,surviving);
    Check(after_save&&after_save!=surviving,"Save temporary RW retained old certificate");
    raw[19]=0xE6;
    rex::stream::ByteStream input(saved.data(),saved.size());
    Check(heap->Restore(&input),"physical heap Restore");
    auto after_restore=memory.TryGetCertifiedPhysicalSnapshot(address,8192,after_save);
    Check(after_restore&&after_restore!=after_save&&after_restore->bytes()->at(19)==0xA5,"Restore did not revoke/recover exact bytes");
    surviving=after_restore;
    memory.Reset();
    const auto reset_before=memory.certified_physical_snapshot_stats();
    Check(!memory.ValidatePhysicalSnapshot(address,8192,surviving),"reuse-only survived Memory Reset");
    NoColdChange(reset_before,memory.certified_physical_snapshot_stats());

    Check(!memory.TryGetCertifiedPhysicalSnapshot(address,8192,surviving),"Memory Reset retained readable certificate");
    auto stats=memory.certified_physical_snapshot_stats();
    Check(stats.hits&&stats.faults&&stats.external_writes&&stats.protect_calls,"actual SDK evidence counters absent");
    Check(early?stats.owned_fault_dispatches>0:stats.owned_fault_dispatches==0,"early owned-fault dispatch evidence");
    Check(batch?stats.batch_queries>0&&stats.query_calls==0:stats.batch_queries==0&&stats.query_calls>0,"actual Memory batch evidence");
    Check(batch?stats.cold_ns>0&&stats.metadata_ns>0&&stats.query_ns>0:stats.cold_ns==0&&stats.metadata_ns==0&&stats.query_ns==0,"actual opt-in phase timing evidence");
  }
  Check(surviving&&surviving->bytes()->at(19)==0xA5,"certificate owner failed Memory teardown lifetime");
  std::cout<<"PASS: actual Memory enable admission, all aliases, unaligned metadata, Protect/old access, free/reuse, expanded legacy watch, raw ownership separation, external writer, Save/Restore/Reset retained teardown owner and reuse-only invalid-token no-rearm proof\n";
}
}
int main(){try{
#if REX_PLATFORM_LINUX || REX_PLATFORM_WIN32
  for(bool batch:{false,true})for(bool early:{false,true})Run(early,batch);
#else
  Memory memory;Check(!memory.SetCertifiedPhysicalSnapshotsEnabled(true),"unsupported platform enabled certificate");
  std::cout<<"PASS: unsupported platform requires exact fallback\n";
#endif
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
