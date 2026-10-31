#include "renderer/native/memory_coherence.h"
#include <rex/system/external_write_ranges.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <latch>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#if GOW3_HOST_MIMALLOC
// rexruntime frees through mimalloc, so this probe allocates from it too.
#include <mimalloc.h>
#define GOW3_FIXTURE_MALLOC mi_malloc
#define GOW3_FIXTURE_FREE mi_free
#else
#define GOW3_FIXTURE_MALLOC std::malloc
#define GOW3_FIXTURE_FREE std::free
#endif
#if REX_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace allocation_probe {
thread_local bool active = false;
thread_local size_t count = 0;
}
void* operator new(size_t bytes) {
  if (allocation_probe::active) ++allocation_probe::count;
  if (void* value = GOW3_FIXTURE_MALLOC(bytes ? bytes : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { GOW3_FIXTURE_FREE(value); }
void operator delete(void* value, size_t) noexcept { GOW3_FIXTURE_FREE(value); }
void operator delete[](void* value) noexcept { GOW3_FIXTURE_FREE(value); }
void operator delete[](void* value, size_t) noexcept { GOW3_FIXTURE_FREE(value); }

using namespace rex::memory;
using gow3::renderer::NativeMemoryCoherence;
namespace {
constexpr uint32_t kAddress = 0x02000000, kPage = 4096, kLength = 0x10000;
constexpr uint32_t kRW = kMemoryProtectRead | kMemoryProtectWrite;
constexpr uint32_t kCommit = kMemoryAllocationReserve | kMemoryAllocationCommit;
size_t checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
struct Notice { uint32_t address, length; bool exact, pending; };
struct State {
  Memory* memory = nullptr;
  NativeMemoryCoherence coherence;
  std::array<Notice, 32> notices{};
  size_t count = 0;
  std::latch* entered = nullptr;
  std::latch* proceed = nullptr;
  static std::pair<uint32_t, uint32_t> Notify(void* context, uint32_t address,
                                            uint32_t length, bool exact) {
    auto& state = *static_cast<State*>(context);
    if (state.count == state.notices.size()) std::terminate();
    state.notices[state.count++] = {
        address, length, exact, state.memory->IsExternalHostWritePending(address, length)};
    state.coherence.Invalidate(address, length);
    if (state.entered) {
      state.entered->count_down();
      state.proceed->wait();
    }
    // Deliberately ask to unwatch everything. Notification-only completion must
    // ignore this suggestion and must not alter genuine or watched permissions.
    return {0, UINT32_MAX};
  }
};
struct Harness {
  Memory memory;
  State state;
  void* callback = nullptr;
  uint32_t address = kAddress;
  uint8_t* destination = nullptr;
  explicit Harness(unsigned alias = 0) {
    Check(memory.Initialize(), "Memory initialization");
    state.memory = &memory;
    if (alias == 4) {
      address = 0x00400000; // Existing C0/7F GPU writeback allocation.
      destination = memory.TranslateVirtual(0x7F000000 + address);
    } else {
      const uint32_t guest = alias == 2 ? 0xC0000000 + address :
          alias == 3 ? 0xDFFFF000 + address : 0xA0000000 + address;
      const uint32_t bytes = alias == 2 ? 0x01000000 : kLength;
      Check(memory.LookupHeap(guest)->AllocFixed(guest, bytes, alias == 3 ? kPage : bytes,
                                                kCommit, kRW), "physical allocation");
      destination = alias == 0 ? memory.TranslatePhysical(address) : memory.TranslateVirtual(guest);
    }
    std::memset(memory.TranslatePhysical(address), 0x44, kLength);
    callback = memory.RegisterPhysicalMemoryInvalidationCallback(State::Notify, &state);
  }
  ~Harness() { if (callback) memory.UnregisterPhysicalMemoryInvalidationCallback(callback); }
  void Watch() {
    state.coherence.Watch(address, 3 * kPage);
    (void)state.coherence.TakeWatches();
  }
  void CheckNotice(uint32_t first, uint32_t bytes, size_t expected = 1) {
    Check(state.count == expected, "unexpected completion notification count");
    for (size_t i = 0; i < state.count; ++i) {
      const auto& n = state.notices[i];
      Check(n.address == first && n.length == bytes && n.exact && n.pending,
            "notification range, exact flag, or active-writer lifetime changed");
    }
  }
};
using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
File PayloadFile() {
  File file(std::tmpfile(), &std::fclose);
  Check(bool(file), "temporary source file");
  Check(std::setvbuf(file.get(), nullptr, _IONBF, 0) == 0, "unbuffered source file");
  constexpr std::array<uint8_t, 9> bytes{1, 2, 3, 4, 5, 6, 7, 8, 9};
  Check(std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size(), "source write");
  std::rewind(file.get());
  return file;
}
void RetireWithoutAllocation(std::optional<Memory::ExternalHostWrite>& guard) {
  allocation_probe::count = 0;
  allocation_probe::active = true;
  guard.reset();
  allocation_probe::active = false;
  Check(allocation_probe::count == 0, "completion notification allocated metadata");
}
void PartialAndFailedWrite(unsigned alias, bool certificates) {
  Harness h(alias);
  if (certificates) Check(h.memory.SetCertifiedPhysicalSnapshotsEnabled(true), "certificate enable");
  auto old = std::vector<uint8_t>(h.memory.TranslatePhysical(h.address),
                                   h.memory.TranslatePhysical(h.address) + kPage);
  auto token = certificates ? h.memory.TryGetCertifiedPhysicalSnapshot(h.address, kPage) : nullptr;
  if (certificates) Check(bool(token), "initial immutable certificate");
  h.Watch();
  auto file = PayloadFile();
  std::optional<Memory::ExternalHostWrite> guard;
  guard.emplace(h.memory.BeginExternalHostWrite(h.destination + 13, 64));
  Check(bool(*guard), "writer admission");
  Check(h.state.count == 0, "completion callback ran before host write");
  Check(std::fread(h.destination + 13, 1, 64, file.get()) == 9 && std::feof(file.get()),
        "actual OS-backed partial write not observed");
  Check(h.memory.IsExternalHostWritePending(h.address, kPage), "partial writer disappeared early");
  if (certificates)
    Check(!h.memory.TryGetCertifiedPhysicalSnapshot(h.address, kPage, token),
          "active host writer allowed a certificate");
#if REX_PLATFORM_LINUX
  // A completed partial transfer followed by a real syscall error in the same
  // guard lifetime: retirement has no success-only status gate.
  errno = 0;
  Check(::read(-1, h.destination + 22, 55) == -1 && errno == EBADF, "actual OS read error");
#endif
  RetireWithoutAllocation(guard);
  h.CheckNotice(h.address, kPage);
  Check(!h.memory.IsExternalHostWritePending(h.address, kPage), "retired writer remained active");
  Check(h.state.coherence.TakeInvalidated() == NativeMemoryCoherence::Ranges{{h.address, kPage}},
        "partial/error write lost exact native invalidation or touched unrelated page");
  Check(old[13] == 0x44 && h.memory.TranslatePhysical(h.address)[13] == 1 &&
        h.memory.TranslatePhysical(h.address)[22] == 0x44, "immutable owner or untouched bytes changed");
  if (certificates) {
    const auto next = h.memory.TryGetCertifiedPhysicalSnapshot(h.address, kPage, token);
    Check(next && next != token && token->bytes()->at(13) == 0x44 && next->bytes()->at(13) == 1,
          "completed write reused stale certificate");
  }
}
void DuplicatesAndMove() {
  Harness h;
  h.Watch();
  std::optional<Memory::ExternalHostWrite> first, second;
  first.emplace(h.memory.BeginExternalHostWrite(h.destination + kPage - 4, 12));
  second.emplace(h.memory.BeginExternalHostWrite(h.destination + kPage - 4, 12));
  auto moved = std::move(*first);
  first.reset();
  Check(h.state.count == 0, "moved-from guard notified a write");
  RetireWithoutAllocation(second);
  Check(h.memory.IsExternalHostWritePending(h.address, 2 * kPage), "duplicate guard retired too early");
  h.CheckNotice(h.address, 2 * kPage);
  std::optional<Memory::ExternalHostWrite> last(std::move(moved));
  RetireWithoutAllocation(last);
  h.CheckNotice(h.address, 2 * kPage, 2);
  Check(!h.memory.IsExternalHostWritePending(h.address, 2 * kPage), "last duplicate not retired");
  Check(h.state.coherence.TakeInvalidated() == NativeMemoryCoherence::Ranges{{h.address, 2 * kPage}},
        "overlapping guards changed canonical dirty union");
  const size_t notices = h.state.count;
  { auto zero = h.memory.BeginExternalHostWrite(h.destination, 0); Check(bool(zero), "zero guard"); }
  uint8_t ordinary = 0;
  { auto unrelated = h.memory.BeginExternalHostWrite(&ordinary, 1); Check(bool(unrelated), "ordinary guard"); }
  Check(h.state.count == notices, "nonphysical or zero guard notified physical memory");
}
void PermissionsPreserved() {
  Harness h(1);
  auto* heap = h.memory.LookupHeap(0xA0000000 + h.address);
  for (const auto permission : {PageAccess::kReadOnly, PageAccess::kNoAccess}) {
    Check(heap->Protect(0xA0000000 + h.address, kLength, permission == PageAccess::kReadOnly ? kMemoryProtectRead : kMemoryProtectNoAccess), "genuine host protection");
    h.Watch();
    std::optional<Memory::ExternalHostWrite> guard;
    guard.emplace(h.memory.BeginExternalHostWrite(h.destination + 7, 9));
    Check(bool(*guard), "failed operation guard admission");
    RetireWithoutAllocation(guard); // zero bytes written; conservative notification
    size_t extent = 0; PageAccess actual = PageAccess::kReadWrite;
    Check(QueryProtect(h.destination, extent, actual) && actual == permission,
          "notification-only completion weakened a genuine permission");
    Check(h.state.coherence.TakeInvalidated() == NativeMemoryCoherence::Ranges{{h.address, kPage}},
          "failed write notification lost ownership invalidation");
    Check(heap->Protect(0xA0000000 + h.address, kLength, kRW), "restore fixture protection");
  }
}
void LastWriterRace() {
  Harness h;
  h.Watch();
  std::optional<Memory::ExternalHostWrite> guard;
  guard.emplace(h.memory.BeginExternalHostWrite(h.destination, kPage));
  h.destination[0] = 0x97;
  std::latch callback_entered(1), callback_proceed(1), observer_started(1);
  h.state.entered = &callback_entered; h.state.proceed = &callback_proceed;
  std::atomic<bool> observed = false, correct = false;
  std::thread retirement([&] { guard.reset(); });
  callback_entered.wait();
  std::thread capture([&] {
    observer_started.count_down();
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    correct = !h.memory.IsExternalHostWritePending(h.address, kPage) && h.state.count == 1 &&
        h.state.coherence.TakeInvalidated() == NativeMemoryCoherence::Ranges{{h.address, kPage}} &&
        h.destination[0] == 0x97;
    observed = true;
  });
  observer_started.wait();
  const bool locked = rex::thread::global_critical_region::mutex().try_lock();
  if (locked) rex::thread::global_critical_region::mutex().unlock();
  const bool raced = observed.load();
  callback_proceed.count_down(); retirement.join(); capture.join();
  Check(!locked && !raced && correct, "snapshot crossed notification/last-writer retirement gap");
  h.state.entered = h.state.proceed = nullptr;
}
void RangeOracle() {
  // Independent byte-address enumeration, including overlapping host aliases,
  // descending physical mappings, page boundaries, and Windows E host offset.
  const std::array<ExternalWriteAlias, 5> aliases{{
      {0x10000, 0x1000, 0x5000}, {0x20000, 0x3000, 0x7000},
      {0x30000, 0, 0x4000}, {0x40000 + 0xF000, 0x1000, 0x5000},
      {0x11000, 0x8000, 0xC000}}};
  for (uint32_t page : {4096u, 16384u}) for (uintptr_t host = 0xF000; host < 0x55000; host += 1009)
    for (size_t length : {size_t(0), size_t(1), size_t(4097), size_t(17001)}) {
      std::set<uint32_t> expected, actual;
      for (const auto& a : aliases) {
        const auto first = std::max(host, a.host_first);
        const auto last = std::min(host + length, a.host_first + a.physical_end - a.physical_first);
        for (auto p = first; p < last; ++p) expected.insert(uint32_t(a.physical_first + p - a.host_first) / page);
      }
      const auto ranges = ExternalWritePhysicalRanges(host, length, page, aliases);
      Check(ranges.count <= 5, "range scratch count exceeded fixed bound");
      for (size_t i = 0; i < ranges.count; ++i) {
        const auto [first, bytes] = ranges.values[i];
        Check(bytes && first % page == 0 && bytes % page == 0, "range rounding changed");
        if (i) Check(ranges.values[i-1].first + ranges.values[i-1].second < first, "ranges were not merged");
        for (uint32_t p = first / page; p < (first + bytes) / page; ++p) actual.insert(p);
      }
      Check(actual == expected, "five-alias physical union differs from byte oracle");
    }
}
}
int main() try {
  RangeOracle();
  for (unsigned alias = 0; alias < 5; ++alias) PartialAndFailedWrite(alias, false);
  PartialAndFailedWrite(0, true);
  DuplicatesAndMove(); PermissionsPreserved(); LastWriterRace();
  std::cout << "PASS: " << checks << " bounded physical-union, actual partial/error I/O, raw/A/C/E/7F aliases, "
      "certification off/on, permissions, immutable owners, duplicate/moved guards, allocation-free retirement and atomic last-writer checks\n";
} catch (const std::exception& error) {
  allocation_probe::active = false;
  std::cerr << error.what() << '\n'; return 1;
}
