// Actual SDK PM4 decoding with real mapped memory; no Vulkan/GPU dependency.
// Policy rejection is tested before sample fabrication, suppressed backend
// errors or packet-boundary advancement. Legacy behavior remains a control.
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native_scene_authority.h>
#include <rex/graphics/native_completion.h>
#include <rex/system/xmemory.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace rex::graphics;
namespace {
unsigned checks=0;
void Check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
template<class F> void Rejects(F&& f){bool rejected=false;try{f();}catch(const std::runtime_error& e){rejected=std::string(e.what()).find("Automatic fallback")!=std::string::npos;}Check(rejected,"operation did not fail closed");}
void Policy() {
  using Op=NativeSceneAuthority::ReferenceOperation;
  for(unsigned n=0;n<unsigned(Op::Count);++n) {
    NativeSceneAuthority legacy;legacy.Reference(Op(n),"legacy control");
    Check(legacy.reference_counts()[n]==1,"legacy entry was not counted");
    Rejects([&]{legacy.EnableStrict();});Check(!legacy.strict(),"late Native install succeeded");
    NativeSceneAuthority native;native.EnableStrict();
    unsigned observed=0;
    native.SetFailureObserver(&observed,[](void* p,std::string_view) noexcept {++*static_cast<unsigned*>(p);});
    Rejects([&]{native.Reference(Op(n),"forbidden legacy operation");});
    Check(native.reference_counts()[n]==0&&observed==1,"rejected scene operation ran or lost fatal diagnostic");
    Rejects([&]{native.ProviderChange("provider removal");});
    Check(native.strict()&&native.rejected()==2,"provider removal changed sticky authority");
  }
  for(unsigned providers=0;providers<16;++providers) {
    NativeSceneAuthority native;native.EnableStrict();
    if(providers==15)native.Require(true,"all providers");
    else Rejects([&]{native.Require(providers==15,"missing provider");});
    Check(native.strict(),"missing provider downgraded Native");
  }
  NativeCompletionState completion;completion.AcceptPending();
  try {completion.Complete([]{throw std::runtime_error("GPU failure");});}catch(const std::runtime_error&){}
  Check(completion.pending(),"completion failure released accepted ownership");
  completion.Complete([]{});Check(!completion.pending(),"terminal completion did not retire ownership");
}
constexpr uint32_t kPhysical=0x02000000,kGuest=0xa2000000,kPacket=kPhysical+0x1000,kOutput=kPhysical+0x2000;
class TestGraphics final:public GraphicsSystem {
 public:explicit TestGraphics(rex::memory::Memory& memory){memory_=&memory;}
 std::string name()const override{return "strict native CPU fixture";}
 protected:void CreateProvider(bool)override{} std::unique_ptr<CommandProcessor>CreateCommandProcessor()override{return {};}
};
class TestProcessor final:public CommandProcessor {
 public:
  TestProcessor(TestGraphics& graphics,bool strict,bool backend=true):CommandProcessor(&graphics,nullptr),backend_(backend) {
    std::memset(register_file_->values,0,sizeof(register_file_->values));if(strict)authority_.EnableStrict();
  }
  void Run(std::span<const uint32_t> words) {
    for(size_t i=0;i<words.size();++i)rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(kPacket+4*i),words[i]);
    ExecutePacket(kPacket,uint32_t(words.size()));
  }
  void Viz(uint32_t value){register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY]=value;}
  uint32_t Status()const{return register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0];}
  unsigned draws=0,failures=0;
  void IssueSwap(uint32_t,uint32_t,uint32_t)override{}
  void TracePlaybackWroteMemory(uint32_t,uint32_t)override{}
  void RestoreEdramSnapshot(const void*)override{}
 protected:
  bool SetupContext()override{return true;}void ShutdownContext()override{}
  Shader*LoadShader(xenos::ShaderType,uint32_t,const uint32_t*,uint32_t)override{return nullptr;}
  bool IssueDraw(xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool)override{++draws;return backend_;}
  bool IssueCopy()override{return false;}
  void OnPacketExecutionFailure(const char* reason)override{++failures;authority_.Require(false,reason);}
 private:NativeSceneAuthority authority_;bool backend_;
};
uint32_t DrawInitiator(xenos::SourceSelect source=xenos::SourceSelect::kAutoIndex) {
  reg::VGT_DRAW_INITIATOR d{};d.source_select=source;d.prim_type=xenos::PrimitiveType::kTriangleList;d.num_indices=3;return d.value;
}
void Decoder() {
  rex::memory::Memory memory;Check(memory.Initialize(),"SDK mapped Memory failed");
  auto* heap=memory.LookupHeap(kGuest);
  Check(heap&&heap->AllocFixed(kGuest,0x10000,0x10000,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite),"physical memory allocation failed");
  TestGraphics graphics(memory);
  const std::array draw{0xc0003600u,DrawInitiator()};
  {TestProcessor p(graphics,true);p.Run(draw);Check(p.draws==1&&!p.failures,"valid native decoder draw rejected");}
  {TestProcessor p(graphics,false,false);p.Run(draw);Check(p.draws==1&&p.failures==1,"legacy failed-draw control changed");}
  {TestProcessor p(graphics,true,false);Rejects([&]{p.Run(draw);});Check(p.draws==1&&p.failures==1,"backend failure was silently dropped");}
  {TestProcessor p(graphics,true);p.Viz(0x81);Rejects([&]{p.Run(draw);});Check(!p.draws,"post-HiZ skip reached backend");}
  {TestProcessor p(graphics,false);p.Viz(0x81);p.Run(draw);Check(!p.draws,"legacy post-HiZ control changed");}
  {TestProcessor p(graphics,true);const std::array words{0xc0012200u,0x100u,DrawInitiator()};Rejects([&]{p.Run(words);});Check(!p.draws,"conditional viz draw ignored its query");}
  {TestProcessor p(graphics,true);const std::array words{0xc0003600u,DrawInitiator(xenos::SourceSelect::kImmediate)};Rejects([&]{p.Run(words);});Check(!p.draws,"unsupported immediate indices reached backend");}
  {TestProcessor p(graphics,true);const std::array words{0xc0002300u,0x100u};Rejects([&]{p.Run(words);});Check(!p.Status(),"native viz query fabricated visible status");}
  {TestProcessor p(graphics,false);const std::array words{0xc0002300u,0x100u};p.Run(words);Check(p.Status()==1,"legacy viz-query control changed");}
  {TestProcessor p(graphics,true);const std::array words{0xc0015a00u,0u,kOutput|1u};std::memset(memory.TranslatePhysical(kOutput),0x5a,12);
   Rejects([&]{p.Run(words);});std::array<uint8_t,12> expected;expected.fill(0x5a);Check(!std::memcmp(expected.data(),memory.TranslatePhysical(kOutput),12),"native screen extents were fabricated");}
  {TestProcessor p(graphics,true);const std::array words{0xc0033600u,DrawInitiator()};Rejects([&]{p.Run(words);});Check(!p.draws,"malformed packet reached backend");}
  {TestProcessor p(graphics,true);const std::array words{0xc0007f00u,0u};Rejects([&]{p.Run(words);});Check(!p.draws,"unknown opcode advanced silently");}
}
}
int main()try{Policy();Decoder();std::cout<<"PASS "<<checks<<" checks: exclusive scene policy and actual PM4 draw/query failure boundaries\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
