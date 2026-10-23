#include "renderer/native/memory_coherence.h"
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>
#include <atomic>
#include <cstring>
#include <exception>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace rex::memory;
using gow3::renderer::NativeMemoryCoherence;
namespace {
constexpr uint32_t kAddress=0x02000000,kGuest=0xA2000000,kPage=4096,kLength=0x10000;
constexpr uint32_t kRW=kMemoryProtectRead|kMemoryProtectWrite;
constexpr uint32_t kCommit=kMemoryAllocationReserve|kMemoryAllocationCommit;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct State {
  NativeMemoryCoherence coherence;
  std::atomic<uint64_t> epoch=0;
  static std::pair<uint32_t,uint32_t> Notify(void* context,uint32_t address,uint32_t bytes,bool) {
    auto& self=*static_cast<State*>(context);
    ++self.epoch;self.coherence.Invalidate(address,bytes);
    return {address,bytes};
  }
};
struct Harness {
  Memory memory;
  State state;
  void* callback=nullptr;
  uint32_t guest;
  explicit Harness(uint32_t alias=kGuest):guest(alias) {
    Check(memory.Initialize(),"Memory Initialize");
    const uint32_t allocation=guest>=0xC0000000u&&guest<0xE0000000u?0x01000000:kLength;
    const uint32_t alignment=guest>=0xE0000000u?kPage:allocation;
    Check(memory.LookupHeap(guest)->AllocFixed(guest,allocation,alignment,kCommit,kRW),"physical allocation");
    std::memset(memory.TranslatePhysical(kAddress),0x44,kLength);
    callback=memory.RegisterPhysicalMemoryInvalidationCallback(State::Notify,&state);
  }
  ~Harness(){if(callback)memory.UnregisterPhysicalMemoryInvalidationCallback(callback);}
  void Arm(uint32_t address=kAddress,uint32_t bytes=kPage) {
    auto lock=rex::thread::global_critical_region::AcquireDirect();
    state.coherence.Watch(address,bytes);
    for(auto [first,count]:state.coherence.TakeWatches())
      Check(memory.TryEnablePhysicalMemoryAccessCallbacks(first,count,true,false),"checked initial arm");
  }
  std::pair<uint64_t,std::vector<uint8_t>> Capture(uint32_t address=kAddress) {
    auto lock=rex::thread::global_critical_region::AcquireDirect();
    if(memory.IsExternalHostWritePending(address,kPage))throw std::runtime_error("external writer");
    if(!(int(memory.GetPhysicalHeap()->QueryRangeAccess(address,address+kPage-1))&int(PageAccess::kReadOnly)))
      return {state.epoch.load(),{}};
    auto result=state.coherence.CaptureCpuSnapshot(address,kPage,
      [&](uint32_t first,uint32_t count){Check(memory.TryEnablePhysicalMemoryAccessCallbacks(first,count,true,false),"checked snapshot arm");},
      [&]{return state.epoch.load();},[&]{
        const auto* source=memory.TranslatePhysical(address);
        return std::vector<uint8_t>(source,source+kPage);
      });
    Check(result.first==state.epoch,"snapshot publication changed epoch");
    return result;
  }
};
void PendingStoreRetry(uint32_t alias) {
  Harness h(alias);h.Arm();
  std::latch notified(1),resume(1);std::exception_ptr writer_error;
  std::thread writer([&]{
    try {
      // Execute the actual SDK notification/unprotect path, then pause at the
      // fault handler's return-to-instruction seam, before the store retries.
      Check(h.memory.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
            h.guest,1,true,true),"initial pending-store notification");
    }catch(...){writer_error=std::current_exception();}
    notified.count_down();resume.wait();
    if(!writer_error)try{
      *static_cast<volatile uint8_t*>(h.memory.TranslateVirtual(h.guest))=0x91;
    }catch(...){writer_error=std::current_exception();}
  });
  notified.wait();
  if(writer_error){resume.count_down();writer.join();std::rethrow_exception(writer_error);}
  std::pair<uint64_t,std::vector<uint8_t>> first;
  try {
    Check(h.state.epoch==1,"notification epoch");
    Check(h.state.coherence.TakeInvalidated()==NativeMemoryCoherence::Ranges{{kAddress,kPage}},"initial dirty page");
    first=h.Capture();
    Check(first.first==1&&first.second.front()==0x44,"pending instruction was incorrectly assumed completed");
  }catch(...){resume.count_down();writer.join();throw;}
  resume.count_down();writer.join();if(writer_error)std::rethrow_exception(writer_error);
  Check(h.state.epoch==2,"retried actual store did not refault after rearm");
  Check(h.state.coherence.TakeInvalidated()==NativeMemoryCoherence::Ranges{{kAddress,kPage}},"retried store lost invalidation");
  const auto second=h.Capture();
  Check(second.first==2&&second.second.front()==0x91,"new store missing from fresh snapshot");
  Check(first.second.front()==0x44,"retained immutable snapshot changed");
}
void LockSerializesNewFaultAndFree() {
  Harness h;h.Arm();
  auto transaction=rex::thread::global_critical_region::AcquireDirect();
  std::latch attempting(1);std::thread writer([&]{attempting.count_down();
    *static_cast<volatile uint8_t*>(h.memory.TranslateVirtual(kGuest))=0xA3;
  });
  attempting.wait();
  auto before=h.Capture();
  Check(before.second.front()==0x44&&h.state.epoch==0,"callback entered protected transaction");
  transaction.unlock();writer.join();
  Check(h.state.epoch==1&&h.memory.TranslatePhysical(kAddress)[0]==0xA3,"queued fault did not finish");
  h.state.coherence.TakeInvalidated();auto retained=h.Capture();
  Check(h.memory.LookupHeap(kGuest)->Release(kGuest),"release watched allocation");
  Check(!h.state.coherence.TakeInvalidated().empty(),"free failed to invalidate retained snapshot");
  Check(h.Capture().second.empty(),"freed page was dereferenced");
  Check(retained.second.front()==0xA3,"free changed immutable retained bytes");
}
void ExternalWriterRanges() {
  Harness h;h.Arm();
  for(uint32_t alias:{kAddress,0xA0000000u+kAddress,0xC0000000u+kAddress,0xDFFFF000u+kAddress}) {
    auto* pointer=alias==kAddress?h.memory.TranslatePhysical(alias):h.memory.TranslateVirtual(alias);
    {
      auto first=h.memory.BeginExternalHostWrite(pointer+13,100);
      Check(bool(first),"first external writer admission");
      auto duplicate=h.memory.BeginExternalHostWrite(pointer+13,100);
      Check(bool(duplicate),"duplicate external writer admission");
      auto moved=std::move(first);
      Check(h.memory.IsExternalHostWritePending(kAddress,kPage),"alias writer not detected");
      Check(!h.memory.IsExternalHostWritePending(kAddress+kPage,kPage),"unrelated external writer rejected");
      bool rejected=false;try{h.Capture();}catch(const std::runtime_error& e){rejected=std::string(e.what())=="external writer";}
      Check(rejected,"snapshot accepted an overlapping OS write");
      Check(h.Capture(kAddress+kPage).second.size()==kPage,"unrelated page snapshot blocked");
    }
    Check(!h.memory.IsExternalHostWritePending(kAddress,kPage),"retired duplicate guard remained active");
  }
  // These were outside the old optional-certificate domain. Track them without
  // enabling certificates or converting a genuine RO page to writable.
  {
    auto low=h.memory.BeginExternalHostWrite(h.memory.TranslateVirtual(0x7F001000),19);
    Check(bool(low)&&h.memory.IsExternalHostWritePending(0x1000,kPage),"7F low physical alias missed");
  }
  Check(!h.memory.IsExternalHostWritePending(0x1000,kPage),"low writer failed to retire");
  Check(h.memory.IsExternalHostWritePending(UINT32_MAX,4096),"overflow range accepted");
  Check(!h.memory.BeginExternalHostWrite(reinterpret_cast<void*>(UINTPTR_MAX-4),8),"overflow writer admitted");
  std::vector<Memory::ExternalHostWrite> guards;guards.reserve(Memory::kMaxExternalHostWrites);
  uint8_t unrelated=0;
  for(size_t i=0;i<Memory::kMaxExternalHostWrites;++i) {
    guards.push_back(h.memory.BeginExternalHostWrite(&unrelated,1));
    Check(bool(guards.back()),"writer capacity reached early");
  }
  Check(!h.memory.BeginExternalHostWrite(&unrelated,1),"writer capacity not bounded");
  guards.clear();
  Check(bool(h.memory.BeginExternalHostWrite(&unrelated,1)),"writer capacity failed to recover");
  Check(h.memory.LookupHeap(kGuest)->Protect(kGuest,kLength,kMemoryProtectRead),"genuine RO protection");
  Check(h.Capture().second.front()==0x44,"genuine readable RO snapshot failed");
  Check(h.memory.LookupHeap(kGuest)->QueryRangeAccess(kGuest,kGuest+kPage-1)==PageAccess::kReadOnly,
        "checked rearm weakened genuine RO permission");
}
#if REX_PLATFORM_LINUX
void PartialArmFailureRetainsRepair() {
  Harness h;
  auto* alias=h.memory.TranslateVirtual(kGuest);
  // Linux mprotect can protect the first VMA then report a hole. This uses the
  // SDK's actual platform primitive, not a fake all-or-nothing protect model.
  Check(UnmapFileView(kFileMappingHandleInvalid,alias+kPage,kPage),"create alias hole");
  Check(!h.memory.TryEnablePhysicalMemoryAccessCallbacks(kAddress,3*kPage,true,false),"partial OS arm accepted");
  Check(!h.memory.TryEnablePhysicalMemoryAccessCallbacks(kAddress,3*kPage,true,false),"uncertain bits skipped failed OS retry");
  Check(!h.memory.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
        kGuest,3*kPage,true,true),"partial unprotect failure claimed completion");
  Check(!h.memory.TryEnablePhysicalMemoryAccessCallbacks(kAddress,3*kPage,true,false),
        "partial unprotect lost forced-rearm state");
  size_t extent=0;PageAccess access=PageAccess::kNoAccess;
  Check(QueryProtect(alias,extent,access)&&access==PageAccess::kReadOnly,"fixture did not create partial RO protection");
  *static_cast<volatile uint8_t*>(alias)=0xD4;
  Check(h.state.epoch>0,"partial RO page lost fault repair ownership");
  Check(AllocFixed(alias+kPage,kPage,AllocationType::kReserveCommit,PageAccess::kReadWrite)==alias+kPage,"repair alias hole");
  Check(h.memory.TryEnablePhysicalMemoryAccessCallbacks(kAddress,3*kPage,true,false),"checked arm did not recover after repair");
  const auto epoch=h.state.epoch.load();
  *static_cast<volatile uint8_t*>(alias+2*kPage)=0xE5;
  Check(h.state.epoch>epoch,"repaired later page missed real write fault");
}
#endif
}
int main()try {
  for(uint32_t alias:{kGuest,0xC0000000u+kAddress,0xDFFFF000u+kAddress})PendingStoreRetry(alias);
  LockSerializesNewFaultAndFree();ExternalWriterRanges();
#if REX_PLATFORM_LINUX
  PartialArmFailureRetainsRepair();
#endif
  std::cout<<"PASS: actual SDK pending-store refault, locked publication, free/RO behavior, immutable lifetime, exact external-writer aliases/duplicates/moves/bounds and partial-arm repair\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
