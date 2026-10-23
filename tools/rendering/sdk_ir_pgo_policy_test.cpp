// Tiny compiler admission fixture. No SDK/game/GPU dependency.
#include <cstdint>
#define KEEP __attribute__((noinline))
KEEP void ExceptionCleanupWitness(unsigned x) { if (x == 42) throw x; }
struct CleanupWitness {
  unsigned value;
  KEEP ~CleanupWitness() noexcept { ExceptionCleanupWitness(value); }
};
namespace rex::graphics {
class CommandProcessor {
 public:
  KEEP unsigned ExecutePacket(unsigned x);
  KEEP void UpdateWritePointer(unsigned x);
};
unsigned CommandProcessor::ExecutePacket(unsigned x) {
  CleanupWitness cleanup{x};
  auto local = [](unsigned y) __attribute__((noinline)) { return y > 7 ? y + 3 : y - 1; };
  return local(x);
}
void CommandProcessor::UpdateWritePointer(unsigned x) { volatile unsigned value=x; (void)value; }
namespace vulkan {
class VulkanCommandProcessor { public: KEEP unsigned IssueDraw(unsigned x); };
unsigned VulkanCommandProcessor::IssueDraw(unsigned x) { return x ? x+9 : 4; }
}
}
namespace rex::memory {
class BaseHeap { public: KEEP unsigned QueryRangeAccess(unsigned first,unsigned last); };
unsigned BaseHeap::QueryRangeAccess(unsigned first,unsigned last) { return first<=last?3:0; }
class RingBuffer { public: KEEP unsigned Read(unsigned x); };
unsigned RingBuffer::Read(unsigned x) { return x&3; }
class Memory { public: KEEP bool AccessViolationCallback(unsigned x); };
bool Memory::AccessViolationCallback(unsigned x) { return x>3; }
class PhysicalSnapshotTracker { public: KEEP bool HandleFault(unsigned x); };
bool PhysicalSnapshotTracker::HandleFault(unsigned x) { return x>4; }
}
namespace rex::runtime {
class FunctionDispatcher { public: KEEP unsigned Execute(unsigned x); };
unsigned FunctionDispatcher::Execute(unsigned x) { return x+2; }
KEEP unsigned ResolveIndirectFunction(unsigned x) { return x+3; }
class MMIOHandler { public: KEEP unsigned ExceptionCallback(unsigned x); };
unsigned MMIOHandler::ExceptionCallback(unsigned x) { return x+5; }
}
namespace rex::thread { KEEP unsigned signal_handler(unsigned x) { return x+7; } }
template<class T> KEEP unsigned CommonTemplate(T x) { return unsigned(x)+11; }
KEEP unsigned UnrelatedGuestBody(unsigned x) { return CommonTemplate(x); }
extern "C" int run_policy_fixture(unsigned argc) {
  rex::graphics::CommandProcessor cp; cp.UpdateWritePointer(argc);
  rex::graphics::vulkan::VulkanCommandProcessor vk;
  rex::memory::BaseHeap heap; rex::memory::RingBuffer ring; rex::memory::Memory mem;
  rex::memory::PhysicalSnapshotTracker watch; rex::runtime::FunctionDispatcher dispatch;
  rex::runtime::MMIOHandler mmio;
  return int(cp.ExecutePacket(argc)+vk.IssueDraw(argc)+heap.QueryRangeAccess(argc,9)+
      ring.Read(argc)+dispatch.Execute(argc)+rex::runtime::ResolveIndirectFunction(argc)+
      mem.AccessViolationCallback(argc)+watch.HandleFault(argc)+mmio.ExceptionCallback(argc)+
      rex::thread::signal_handler(argc)+UnrelatedGuestBody(argc));
}
