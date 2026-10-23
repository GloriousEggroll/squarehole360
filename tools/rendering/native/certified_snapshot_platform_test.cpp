#include <rex/system/physical_snapshot.h>
#include <rex/system/mmio_handler.h>
#include <rex/memory/utils.h>
#include <rex/filesystem.h>
#include <rex/exception_handler.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace rex::memory;
namespace {
void Check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
using T=PhysicalSnapshotTracker;
struct Mapping {
  static constexpr uint32_t address=T::kFirst+0x2000;
  static constexpr size_t bytes=8192;
  static constexpr size_t reservation_bytes=0x120000000ull;
  uint8_t* base=nullptr;FileMappingHandle file=kFileMappingHandleInvalid;
  std::filesystem::path name;
  std::unique_ptr<T> tracker;
  std::unique_ptr<rex::runtime::MMIOHandler> handler;
  std::atomic<bool> pause_next=false,paused=false,resume=false;
  explicit Mapping(bool early,bool batch){
    Check(page_size()==4096&&allocation_granularity()==4096,"platform fixture requires validated 4KiB layout");
    name="gow3-certified-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    base=static_cast<uint8_t*>(AllocFixed(nullptr,reservation_bytes,AllocationType::kReserve,PageAccess::kNoAccess));
    Check(base,"reserve address space");
    file=CreateFileMappingHandle(name,0x20001000,PageAccess::kReadWrite,false);
    Check(file!=kFileMappingHandleInvalid,"create SDK shared file mapping");
    for(auto [offset,backing]:std::array<std::pair<uint64_t,size_t>,4>{{{0xA0000000,0},{0xC0000000,0},{0xE0000000,4096},{0x100000000,0}}})
      Check(MapFileView(file,base+offset,0x20000000,PageAccess::kReadWrite,backing)==base+offset,"map shared alias");
    T::Host host{};
    host.query=[](void*,uintptr_t p,size_t& length,T::Access& out){
      PageAccess actual{};if(!QueryProtect(reinterpret_cast<void*>(p),length,actual))return false;
      out=actual==PageAccess::kReadWrite?T::Access::ReadWrite:actual==PageAccess::kReadOnly?T::Access::ReadOnly:
          actual==PageAccess::kNoAccess?T::Access::NoAccess:T::Access::Unsupported;return true;
    };
    host.protect=[](void*,uintptr_t p,size_t n,T::Access a)noexcept{
      return Protect(reinterpret_cast<void*>(p),n,a==T::Access::ReadWrite?PageAccess::kReadWrite:PageAccess::kReadOnly);
    };
    host.query_pages=[](void*,std::span<const T::PageQuery> requests) {
      size_t total=0;for(const auto& r:requests)total+=r.pages.size();
      std::vector<PageAccess> values(total);std::vector<PageProtectionQuery> queries;
      size_t offset=0;for(const auto& r:requests){queries.push_back({r.address,std::span<PageAccess>(values).subspan(offset,r.pages.size())});offset+=r.pages.size();}
      if(!QueryProtectPages(queries))return false;
      offset=0;for(const auto& r:requests)for(auto& value:r.pages){
        const auto a=values[offset++];value=a==PageAccess::kReadWrite?T::Access::ReadWrite:
          a==PageAccess::kReadOnly?T::Access::ReadOnly:a==PageAccess::kNoAccess?T::Access::NoAccess:T::Access::Unsupported;
      }
      return true;
    };
    const auto v=reinterpret_cast<uintptr_t>(base);
    tracker=std::make_unique<T>(host,std::array<uintptr_t,4>{v+0x100000000,v+0xA0000000,v+0xC0000000,v+0xDFFFF000});
    handler=rex::runtime::MMIOHandler::Install(base,base+0x100000000,base+reservation_bytes-1,
      [](const void* context,const void* p){return uint32_t(static_cast<const uint8_t*>(p)-static_cast<const Mapping*>(context)->base);},this,
      [](std::unique_lock<std::recursive_mutex> lock,void* context,void* p,bool write){
        auto& m=*static_cast<Mapping*>(context);
        const auto result=m.tracker->HandleFault(reinterpret_cast<uintptr_t>(p),write);
        if(result==T::Fault::Handled&&m.pause_next.exchange(false)){
          // Deliberately hold the faulted store before instruction retry, after
          // releasing the SDK lock just as a real returning handler would.
          lock.unlock();m.paused.store(true,std::memory_order_release);
          while(!m.resume.load(std::memory_order_acquire))std::this_thread::yield();
        }
        return result==T::Fault::Handled;
      },this,early?+[](void* context,void* p,bool write) {
        auto& m=*static_cast<Mapping*>(context);
        const bool owned=m.tracker->OwnsFault(reinterpret_cast<uintptr_t>(p),write);
        if(owned)m.tracker->RecordOwnedFaultDispatch();return owned;
      }:nullptr);
    Check(bool(handler),"install SDK MMIO fault handler");
    std::memset(Pointer(0),0x35,bytes);
    tracker->SetBatchQueriesEnabled(batch);tracker->Enable();
  }
  ~Mapping(){
    if(tracker)tracker->Disable();handler.reset();tracker.reset();
    if(base)DeallocFixed(base,reservation_bytes,DeallocationType::kRelease);
    if(file!=kFileMappingHandleInvalid)CloseFileMappingHandle(file,name);
  }
  uint8_t* Pointer(unsigned a){
    constexpr std::array<uint64_t,4> offsets{0x100000000,0xA0000000,0xC0000000,0xDFFFF000};
    return base+offsets[a]+address;
  }
  CertifiedPhysicalSnapshotPtr Capture(const CertifiedPhysicalSnapshotPtr& old={}){
    auto lock=rex::thread::global_critical_region::AcquireDirect();return tracker->Capture(address,bytes,old);
  }
};
void Run(bool genuine_fault,bool early,bool batch=false){
  Mapping m(early,batch);
  // Keep one alias genuinely read-only. The tracker must not claim or relax it.
  Check(Protect(m.Pointer(2),4096,PageAccess::kReadOnly),"genuine readonly protect");
  Check(Protect(m.Pointer(3)+4096,4096,PageAccess::kNoAccess),"genuine noaccess protect");
  // Validate the portable batch API against actual fragmented mappings before
  // certification, using deliberately unsorted and overlapping requests.
  std::array<PageAccess,2> c{},e{},raw{};
  std::array<PageProtectionQuery,3> qs{{{reinterpret_cast<uintptr_t>(m.Pointer(0)),raw},
    {reinterpret_cast<uintptr_t>(m.Pointer(2)),c},{reinterpret_cast<uintptr_t>(m.Pointer(3)),e}}};
  Check(QueryProtectPages(qs),"actual complete permission batch");
  Check(raw[0]==PageAccess::kReadWrite&&raw[1]==PageAccess::kReadWrite&&c[0]==PageAccess::kReadOnly&&c[1]==PageAccess::kReadWrite&&
        e[0]==PageAccess::kReadWrite&&e[1]==PageAccess::kNoAccess,"batch crossed genuine permission boundary");
  std::array<PageAccess,2> overlap{};std::array<PageProtectionQuery,2> oq{{qs[1],{qs[1].address,overlap}}};
  Check(QueryProtectPages(oq)&&overlap==c,"overlapping queried ranges changed proof");
  auto malformed=qs;malformed[0].address+=1;Check(!QueryProtectPages(malformed),"unaligned request accepted");
  malformed=qs;malformed[0].address=UINTPTR_MAX-4095;Check(!QueryProtectPages(malformed),"overflow request accepted");
  std::array<std::array<PageAccess,1>,17> bound_values{};
  std::array<PageProtectionQuery,17> too_many{};
  for(size_t i=0;i<too_many.size();++i)too_many[i]={qs[0].address,bound_values[i]};
  Check(!QueryProtectPages(too_many),"range bound bypassed");
  Check(QueryProtectPages(std::span<const PageProtectionQuery>(too_many).first(16)),"exact range bound rejected");
  std::vector<PageAccess> too_large(16385);const PageProtectionQuery large{qs[0].address,too_large};
  Check(!QueryProtectPages(std::span(&large,1)),"page bound bypassed");
  Check(!QueryProtectPages({}),"empty request batch accepted");
  const PageProtectionQuery empty{qs[0].address,{}};
  Check(!QueryProtectPages(std::span(&empty,1)),"empty request range accepted");
  const auto hole=reinterpret_cast<uintptr_t>(m.base+0xB0000000ull);
  Check(DeallocFixed(reinterpret_cast<void*>(hole),4096,DeallocationType::kRelease),"create test mapping gap");
  std::array<PageAccess,1> gap_value{};const PageProtectionQuery gap{hole,gap_value};
  Check(!QueryProtectPages(std::span(&gap,1)),"unmapped gap accepted");
  std::array<PageAccess,1> partial_value{PageAccess::kNoAccess};
  std::array<PageProtectionQuery,2> partial{{{reinterpret_cast<uintptr_t>(m.Pointer(1)),partial_value},gap}};
  Check(!QueryProtectPages(partial)&&partial_value[0]==PageAccess::kReadWrite,"partial failed query fixture did not cross gap");
  {size_t n=0;PageAccess a{};Check(QueryProtect(m.Pointer(1),n,a)&&a==PageAccess::kReadWrite,"failed query changed actual permissions");}
  auto first=m.Capture();Check(bool(first),"initial real snapshot");
  if(genuine_fault){
    // SDK handlers run in registration order. This trailing observer is reached
    // only if the real MMIO callback declined the genuine guest restriction.
    // The separate bounded child exits without changing permissions/retrying
    // the store, avoiding the SDK's interactive unhandled-fault policy.
    rex::arch::ExceptionHandler::Install(+[](rex::arch::Exception* ex,void* context){
      auto& m=*static_cast<Mapping*>(context);
      const bool rejected=ex->code()==rex::arch::Exception::Code::kAccessViolation &&
        ex->access_violation_operation()==rex::arch::Exception::AccessViolationOperation::kWrite &&
        ex->fault_address()==reinterpret_cast<uintptr_t>(m.Pointer(2)) &&
        !m.tracker->OwnsFault(ex->fault_address(),true) &&
        m.tracker->stats().owned_fault_dispatches==0 &&
        *static_cast<volatile const uint8_t*>(m.Pointer(2))==0x35;
      std::_Exit(rejected?73:74);
      return false;
    },&m);
    // Existing views remain valid after closing/unlinking their backing handle;
    // the child must not leave a named shared-memory object after _Exit.
    CloseFileMappingHandle(m.file,m.name);m.file=kFileMappingHandleInvalid;
    *static_cast<volatile uint8_t*>(m.Pointer(2))=0xFF;
    throw std::runtime_error("genuine readonly fault swallowed");
  }
  Check(m.tracker->HandleFault(reinterpret_cast<uintptr_t>(m.Pointer(2)),true)==T::Fault::NotOwned,"genuine readonly ownership");
  Check(m.tracker->BeforeHostMutation(reinterpret_cast<uintptr_t>(m.Pointer(2)),4096),"normal readonly change guard");
  Check(Protect(m.Pointer(2),4096,PageAccess::kReadWrite),"normal readonly change");
  auto previous=m.Capture(first);
  for(unsigned a=0;a<4;++a)for(unsigned repeat=0;repeat<2;++repeat){
    auto* p=static_cast<volatile uint8_t*>(m.Pointer(a));p[31+a]=uint8_t(0x50+a*2+repeat);
    auto next=m.Capture(previous);Check(next&&next!=previous,"real alias fault did not invalidate");
    Check(next->bytes()->at(31+a)==0x50+a*2+repeat,"real aliased source bytes differ");
    Check(m.Capture(next)==next,"unchanged real certificate");previous=next;
  }
  Check(first->bytes()->at(31)==0x35,"retained real owner mutated");
  m.pause_next=true;m.resume=false;
  std::thread writer([&]{*static_cast<volatile uint8_t*>(m.Pointer(0))=0xC7;});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!m.paused.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  if(!m.paused){m.resume=true;writer.join();throw std::runtime_error("pending real fault did not reach pause");}
  auto before_retry=m.Capture(previous);const auto faults=m.tracker->stats().faults;
  m.resume.store(true,std::memory_order_release);writer.join();
  Check(m.tracker->stats().faults==faults+1,"real retry after capture failed to refault");
  Check(m.Capture(before_retry)->bytes()->front()==0xC7,"real retry mutation missed");
  const auto temporary=std::filesystem::temp_directory_path()/(m.name.string()+".bin");
  {std::ofstream output(temporary,std::ios::binary);std::string payload(217,0x6B);output.write(payload.data(),payload.size());}
  auto input=rex::filesystem::FileHandle::OpenExisting(temporary,rex::filesystem::FileAccess::kGenericRead);
  Check(bool(input),"SDK input file open");
  previous=m.Capture();
  {auto lock=rex::thread::global_critical_region::AcquireDirect();Check(m.tracker->BeginExternalWrite(reinterpret_cast<uintptr_t>(m.Pointer(0)),8192),"real kernel guard");}
  Check(!m.Capture(previous),"in-flight real kernel span certified");
  size_t read=0;input->Read(0,m.Pointer(0),8192,&read);Check(read==217,"real partial kernel read");
  {auto lock=rex::thread::global_critical_region::AcquireDirect();m.tracker->EndExternalWrite(reinterpret_cast<uintptr_t>(m.Pointer(0)),8192);}
  auto after=m.Capture(previous);Check(after->bytes()->front()==0x6B&&after->bytes()->at(216)==0x6B,"real partial IO missed");
  input.reset();std::filesystem::remove(temporary);
  const auto stats=m.tracker->stats();
  for(auto count:stats.alias_faults)Check(count>=2,"an alias lacked actual fault coverage");
  Check(stats.owned_fault_dispatches==(early?stats.faults:0),"actual MMIO early path did not match own fault handling");
  Check(batch?stats.batch_queries>0&&stats.query_calls==0:stats.batch_queries==0&&stats.query_calls>0,"actual batch mode evidence");
  Check(m.tracker->Disable(),"real disable restore");
  {size_t n=0;PageAccess a{};Check(QueryProtect(m.Pointer(3)+4096,n,a)&&a==PageAccess::kNoAccess,"batch/disable relaxed genuine NoAccess");}
  for(unsigned a=0;a<4;++a){size_t n=0;PageAccess access{};Check(QueryProtect(m.Pointer(a),n,access)&&access==PageAccess::kReadWrite,"disable did not restore writable alias");}
  std::cout<<"PASS: actual SDK four-alias faults="<<stats.faults<<", pending-store retry, genuine read-only ownership, immutable lifetime, exact hit, partial OS file read and shutdown restore\n";
}
}
int main(int argc,char** argv){try{
#if !REX_PLATFORM_LINUX
  std::cout<<"SKIP: platform certification unsupported; exact fallback required\n";return 0;
#else
  if(argc==2){
    const std::string_view mode=argv[1];
    if(mode=="--genuine-read-only-fault" || mode=="--genuine-read-only-fault=legacy")Run(true,false);
    else if(mode=="--genuine-read-only-fault=early")Run(true,true);
    else if(mode=="--genuine-read-only-fault=batch-legacy")Run(true,false,true);
    else if(mode=="--genuine-read-only-fault=batch-early")Run(true,true,true);
    else throw std::runtime_error("unknown fixture option");
    throw std::runtime_error("negative fixture returned without rejected write");
  }
  Check(argc==1,"unexpected fixture options");
  for(bool batch:{false,true})for(bool early:{false,true})Run(false,early,batch);return 0;
#endif
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
