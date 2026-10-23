// Runs the actual SDK PM4 WAIT decoder with real mapped guest memory and a
// scripted completion backend. The companion Scene fixture tests real GPU
// alias/readback correctness; this test makes no GPU sample claim.
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native_completion.h>
#include <rex/graphics/marker_query_cohort_census.h>
#include <rex/graphics/native_coherence_wait.h>
#include <rex/system/xmemory.h>
#include <rex/memory/ring_buffer.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace rex::graphics;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
constexpr uint32_t kPhysical = 0x02000000, kGuest = 0xa2000000;
constexpr uint32_t kPacket = kPhysical + 0x1000, kMemory = kPhysical + 0x2000;
NativeCoherenceWait Canonical() { return {5,3,0xa31,0,0x80000000,8,0x83000000}; }
NativeCoherenceOwnership Healthy() { return {true,true,false,false,false,true,true,true,true}; }

void CheckPolicy() {
  for (unsigned mask = 0; mask < 512; ++mask) {
    NativeCoherenceOwnership state{bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8),
      bool(mask&16),bool(mask&32),bool(mask&64),bool(mask&128),bool(mask&256)};
    const bool eligible=(mask&0x1e3)==0x1e3&&(mask&0x1c)==0;
    for (bool accepted : {false,true}) {
      unsigned submits=0,completes=0;
      NativeCompletionState latch;
      if(state.pending)latch.AcceptPending();
      const auto disposition=PrepareNativeCoherenceWait(Canonical(),state,[&]{++submits;return accepted;},
        [&]{++completes;latch.Complete([]{});});
      Check(submits==unsigned(eligible),"ownership policy admitted an invalid provider");
      Check(completes==unsigned(state.pending&&!(eligible&&accepted)),"missing/declined ownership lost full completion");
      Check(latch.pending()==(eligible&&accepted),"submit progress incorrectly cleared completion ownership");
      Check(disposition==(!state.pending?NativeCoherenceDisposition::NoPending:eligible&&accepted?
          NativeCoherenceDisposition::Submitted:NativeCoherenceDisposition::Completed),"wrong wait disposition");
    }
  }
  for(uint32_t action:{0x01000000u,0x02000000u,0x03000000u})
    for(uint32_t busy:{0u,0x80000000u}) {
      auto wait=Canonical();wait.status=action|busy;
      Check(IsNativeCacheOnlyWait(wait),"valid cache action rejected");
    }
  for(unsigned bit=0;bit<32;++bit) {
    auto wait=Canonical();wait.status|=uint32_t(1)<<bit;
    Check(IsNativeCacheOnlyWait(wait)==(bit==24||bit==25||bit==31),"unrecognized coherence status accepted");
  }
  for(uint32_t status:{0u,0x80000000u}) {
    auto wait=Canonical();wait.status=status;
    Check(!IsNativeCacheOnlyWait(wait),"no cache action was treated as owned invalidation");
  }
  for(unsigned field=0;field<5;++field) {
    auto wait=Canonical();
    switch(field) {
      case 0: wait.count=6;break;case 1:wait.info=0x13;break;
      case 2:wait.address=0xa32;break;case 3:wait.reference=1;break;
      case 4:wait.mask=0xffffffff;break;
    }
    unsigned submits=0,completes=0;
    PrepareNativeCoherenceWait(wait,Healthy(),[&]{++submits;return true;},[&]{++completes;});
    Check(!submits&&completes==1,"noncanonical operands escaped full completion");
  }
}
class TestGraphics final : public GraphicsSystem {
 public:
  explicit TestGraphics(rex::memory::Memory& memory){memory_=&memory;}
  std::string name()const override{return "coherence wait CPU fixture";}
 protected:
  void CreateProvider(bool)override{}
  std::unique_ptr<CommandProcessor>CreateCommandProcessor()override{return {};}
};
struct Case {
  NativeCoherenceWait wait=Canonical();
  NativeCoherenceOwnership ownership=Healthy();
  bool accepted=true,fail_submit=false,fail_complete=false,changed_memory=false;
};
class TestProcessor final : public CommandProcessor {
 public:
  TestProcessor(TestGraphics& graphics,Case test):CommandProcessor(&graphics,nullptr),test_(test) {
    std::memset(register_file_->values,0,sizeof(register_file_->values));
    register_file_->values[XE_GPU_REG_COHER_STATUS_HOST]=test_.wait.status;
    if(test_.ownership.pending)latch_.AcceptPending();
    Store(kMemory,1);
  }
  void Run() {
    const auto& w=test_.wait;
    const std::array<uint32_t,6> words{0xc0043c00,w.info,w.address,w.reference,w.mask,w.delay};
    for(unsigned i=0;i<words.size();++i)Store(kPacket+4*i,words[i]);
    bool failed=false;
    try{ExecutePacket(kPacket,words.size());}
    catch(const std::runtime_error&){failed=true;}
    Check(failed==(test_.fail_submit||test_.fail_complete),"unexpected real WAIT decoder failure");
    Check(decoded_==1,"actual WAIT operands did not pass through the pre-effect hook");
    const bool eligible=IsNativeCacheOnlyWait(w)&&test_.ownership.native_authority&&
      !test_.ownership.authority_retired&&!test_.ownership.foreign_query&&!test_.ownership.shutdown&&
      test_.ownership.draw_provider&&test_.ownership.query_provider&&test_.ownership.completion_provider&&
      test_.ownership.coherence_provider&&test_.ownership.pending;
    Check(submitted_==unsigned(eligible),"real decoder called the wrong progress provider");
    if(failed) {
      Check(!coherent_&&latch_.pending()&&register_file_->values[XE_GPU_REG_COHER_STATUS_HOST]==w.status,
        "failure changed status or released accepted work");
    } else {
      Check(latch_.pending()==(eligible&&test_.accepted),"real decoder cleared unfinished native ownership");
      if(!(w.info&0x10)&&w.address==XE_GPU_REG_COHER_STATUS_HOST)
        Check(coherent_==1&&register_file_->values[XE_GPU_REG_COHER_STATUS_HOST]==
            ((w.status&0x80000000u)?0u:w.status),
          "base MakeCoherent behavior changed");
      if(test_.changed_memory)
        Check(waited_==1&&completed_==1&&Read(kMemory)==1,
          "memory predicate was tested against stale pre-completion bytes");
    }
    // Original serial and scratch publication remain synchronous even after a
    // successful cache-only wait leaves accepted ownership pending.
    if(!failed&&latch_.pending()) {
      const std::array<uint32_t,4> serial{0xc0025800,3,kMemory|2u,0x31415926};
      for(unsigned i=0;i<serial.size();++i)Store(kPacket+4*i,serial[i]);
      ExecutePacket(kPacket,serial.size());
      Check(!latch_.pending()&&Read(kMemory)==0x31415926,
        "unchanged serial barrier failed to complete before its exact guest write");
      latch_.AcceptPending();register_file_->values[XE_GPU_REG_SCRATCH_ADDR]=kMemory;
      register_file_->values[XE_GPU_REG_SCRATCH_UMSK]=1;
      Store(kPacket,XE_GPU_REG_SCRATCH_REG0);Store(kPacket+4,0x27182818);
      ExecutePacket(kPacket,2);
      Check(!latch_.pending()&&Read(kMemory)==0x27182818,
        "unchanged scratch barrier failed to complete before its exact guest write");
    }
  }
  void IssueSwap(uint32_t,uint32_t,uint32_t)override{}
  void TracePlaybackWroteMemory(uint32_t,uint32_t)override{}
  void RestoreEdramSnapshot(const void*)override{}
 protected:
  bool SetupContext()override{return true;}
  void ShutdownContext()override{}
  Shader*LoadShader(xenos::ShaderType,uint32_t,const uint32_t*,uint32_t)override{return nullptr;}
  bool IssueDraw(xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool)override{return false;}
  bool IssueCopy()override{return false;}
  void PrepareForPacket(uint32_t opcode,uint32_t count)override {
    const auto& owner=test_.ownership;
    if(opcode==0x3c&&count==5&&owner.coherence_provider&&owner.native_authority&&
       !owner.authority_retired&&!owner.foreign_query&&!owner.shutdown&&
       owner.draw_provider&&owner.query_provider&&owner.completion_provider)return;
    if(NativePacketNeedsCompletion(opcode,count))Complete();
  }
  void PrepareForRegisterWait(uint32_t count,uint32_t info,uint32_t address,
      uint32_t reference,uint32_t mask,uint32_t delay)override {
    ++decoded_;
    Check(count==test_.wait.count&&info==test_.wait.info&&address==test_.wait.address&&
      reference==test_.wait.reference&&mask==test_.wait.mask&&delay==test_.wait.delay,
      "actual PM4 decoding changed or reordered wait operands");
    Check(coherent_==0&&register_file_->values[XE_GPU_REG_COHER_STATUS_HOST]==test_.wait.status,
      "decoded pre-effect callback ran after coherence mutation");
    const auto& owner=test_.ownership;
    if(!owner.coherence_provider||!owner.native_authority||owner.authority_retired||owner.foreign_query||
       owner.shutdown||!owner.draw_provider||!owner.query_provider||!owner.completion_provider)
      Check(!latch_.pending(),"absent/unhealthy option changed the original pre-decode barrier");
    auto ownership=test_.ownership;ownership.pending=latch_.pending();
    PrepareNativeCoherenceWait({count,info,address,reference,mask,delay,
      register_file_->values[XE_GPU_REG_COHER_STATUS_HOST]},ownership,[&] {
        ++submitted_;if(test_.fail_submit)throw std::runtime_error("progress failed");return test_.accepted;
      },[&]{Complete();});
  }
  void MakeCoherent()override{++coherent_;CommandProcessor::MakeCoherent();}
  void PrepareForWait()override {
    ++waited_;Check(test_.changed_memory&&waited_==1&&!latch_.pending(),"unexpected unmatched predicate");
    Store(kMemory,1); // Independent later CPU writer releases the real wait.
  }
  void WriteRegister(uint32_t index,uint32_t value)override {
    if(NativeScratchWriteNeedsCompletion(index,register_file_->values[XE_GPU_REG_SCRATCH_UMSK]))Complete();
    CommandProcessor::WriteRegister(index,value);
  }
 private:
  void Store(uint32_t address,uint32_t value){rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address),value);}
  uint32_t Read(uint32_t address)const{return rex::memory::load_and_swap<uint32_t>(memory_->TranslatePhysical(address));}
  void Complete() {
    latch_.Complete([&] {
      ++completed_;if(test_.fail_complete)throw std::runtime_error("completion failed");
      if(test_.changed_memory)Store(kMemory,2);
    });
  }
  Case test_;NativeCompletionState latch_;
  unsigned decoded_=0,submitted_=0,completed_=0,coherent_=0,waited_=0;
};
void CheckActualDecoder() {
  rex::memory::Memory memory;Check(memory.Initialize(),"SDK Memory initialization failed");
  auto* heap=memory.LookupHeap(kGuest);
  Check(heap&&heap->AllocFixed(kGuest,0x10000,0x10000,
      rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite),"physical fixture allocation failed");
  TestGraphics graphics(memory);std::vector<Case> cases;
  for(uint32_t action:{0x01000000u,0x02000000u,0x03000000u})
    for(uint32_t busy:{0u,0x80000000u}) {Case c;c.wait.status=action|busy;cases.push_back(c);}
  for(unsigned bit=0;bit<32;++bit)if(bit!=24&&bit!=25&&bit!=31){Case c;c.wait.status|=uint32_t(1)<<bit;cases.push_back(c);}
  for(unsigned field=0;field<11;++field) {
    Case c;
    switch(field) {
      case 0:c.ownership.coherence_provider=false;break;case 1:c.ownership.native_authority=false;break;
      case 2:c.ownership.authority_retired=true;break;case 3:c.ownership.foreign_query=true;break;
      case 4:c.ownership.shutdown=true;break;case 5:c.ownership.draw_provider=false;break;
      case 6:c.ownership.query_provider=false;break;case 7:c.ownership.completion_provider=false;break;
      case 8:c.accepted=false;break;case 9:c.fail_submit=true;break;
      case 10:c.accepted=false;c.fail_complete=true;break;
    }
    cases.push_back(c);
  }
  for(uint32_t info:{7u,0x103u}){Case c;c.wait.info=info;cases.push_back(c);}
  {Case c;c.wait.mask=0xffffffff;cases.push_back(c);}
  {Case c;c.wait.address=XE_GPU_REG_WAIT_UNTIL;cases.push_back(c);}
  {Case c;c.wait.info=0x13;c.wait.address=kMemory|2u;c.wait.reference=1;c.wait.mask=0xffffffff;
    c.changed_memory=true;cases.push_back(c);}
  for(const auto& c:cases){TestProcessor processor(graphics,c);processor.Run();}
}

using OperandCensus=gow3::diagnostic::MarkerOperandCensus;
OperandCensus::Snapshot MarkerWork(uint64_t work,uint64_t completed) {
  return {7,work-2,2,work,completed,true};
}
OperandCensus::Readiness MarkerBefore(uint32_t closed=0,bool active=false) {
  return {MarkerWork(8,6),closed,0,0,active,false,true};
}
OperandCensus::Operands MarkerOp(uint32_t begin,uint32_t target,uint32_t value) {
  return {0x12340000,256,begin+16,0xC0025800,3,3,target|2u,value};
}
OperandCensus::Allocation MarkerAllocation() {
  return {0x82001000,0xa2002000,kMemory,kPhysical,0x10000,96,11,4,true,true};
}
void CheckMarkerOperands() {
  using B=OperandCensus::Band;
  const auto finish=MarkerWork(12,8);
  OperandCensus c;c.SetEnabled(true);
  auto allocation=MarkerAllocation();
  c.BeginPacket();c.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
  c.Decoded(MarkerOp(32,kMemory+4,0xfffffff0),MarkerWork(8,8),allocation);
  c.BeginPacket();c.Decoded(MarkerOp(48,kMemory,0xfffffffe),MarkerWork(8,8),allocation);
  c.End(finish,OperandCensus::Boundary::QueryBegin);
  Check(c.summary().shape_pairs==1&&c.summary().allocation_matched==1,"canonical observed marker pair lost");
  for(auto b:{B::ZeroImmediate3,B::AllPair,B::ZeroPair,B::ZeroInactivePair,
      B::AllObservedAllocationPair,B::ZeroObservedAllocationPair,B::ZeroInactiveObservedAllocationPair}) {
    const auto& m=c.band(size_t(b)).summary();
    Check(m.admitted==1&&m.finalized==1&&m.draw_only==1&&m.work_distance[3]==1,
        "marker pair following-work attribution changed");
  }
  Check(c.example_count()==1&&c.example(0).position==0xfffffff0&&c.example(0).serial==0xfffffffe,
      "wrapped guest values were compared as serial progress instead of preserved bytes");
  // Each mutation independently invalidates an observed registration match,
  // while leaving its literal contiguous command pair classification intact.
  for(unsigned field=0;field<11;++field) {
    auto changed=allocation;
    switch(field) {
      case 0:changed.device++;break;case 1:changed.virtual_counter++;break;
      case 2:changed.physical_counter+=4;break;case 3:changed.registration_generation++;break;
      case 4:changed.command_epoch++;break;case 5:changed.live=false;break;
      case 6:changed.logical_bytes=32;break;case 7:changed.allocation_base++;break;
      case 8:changed.allocation_bytes--;break;case 9:changed.available=false;break;
      case 10:changed.registration_generation=0;break;
    }
    OperandCensus changed_c;changed_c.SetEnabled(true);
    changed_c.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
    changed_c.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8),allocation);
    changed_c.BeginPacket();changed_c.Decoded(MarkerOp(48,kMemory,2),MarkerWork(8,8),changed);
    Check(changed_c.summary().shape_pairs==1&&changed_c.summary().allocation_matched==0&&
        changed_c.band(size_t(B::ZeroObservedAllocationPair)).summary().admitted==0,
        "changed registration facts were called a matching allocation");
  }
  // Shape refusals must not manufacture a pair, including buffer reuse,
  // command gaps, predication, timestamp initiator and physical wrap.
  for(unsigned field=0;field<9;++field) {
    OperandCensus changed_c;changed_c.SetEnabled(true);
    changed_c.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
    changed_c.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8));
    auto second=MarkerOp(48,kMemory,2);
    switch(field) {
      case 0:second.packet|=1;break;case 1:second.initiator=0x80000003;break;
      case 2:second.address^=2;break;case 3:second.address+=4;break;
      case 4:second.command_buffer++;break;case 5:second.end_offset+=4;break;
      case 6:second.capacity+=4;break;case 7:second.count=4;break;
      case 8:second.address=0xfffffffau;break;
    }
    changed_c.BeginPacket();changed_c.Decoded(second,MarkerWork(8,8));
    Check(changed_c.summary().shape_pairs==0,"noncanonical operands became a canonical pair");
  }
  {OperandCensus x;x.SetEnabled(true);x.OriginalFull58(MarkerBefore(1),1,MarkerWork(8,8));
   x.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8));x.BeginPacket();
   x.Decoded(MarkerOp(48,kMemory,2),MarkerWork(8,8));
   Check(x.band(size_t(B::AllPair)).summary().admitted==1&&x.band(size_t(B::ZeroPair)).summary().admitted==0,
       "pending-query pair leaked into zero-query cohort");}
  {OperandCensus x;x.SetEnabled(true);x.OriginalFull58(MarkerBefore(0,true),0,MarkerWork(8,8));
   x.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8));x.BeginPacket();
   x.Decoded(MarkerOp(48,kMemory,2),MarkerWork(8,8));
   Check(x.band(size_t(B::ZeroPair)).summary().admitted==1&&x.band(size_t(B::ZeroInactivePair)).summary().admitted==0,
       "active-query state was erased by zero closed tickets");}
  {OperandCensus x;x.SetEnabled(true);x.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
   auto first=MarkerOp(224,kMemory+4,1);x.Decoded(first,MarkerWork(8,8));x.BeginPacket();
   auto second=MarkerOp(240,kMemory,2);second.end_offset=0;x.Decoded(second,MarkerWork(8,8));
   Check(x.summary().shape_pairs==1,"contiguous pair ending exactly at ring capacity rejected");}
  {OperandCensus x;x.SetEnabled(true);x.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
   auto first=MarkerOp(240,kMemory+4,1);first.end_offset=0;x.Decoded(first,MarkerWork(8,8));x.BeginPacket();
   x.Decoded(MarkerOp(0,kMemory,2),MarkerWork(8,8));
   Check(x.summary().shape_pairs==0,"wrapped pair accepted as contiguous32 command bytes");}
  {OperandCensus x;x.SetEnabled(true);x.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
   x.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8));x.End(MarkerWork(8,8),OperandCensus::Boundary::PrimaryBufferEnd);
   x.BeginPacket();x.Decoded(MarkerOp(48,kMemory,2),MarkerWork(8,8));
   Check(x.summary().shape_pairs==0,"pair joined across a strong boundary");}
  {OperandCensus x;x.SetEnabled(true);x.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
   x.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8));x.BeginPacket();
   x.Decoded(MarkerOp(48,kMemory,2),MarkerWork(9,9));
   Check(x.summary().pair_frontier_changed==1&&x.band(size_t(B::AllPair)).summary().admitted==0,
       "changed accepted frontier became a pair seed");}
  {OperandCensus x;x.SetEnabled(true);for(unsigned i=0;i<65;++i) {
     x.BeginPacket();x.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
     x.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8));}
   Check(x.band(size_t(B::ZeroImmediate3)).summary().admitted==64&&
       x.band(size_t(B::ZeroImmediate3)).summary().overflow_dropped==1,"operand measurement exceeded bounded storage");
   x.FailedCompletion();Check(x.open_markers()==0,"failed completion retained uncensored windows");}
  {OperandCensus x;x.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
   x.Decoded(MarkerOp(32,kMemory+4,1),MarkerWork(8,8));
   Check(x.summary().decoded==0&&x.summary().successful_full==0,"default OFF collected operand telemetry");}
}

// Actual SDK packet decoding and original native-style completion policy.
// The scripted GPU frontier is only a CPU ordering witness, not a GPU claim.
class MarkerTestProcessor final : public CommandProcessor {
 public:
  enum class Mode {Pair,Timestamp,Disabled,FailedCompletion,PredicatedSkip};
  MarkerTestProcessor(TestGraphics& graphics,Mode mode):CommandProcessor(&graphics,nullptr),mode_(mode) {
    std::memset(register_file_->values,0,sizeof(register_file_->values));
    register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR]=0xdead;
    counter_=0x24681357;observe_decoded_event_write_shd_=mode!=Mode::Disabled;
    census_.SetEnabled(observe_decoded_event_write_shd_);latch_.AcceptPending();
    Store(kMemory,0xaabbccdd);Store(kMemory+4,0x11223344);Store(kMemory+64,0x55667788);
    if(mode==Mode::PredicatedSkip)bin_mask_=0;
  }
  void Run() {
    std::array<uint32_t,8> words{0xc0025800,3,(kMemory+4)|2u,0xabcdef01,
        0xc0025800,3,kMemory|2u,0xfffffffe};
    unsigned size=8;
    if(mode_==Mode::Timestamp){words[1]=0x80000003;words[2]=(kMemory+64)|2u;size=4;}
    if(mode_==Mode::PredicatedSkip)words[0]|=1;
    for(unsigned i=0;i<size;++i)Store(kPacket+4*i,words[i]);
    bool failed=false;try{ExecutePacket(kPacket,size);}catch(const std::runtime_error&){failed=true;}
    Check(failed==(mode_==Mode::FailedCompletion),"actual marker parser failure changed");
    if(failed) {
      Check(decoded_==0&&latch_.pending()&&Read(kMemory)==0xaabbccdd&&Read(kMemory+4)==0x11223344&&
          register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR]==0xdead,
          "failed original completion reached decoded callback or store effects");return;
    }
    Check(completed_==1&&!latch_.pending(),"marker completion count/order changed");
    Check(pre_effect_ok_,"decoded marker callback ran before completion or after original effects");
    if(mode_==Mode::Timestamp) {
      Check(decoded_==1&&Read(kMemory+64)==counter_&&census_.summary().timestamp3==1&&
          census_.summary().shape_pairs==0,"counter-mode timestamp substituted raw operand or became pair");
    } else if(mode_==Mode::PredicatedSkip) {
      Check(decoded_==1&&Read(kMemory+4)==0x11223344&&Read(kMemory)==0xfffffffe&&
          census_.summary().shape_pairs==0,"predicated skip acquired a decoded pair");
    } else {
      Check(Read(kMemory+4)==0xabcdef01&&Read(kMemory)==0xfffffffe,
          "original position-before-serial marker bytes changed");
      Check(decoded_==(mode_==Mode::Disabled?0u:2u)&&census_.summary().shape_pairs==(mode_==Mode::Disabled?0u:1u),
          "actual parser did not expose exact32-byte pair or ignored diagnostic OFF");
    }
  }
  void IssueSwap(uint32_t,uint32_t,uint32_t)override{}
  void TracePlaybackWroteMemory(uint32_t,uint32_t)override{}
  void RestoreEdramSnapshot(const void*)override{}
 protected:
  bool SetupContext()override{return true;}void ShutdownContext()override{}
  Shader*LoadShader(xenos::ShaderType,uint32_t,const uint32_t*,uint32_t)override{return nullptr;}
  bool IssueDraw(xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool)override{return false;}
  bool IssueCopy()override{return false;}
  void PrepareForPacket(uint32_t opcode,uint32_t count)override {
    census_.BeginPacket();const bool pending=latch_.pending();
    if(NativePacketNeedsCompletion(opcode,count))latch_.Complete([&]{
      ++completed_;if(mode_==Mode::FailedCompletion)throw std::runtime_error("scripted accepted completion failure");});
    if(pending&&opcode==0x58)census_.OriginalFull58(MarkerBefore(),0,MarkerWork(8,8));
  }
  void ObserveDecodedEventWriteShd(uint32_t packet,uint32_t count,uint32_t initiator,
      uint32_t address,uint32_t value,uintptr_t command_buffer,uint32_t capacity,uint32_t end_offset)noexcept override {
    pre_effect_ok_&=!latch_.pending()&&completed_==1;
    if(!decoded_)pre_effect_ok_&=register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR]==0xdead&&
        Read(kMemory)==0xaabbccdd&&Read(kMemory+4)==0x11223344&&Read(kMemory+64)==0x55667788;
    else pre_effect_ok_&=register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR]==3&&
        Read(kMemory)==0xaabbccdd&&Read(kMemory+4)==0xabcdef01;
    ++decoded_;census_.Decoded({command_buffer,capacity,end_offset,packet,count,initiator,address,value},
        MarkerWork(8,8),MarkerAllocation());
  }
 private:
  void Store(uint32_t address,uint32_t value){rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address),value);}
  uint32_t Read(uint32_t address)const{return rex::memory::load_and_swap<uint32_t>(memory_->TranslatePhysical(address));}
  Mode mode_;NativeCompletionState latch_;OperandCensus census_;
  unsigned completed_=0,decoded_=0;bool pre_effect_ok_=true;
};
void CheckActualMarkerDecoder() {
  rex::memory::Memory memory;Check(memory.Initialize(),"marker fixture Memory initialization failed");
  auto* heap=memory.LookupHeap(kGuest);
  Check(heap&&heap->AllocFixed(kGuest,0x10000,0x10000,
      rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite),"marker fixture physical allocation failed");
  TestGraphics graphics(memory);
  for(auto mode:{MarkerTestProcessor::Mode::Pair,MarkerTestProcessor::Mode::Timestamp,
      MarkerTestProcessor::Mode::Disabled,MarkerTestProcessor::Mode::FailedCompletion,
      MarkerTestProcessor::Mode::PredicatedSkip}){MarkerTestProcessor p(graphics,mode);p.Run();}
}

// Runs the new actual base decoder using a scripted immutable-input/target
// guard. This proves parser/store order only, not real GPU or title ownership.
class ImmediateMarkerProcessor final:public CommandProcessor {
 public:
  enum class Mode {Off,Accepted,PrepareRefused,TargetRefused,PrepareFailure,ChangedSource,Partial,Nonpair};
  ImmediateMarkerProcessor(TestGraphics& g,Mode mode):CommandProcessor(&g,nullptr),mode_(mode) {
    native_immediate_marker_pairs_enabled_=mode!=Mode::Off;
    std::memset(register_file_->values,0,sizeof(register_file_->values));
    latch_.AcceptPending();Store(kMemory,0xaabbccdd);Store(kMemory+4,0x11223344);
    for(size_t i=8;i<96;i+=4)Store(kMemory+uint32_t(i),0x55667788);
  }
  void Run() {
    std::array<uint32_t,8> words{0xc0025800,3,(kMemory+4)|2u,0xfffffffc,
        0xc0025800,3,kMemory|2u,0xfffffffe};
    if(mode_==Mode::Nonpair)words[6]=(kMemory+8)|2u;
    for(size_t i=0;i<words.size();++i)Store(kPacket+uint32_t(4*i),words[i]);
    bool failed=false;
    rex::memory::RingBuffer reader(memory_->TranslatePhysical(kPacket),64);
    reader.set_write_offset(mode_==Mode::Partial?16:32);
    try {while(reader.read_count())Check(ExecutePacket(&reader),"actual immediate marker packet failed");}
    catch(const std::runtime_error&){failed=true;}
    const bool selected=mode_==Mode::Accepted||mode_==Mode::ChangedSource;
    Check(failed==(mode_==Mode::PrepareFailure),"immediate marker unexpected failure");
    Check(!guard_live_,"target guard escaped actual pair decoding");
    if(failed) {
      Check(!registers_&&!completed_&&latch_.pending()&&Read(kMemory)==0xaabbccdd&&
        Read(kMemory+4)==0x11223344,"failed submit published marker or lost pending work");return;
    }
    Check(completed_==unsigned(!selected)&&latch_.pending()==selected,
        "pair submission fabricated GPU completion or fallback lost original fence");
    Check(Read(kMemory+4)==0xfffffffc,"position store changed endian/value");
    Check(Read(kMemory)==(mode_==Mode::Partial||mode_==Mode::Nonpair?0xaabbccdd:0xfffffffe),
        "serial store changed endian/value or scalar fallback");
    for(uint32_t offset=mode_==Mode::Nonpair?12:8;offset<96;offset+=4)
      Check(Read(kMemory+offset)==0x55667788,"pair modified untouched logical counter bytes");
    Check(registers_==(mode_==Mode::Partial?1u:2u),"pair changed original register effect count");
    Check(command_summary_.type3_packets==(mode_==Mode::Partial?1u:2u),"pair changed packet accounting");
    // Original scratch completion follows after the guard has gone away.
    if(selected) {
      register_file_->values[XE_GPU_REG_SCRATCH_ADDR]=kMemory+64;
      register_file_->values[XE_GPU_REG_SCRATCH_UMSK]=1;
      Store(kPacket,XE_GPU_REG_SCRATCH_REG0);Store(kPacket+4,0x31415926);
      ExecutePacket(kPacket,2);
      Check(completed_==1&&!latch_.pending()&&Read(kMemory+64)==0x31415926,
          "later mandatory scratch fence did not survive immediate marker");
    }
  }
  void IssueSwap(uint32_t,uint32_t,uint32_t)override{}
  void TracePlaybackWroteMemory(uint32_t,uint32_t)override{}
  void RestoreEdramSnapshot(const void*)override{}
 protected:
  bool SetupContext()override{return true;}
  void ShutdownContext()override{}
  Shader*LoadShader(xenos::ShaderType,uint32_t,const uint32_t*,uint32_t)override{return nullptr;}
  bool IssueDraw(xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool)override{return false;}
  bool IssueCopy()override{return false;}
  void PrepareForPacket(uint32_t opcode,uint32_t count)override {
    if(NativePacketNeedsCompletion(opcode,count))Complete();
  }
  std::shared_ptr<void> PrepareNativeImmediateMarkerPair(const NativeCompletionMarkerPair& pair)override {
    Check(IsNativeCompletionMarkerPair(pair)&&pair.command_address==kPacket&&pair.counter==kMemory,
        "actual decoder supplied unauthenticated pair shape");
    Check(!registers_&&Read(kMemory+4)==0x11223344&&Read(kMemory)==0xaabbccdd,
        "input submission ran after first target effect");
    if(mode_==Mode::PrepareFailure)throw std::runtime_error("scripted submit failure");
    if(mode_==Mode::PrepareRefused||mode_==Mode::TargetRefused)return {};
    if(mode_==Mode::ChangedSource)Store(kPacket+28,0x10203040);
    guard_live_=true;
    return std::shared_ptr<void>(this,[this](void*)noexcept{guard_live_=false;});
  }
  void WriteRegister(uint32_t index,uint32_t value)override {
    if(index==XE_GPU_REG_VGT_EVENT_INITIATOR) {
      if(mode_==Mode::Accepted||mode_==Mode::ChangedSource) {
        Check(guard_live_&&latch_.pending(),"pair target lease/fence obligation missing at store");
        Check(Read(kMemory)==0xaabbccdd,"serial was published before original second register write");
        Check(Read(kMemory+4)==(registers_?0xfffffffc:0x11223344),"position-before-serial order changed");
      }
      ++registers_;
    }
    if(NativeScratchWriteNeedsCompletion(index,register_file_->values[XE_GPU_REG_SCRATCH_UMSK]))Complete();
    CommandProcessor::WriteRegister(index,value);
  }
 private:
  void Store(uint32_t address,uint32_t value){rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address),value);}
  uint32_t Read(uint32_t address)const{return rex::memory::load_and_swap<uint32_t>(memory_->TranslatePhysical(address));}
  void Complete(){Check(!guard_live_,"completion ran under target guard");latch_.Complete([&]{++completed_;});}
  Mode mode_;NativeCompletionState latch_;unsigned completed_=0,registers_=0;bool guard_live_=false;
};
void CheckImmediateMarkerDecoder() {
  rex::memory::Memory memory;Check(memory.Initialize(),"immediate marker Memory initialization failed");
  auto* heap=memory.LookupHeap(kGuest);
  Check(heap&&heap->AllocFixed(kGuest,0x10000,0x10000,
      rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite),"immediate marker physical allocation failed");
  TestGraphics graphics(memory);
  for(auto mode:{ImmediateMarkerProcessor::Mode::Off,ImmediateMarkerProcessor::Mode::Accepted,
      ImmediateMarkerProcessor::Mode::PrepareRefused,ImmediateMarkerProcessor::Mode::TargetRefused,
      ImmediateMarkerProcessor::Mode::PrepareFailure,ImmediateMarkerProcessor::Mode::ChangedSource,
      ImmediateMarkerProcessor::Mode::Partial,ImmediateMarkerProcessor::Mode::Nonpair}) {
    ImmediateMarkerProcessor p(graphics,mode);p.Run();
  }
}

void CheckActualMarkerAllocationObservations() {
  for(unsigned mutation=0;mutation<3;++mutation) {
    rex::memory::Memory memory;Check(memory.Initialize(),"allocation token fixture Memory initialization failed");
    auto* heap=memory.LookupHeap(kGuest);
    const auto allocate=[&] {return heap&&heap->AllocFixed(kGuest,0x10000,0x10000,
        rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite);};
    Check(allocate(),"allocation token fixture allocation failed");
    auto old=memory.ObservePhysicalAllocation(kPhysical,96);
    Check(old&&old->current()&&old->address==kPhysical&&old->bytes==0x10000,
        "real physical allocation did not produce exact observation token");
    if(mutation==0)Check(heap->Protect(kGuest,0x10000,
        rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite),
        "alias Protect failed");
    else if(mutation==1) {
      Check(heap->Release(kGuest),"alias Release failed");
      Check(!old->current(),"alias Release left old token current");
      Check(allocate(),"same-address physical reallocation failed");
    } else memory.GetPhysicalHeap()->Reset();
    Check(!old->current(),"real physical allocator mutation failed to invalidate observation");
    auto next=memory.ObservePhysicalAllocation(kPhysical,96);
    if(mutation==2)Check(!next,"reset physical allocation was reauthorized");
    else Check(next&&next->current()&&next!=old&&!old->current(),
        "same-address current allocation reused stale observation identity");
  }
}

}
int main()try {
  CheckPolicy();CheckActualDecoder();CheckMarkerOperands();CheckActualMarkerDecoder();CheckImmediateMarkerDecoder();CheckActualMarkerAllocationObservations();
  std::cout<<"PASS "<<checks<<" checks: exhaustive owned cache-wait policy, real CP WAIT operands/pre-effect hook, "
    "status clear, all excluded status/ownership cases, stale memory predicate, failure latch, serial and scratch stores; actual marker operands/pair/counter/predication/OFF/order/failure and bounded cohort helper; immediate marker pair exact stores/order/pending/scratch/failure/refusal/immutable bytes; real allocator protect/free/reuse/reset tokens\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
