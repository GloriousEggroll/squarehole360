// QueryRangeAccess caches results per thread; every page-table mutation must
// still be observed by the next query, on this thread and on others.
#include <rex/system/xmemory.h>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace rex::memory;
constexpr uint32_t Address = 0x02000000, Guest = 0xA2000000, Length = 0x400000;
constexpr uint32_t RW = kMemoryProtectRead | kMemoryProtectWrite;
constexpr uint32_t Commit = kMemoryAllocationReserve | kMemoryAllocationCommit;
size_t checks = 0;
void Check(bool value, const char* why) {
  ++checks;
  if (!value) throw std::runtime_error(why);
}
PageAccess Query(Memory& memory, uint32_t first, uint32_t bytes) {
  return memory.GetPhysicalHeap()->QueryRangeAccess(first, first + bytes - 1);
}
PageAccess OtherThread(Memory& memory, uint32_t first, uint32_t bytes) {
  PageAccess access{};
  std::thread([&] { access = Query(memory, first, bytes); }).join();
  return access;
}
// A new Memory may reuse the previous heap's address; its fresh state must
// not match a result cached for the old heap.
void ReusedHeapAddress() {
  for (int round = 0; round < 3; ++round) {
    Memory memory;
    Check(memory.Initialize(), "reused Memory Initialize failed");
    auto* heap = memory.LookupHeap(Guest);
    Check(heap->AllocFixed(Guest, Length, 0x10000, Commit, RW), "reused allocation failed");
    Check(Query(memory, Address, Length) == PageAccess::kReadWrite, "reused heap returned stale access");
    Check(heap->Release(Guest), "reused release failed");
    Check(Query(memory, Address, Length) == PageAccess::kNoAccess, "reused release not observed");
  }
}
}  // namespace

int main() try {
  ReusedHeapAddress();
  Memory memory;
  Check(memory.Initialize(), "Memory Initialize failed");
  auto* heap = memory.LookupHeap(Guest);
  Check(Query(memory, Address, Length) == PageAccess::kNoAccess, "unallocated range readable");
  Check(Query(memory, Address, Length) == PageAccess::kNoAccess, "cached unallocated range readable");
  Check(heap && heap->AllocFixed(Guest, Length, 0x10000, Commit, RW), "allocation failed");
  Check(Query(memory, Address, Length) == PageAccess::kReadWrite, "allocation not observed");
  Check(OtherThread(memory, Address, Length) == PageAccess::kReadWrite, "allocation not observed by other thread");
  for (int i = 0; i < 3; ++i) Check(Query(memory, Address, Length) == PageAccess::kReadWrite, "repeat query changed");
  // One page in the middle of a large cached range.
  Check(heap->Protect(Guest + 0x200000, 0x1000, kMemoryProtectRead), "protect failed");
  Check(Query(memory, Address, Length) == PageAccess::kReadOnly, "partial protect not observed");
  Check(Query(memory, Address, 0x1000) == PageAccess::kReadWrite, "unprotected subrange wrong");
  Check(OtherThread(memory, Address, Length) == PageAccess::kReadOnly, "protect not observed by other thread");
  Check(heap->Protect(Guest + 0x200000, 0x1000, kMemoryProtectNoAccess), "no-access protect failed");
  Check(Query(memory, Address, Length) == PageAccess::kNoAccess, "no-access protect not observed");
  Check(heap->Protect(Guest + 0x200000, 0x1000, RW), "restore protect failed");
  Check(Query(memory, Address, Length) == PageAccess::kReadWrite, "restore not observed");
  Check(heap->Decommit(Guest + 0x10000, 0x10000), "decommit failed");
  Check(heap->Release(Guest), "release failed");
  Check(Query(memory, Address, Length) == PageAccess::kNoAccess, "release not observed");
  Check(OtherThread(memory, Address, Length) == PageAccess::kNoAccess, "release not observed by other thread");
  Check(heap->AllocFixed(Guest, Length, 0x10000, Commit, kMemoryProtectRead), "read-only reallocation failed");
  Check(Query(memory, Address, Length) == PageAccess::kReadOnly, "reallocation not observed");
  std::cout << "page access cache: " << checks << " checks passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << "\n";
  return 1;
}
