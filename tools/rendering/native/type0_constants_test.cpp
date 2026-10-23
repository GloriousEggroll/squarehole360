// Exercise the actual PM4 decoder and ring reader. A scripted backend records
// the existing bulk entry point while retaining original register side effects.
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/native_completion.h>
#include <rex/system/xmemory.h>
#include <rex/memory/ring_buffer.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

REXCVAR_DECLARE(bool, gpu_bulk_type0_constants);
using namespace rex::graphics;
namespace {
size_t checks=0;
void Check(bool condition,const char* message) {
  ++checks;if(!condition)throw std::runtime_error(message);
}
constexpr uint32_t kPhysical=0x02000000,kGuest=0xa2000000;
class TestGraphics final:public GraphicsSystem {
public:
  explicit TestGraphics(rex::memory::Memory& memory){memory_=&memory;}
  std::string name()const override{return "Type0 constant CPU fixture";}
protected:
  void CreateProvider(bool)override{}
  std::unique_ptr<CommandProcessor>CreateCommandProcessor()override{return {};}
};
struct Result {
  bool accepted=false;uint32_t consumed=0,scratch_completions=0;
  std::vector<uint32_t> registers;
  std::array<uint32_t,8> scratch{};
  std::vector<std::pair<uint32_t,uint32_t>> writes,bulk;
};
class TestProcessor final:public CommandProcessor {
public:
  explicit TestProcessor(TestGraphics& graphics):CommandProcessor(&graphics,nullptr){}
  Result Run(uint32_t first,uint32_t count,bool repeat,uint32_t read_offset,bool truncated) {
    Check(count>=1&&count<=16384,"invalid fixture packet length");
    result_={};std::memset(register_file_->values,0,sizeof(register_file_->values));
    register_file_->values[XE_GPU_REG_SCRATCH_ADDR]=kPhysical;
    register_file_->values[XE_GPU_REG_SCRATCH_UMSK]=255;
    std::memset(memory_->TranslatePhysical(kPhysical),0,32);
    constexpr uint32_t capacity=131072;
    std::vector<uint32_t> ring_storage(capacity/4,0xfeedface);
    auto* bytes=reinterpret_cast<uint8_t*>(ring_storage.data());
    for(uint32_t n=0;n<count;++n) {
      // All bit patterns are transferred as integers, including NaN encodings.
      const uint32_t value=(n*0x9e3779b9u)^0xff817e00u;
      rex::memory::store_and_swap<uint32_t>(bytes+(read_offset+n*4)%capacity,value);
    }
    rex::memory::RingBuffer ring(bytes,capacity);ring.set_read_offset(read_offset);
    const uint32_t payload_bytes=(count-uint32_t(truncated))*4;
    ring.set_write_offset((read_offset+payload_bytes)%capacity);
    const uint32_t packet=((count-1)<<16)|first|(uint32_t(repeat)<<15);
    result_.accepted=ExecutePacketType0(&ring,packet);
    result_.consumed=(ring.read_offset()+capacity-read_offset)%capacity;
    result_.registers.assign(register_file_->values,register_file_->values+RegisterFile::kRegisterCount);
    for(uint32_t i=0;i<8;++i)result_.scratch[i]=rex::memory::load_and_swap<uint32_t>(memory_->TranslatePhysical(kPhysical+4*i));
    return std::move(result_);
  }
  void IssueSwap(uint32_t,uint32_t,uint32_t)override{}
  void TracePlaybackWroteMemory(uint32_t,uint32_t)override{}
  void RestoreEdramSnapshot(const void*)override{}
protected:
  bool SetupContext()override{return true;}
  void ShutdownContext()override{}
  Shader* LoadShader(xenos::ShaderType,uint32_t,const uint32_t*,uint32_t)override{return nullptr;}
  bool IssueDraw(xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool)override{return false;}
  bool IssueCopy()override{return false;}
  void WriteRegister(uint32_t index,uint32_t value)override {
    if(NativeScratchWriteNeedsCompletion(index,register_file_->values[XE_GPU_REG_SCRATCH_UMSK]))
      ++result_.scratch_completions;
    result_.writes.emplace_back(index,value);
    CommandProcessor::WriteRegister(index,value);
  }
  void WriteRegistersFromMem(uint32_t first,uint32_t* words,uint32_t count)override {
    result_.bulk.emplace_back(first,count);
    CommandProcessor::WriteRegistersFromMem(first,words,count);
  }
private:
  Result result_;
};
void Decoder() {
  rex::memory::Memory memory;Check(memory.Initialize(),"SDK memory initialization failed");
  auto* heap=memory.LookupHeap(kGuest);
  Check(heap&&heap->AllocFixed(kGuest,0x10000,0x10000,
      rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite),"physical fixture allocation failed");
  TestGraphics graphics(memory);TestProcessor processor(graphics);
  struct Case {uint32_t first,count;bool eligible;};
  // Independent literal boundaries from the Xbox register file. Crossing a
  // constant-family edge must retain the original per-register dispatch.
  std::vector<Case> cases{{0x4000,2048,true},{0x4000,1025,true},
      {0x43fd,9,true},{0x47ff,1,true},{0x4800,192,true},{0x4803,7,true},
      {0x48bf,1,true},{0x4900,40,true},{0x4907,2,true},{0x4927,1,true},
      {0x3fff,2,false},{0x47ff,2,false},{0x48bf,2,false},{0x48ff,2,false},
      {0x4927,2,false},{XE_GPU_REG_SCRATCH_REG0,8,false},
      {XE_GPU_REG_COHER_STATUS_HOST,1,false}};
  for(uint32_t n=0;n<256;++n)cases.push_back({0x4000+n*7,1+n%31,true});
  size_t selected=0,wrapped=0;
  for(const auto c:cases)for(bool repeat:{false,true})
    for(uint32_t offset:{0u,131068u,131060u})for(bool truncated:{false,true}) {
      REXCVAR_SET(gpu_bulk_type0_constants,false);
      const auto off=processor.Run(c.first,c.count,repeat,offset,truncated);
      REXCVAR_SET(gpu_bulk_type0_constants,true);
      const auto on=processor.Run(c.first,c.count,repeat,offset,truncated);
      Check(off.accepted==!truncated&&on.accepted==off.accepted,"truncation acceptance changed");
      Check(off.consumed==on.consumed&&on.consumed==(truncated?0:c.count*4),"ring consumption changed");
      Check(off.registers==on.registers&&off.writes==on.writes&&off.scratch==on.scratch&&
          off.scratch_completions==on.scratch_completions,"register order, bits or side effects changed");
      Check(off.bulk.empty(),"disabled mode used bulk dispatch");
      const bool bulk=c.eligible&&!repeat&&!truncated;
      Check(on.bulk.empty()!=bulk,"constant range dispatch eligibility differs");
      if(truncated)Check(on.writes.empty(),"truncated packet caused partial effects");
      else {
        Check(on.writes.size()==c.count,"register writes lost");
        for(uint32_t i=0;i<c.count;++i)Check(on.writes[i]==std::pair(c.first+(repeat?0:i),(i*0x9e3779b9u)^0xff817e00u),"independent register-word oracle differs");
      }
      if(bulk) {
        ++selected;uint32_t n=0;
        for(auto [first,count]:on.bulk){Check(first==c.first+n&&count>0,"split range reordered");n+=count;}
        Check(n==c.count&&on.bulk.size()<=2,"split bulk range changed extent");
        if(on.bulk.size()==2)++wrapped;
      }
    }
  Check(selected>0&&wrapped>0,"missing positive and wrapped bulk cases");
  REXCVAR_SET(gpu_bulk_type0_constants,false);
}
}
int main()try {Decoder();std::cout<<"PASS "<<checks<<" checks: actual Type0 decoder, exact register bits/order, ring wrap, repeated writes, truncation and scratch side effects\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
