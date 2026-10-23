#include <rex/system/xmemory.h>
#include <rex/stream.h>
#include "snapshot_batch_allocation_gate.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace rex::memory;
using Token = CertifiedPhysicalSnapshotPtr;
using Request = PhysicalSnapshotBatchRequest;
constexpr auto Capture = Request::Mode::Capture;
constexpr auto Validate = Request::Mode::ValidateOnly;
constexpr uint32_t Address = 0x02000000, Guest = 0xA2000000;
constexpr uint32_t Page = PhysicalSnapshotTracker::kPage, Length = 0x10000;
constexpr uint32_t RW = kMemoryProtectRead | kMemoryProtectWrite;
constexpr uint32_t Commit = kMemoryAllocationReserve | kMemoryAllocationCommit;
size_t checks = 0, oracle_bytes = 0, reached_oom = 0, actual_alias_writes = 0;
void Check(bool value, const char* why) {
  ++checks;
  if (!value) throw std::runtime_error(why);
}
void ExactBytes(const Token& token, const uint8_t* source, uint32_t length) {
  Check(token && token->bytes() && token->bytes()->size() == length, "actual Memory wrong view extent");
  for (size_t i = 0; i < length; ++i) {
    ++oracle_bytes;
    Check(token->bytes()->at(i) == source[i], "actual Memory independent byte oracle mismatch");
  }
}
void NoColdChange(const PhysicalSnapshotTracker::Stats& a, const PhysicalSnapshotTracker::Stats& b) {
  Check(a.captures == b.captures && a.capture_bytes == b.capture_bytes &&
        a.protect_calls == b.protect_calls && a.protect_failures == b.protect_failures &&
        a.query_calls == b.query_calls && a.batch_queries == b.batch_queries &&
        a.queried_pages == b.queried_pages && a.armed_pages == b.armed_pages &&
        a.metadata_bytes == b.metadata_bytes && a.cold_ns == b.cold_ns &&
        a.metadata_ns == b.metadata_ns && a.query_ns == b.query_ns &&
        a.capture_protect_ns == b.capture_protect_ns && a.copy_ns == b.copy_ns,
        "ValidateOnly changed capture/protection/metadata or cold clocks");
}
void Indexed(const std::vector<Token>& tokens, size_t count) {
  Check(tokens.size() == count, "actual Memory batch lost original output indices");
}
void Run(bool early, bool permission_batch) {
  Token surviving;
  std::vector<uint8_t> immutable;
  {
    Memory memory;
    const std::array<Request, 1> unopened{{{Address, Page, {}, Capture}}};
    Check(memory.TryGetCertifiedPhysicalSnapshots(unopened).empty(), "uninitialized Memory batch admitted");
    Check(memory.Initialize(), "actual Memory Initialize failed");
    memory.SetCertifiedPhysicalFaultFastPath(early);
    memory.SetCertifiedPhysicalSnapshotBatchQueries(permission_batch);
    auto* heap = memory.LookupHeap(Guest);
    Check(heap && heap->AllocFixed(Guest, Length, Length, Commit, RW), "actual physical allocation failed");
    auto* raw = memory.TranslatePhysical(Address);
    for (uint32_t i = 0; i < Length; ++i) raw[i] = uint8_t((i * 29u) ^ (i >> 5) ^ 0x6du);
    Check(memory.SetCertifiedPhysicalSnapshotsEnabled(true), "actual batch snapshot feature refused");
    std::array<Request, 3> requests{{
      {Address + 19, Page + 37, {}, Capture},
      {Address + 9 * Page + 11, Page + 13, {}, Capture},
      {Address + 19, Page + 37, {}, Capture}}};
    // Two exact disjoint ranges, one repeated descriptor, genuine untouched
    // gap permissions. The batch may not protect the intervening gap pages.
    Check(memory.GetPhysicalHeap()->Protect(Address + 5 * Page, Page, kMemoryProtectRead), "genuine gap protection failed");
    auto first = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(first, 3);
    for (size_t i = 0; i < requests.size(); ++i) {
      ExactBytes(first[i], memory.TranslatePhysical(requests[i].address), requests[i].length);
      auto scalar = memory.TryGetCertifiedPhysicalSnapshot(requests[i].address, requests[i].length, first[i]);
      Check(scalar == first[i] && scalar->bytes() == first[i]->bytes(), "scalar and batch token/owner identity differs");
      requests[i].previous = first[i]; requests[i].mode = Validate;
    }
    // All four actual aliases must leave every gap's real protection alone.
    for (uint32_t alias : {Address, 0xA0000000u + Address, 0xC0000000u + Address, 0xDFFFF000u + Address}) {
      auto* gap = alias == Address ? raw + 4 * Page : memory.TranslateVirtual(alias + 4 * Page);
      size_t extent = 0; PageAccess access{};
      Check(QueryProtect(gap, extent, access) && access == PageAccess::kReadWrite,
            "actual batch armed an unrequested gap");
    }
    Check(memory.GetPhysicalHeap()->QueryRangeAccess(Address + 5 * Page, Address + 6 * Page - 1) == PageAccess::kReadOnly,
          "batch relaxed genuine gap protection");
    const auto before_validate = memory.certified_physical_snapshot_stats();
    auto reuse = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(reuse, 3);
    for (size_t i = 0; i < requests.size(); ++i) Check(reuse[i] == first[i], "actual batch validation changed token");
    NoColdChange(before_validate, memory.certified_physical_snapshot_stats());

    for (uint32_t alias : {Address, 0xA0000000u + Address, 0xC0000000u + Address, 0xDFFFF000u + Address}) {
      auto* destination = alias == Address ? raw : memory.TranslateVirtual(alias);
      ++actual_alias_writes;
      *static_cast<volatile uint8_t*>(destination + 43) = uint8_t(alias >> 24);
      const auto invalid_before = memory.certified_physical_snapshot_stats();
      auto invalid = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(invalid, 3);
      Check(!invalid[0] && !invalid[2] && invalid[1] == requests[1].previous,
            "actual alias mutation rearmed invalid active member or dropped independent neighbor");
      NoColdChange(invalid_before, memory.certified_physical_snapshot_stats());
      const auto old_owner = requests[0].previous->bytes(); const auto old_bytes = *old_owner;
      for (auto& request : requests) request.mode = Capture;
      auto next = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(next, 3);
      for (size_t i = 0; i < requests.size(); ++i) {
        ExactBytes(next[i], memory.TranslatePhysical(requests[i].address), requests[i].length);
        Check(next[i]->bytes()->at(0) == memory.TranslatePhysical(requests[i].address)[0], "wrong indexed physical view");
        requests[i].previous = next[i]; requests[i].mode = Validate;
      }
      Check(*old_owner == old_bytes, "actual old immutable payload changed after fault");
    }

    // A cold inaccessible or unallocated member must not affect a valid active
    // neighbor. This validates canonical metadata before any source load.
    Check(memory.GetPhysicalHeap()->Protect(Address + 6 * Page, Page, kMemoryProtectNoAccess),
          "canonical no-access fixture failed");
    std::array<Request, 3> metadata{{requests[1],
      {Address + 6 * Page - 7, 19, {}, Capture},
      {Address + Length, Page, {}, Capture}}};
    auto unavailable = memory.TryGetCertifiedPhysicalSnapshots(metadata); Indexed(unavailable, 3);
    Check(unavailable[0] == requests[1].previous && !unavailable[1] && !unavailable[2],
          "batch skipped inaccessible boundary or uncommitted metadata");
    Check(memory.GetPhysicalHeap()->Protect(Address + 6 * Page, Page, RW), "canonical access restoration failed");

    // In-flight OS writers reject only intersecting ranges, including duplicate
    // move-only guards. Completing the guard cannot rearm an invalid active view.
    for (uint32_t alias : {Address, 0xA0000000u + Address, 0xC0000000u + Address, 0xDFFFF000u + Address}) {
      auto* pointer = alias == Address ? raw : memory.TranslateVirtual(alias);
      {
        auto guard = memory.BeginExternalHostWrite(pointer + 47, 31);
        auto duplicate = memory.BeginExternalHostWrite(pointer + 47, 31);
        Check(bool(guard) && bool(duplicate), "actual duplicate external guard admission failed");
        auto moved = std::move(guard);
        auto during = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(during, 3);
        Check(!during[0] && !during[2] && during[1] == requests[1].previous,
              "batch certified active external writer or refused unrelated range");
        auto capture_during = requests; capture_during[0].mode = Capture; capture_during[2].mode = Capture;
        auto rejected = memory.TryGetCertifiedPhysicalSnapshots(capture_during); Indexed(rejected, 3);
        Check(!rejected[0] && !rejected[2] && rejected[1] == requests[1].previous,
              "fresh actual SDK batch captured active external writer");
        pointer[47] = uint8_t(0xc0 + (alias >> 28));
      }
      auto ended = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(ended, 3);
      Check(!ended[0] && !ended[2], "external retirement rearmed invalid active member");
      for (auto& request : requests) request.mode = Capture;
      auto fresh = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(fresh, 3);
      for (size_t i = 0; i < requests.size(); ++i) {
        ExactBytes(fresh[i], memory.TranslatePhysical(requests[i].address), requests[i].length);
        requests[i].previous = fresh[i]; requests[i].mode = Validate;
      }
    }

    uint32_t old_access = 0;
    Check(heap->Protect(Guest, Page, kMemoryProtectRead, &old_access), "actual ordinary Protect failed");
    Check((old_access & kMemoryProtectWrite) != 0, "certificate changed ordinary old-protection result");
    auto protected_invalid = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(protected_invalid, 3);
    Check(!protected_invalid[0] && !protected_invalid[2], "ordinary Protect left old active token valid");
    Check(heap->Protect(Guest, Page, RW), "ordinary writable restore failed");
    for (auto& request : requests) request.mode = Capture;
    auto current = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(current, 3);
    auto before_free = current[0]; const auto before_free_bytes = *before_free->bytes();
    Check(heap->Release(Guest), "actual allocation Release failed");
    for (size_t i = 0; i < requests.size(); ++i) {requests[i].previous = current[i]; requests[i].mode = Validate;}
    auto freed = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(freed, 3);
    Check(std::none_of(freed.begin(), freed.end(), [](const auto& token){return bool(token);}),
          "released memory retained readable batch token");
    Check(*before_free->bytes() == before_free_bytes, "free changed retained original owner");
    Check(heap->AllocFixed(Guest, Length, Length, Commit, RW), "same-address allocation failed");
    std::memset(raw, 0x89, Length);
    auto reused_address = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(reused_address, 3);
    Check(std::none_of(reused_address.begin(), reused_address.end(), [](const auto& token){return bool(token);}),
          "same-address reallocation reused previous Memory epoch/token");
    for (auto& request : requests) request.mode = Capture;
    auto reallocated = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(reallocated, 3);
    for (size_t i = 0; i < requests.size(); ++i) {
      ExactBytes(reallocated[i], memory.TranslatePhysical(requests[i].address), requests[i].length);
      requests[i].previous = reallocated[i]; requests[i].mode = Validate;
    }

    // Exact SDK allocation-failure fallback: enumerate every reached standard
    // allocation until one complete cold call succeeds. Assertions run with
    // the thread-local gate disabled; failure must precede any protection.
    bool allocation_complete = false; size_t local_failures = 0;
    for (size_t failure = 0; failure < 128; ++failure) {
      const std::array<Request, 2> pending{{requests[1],
        {Address + 3 * Page + 5, Page + 7, {}, Capture}}};
      const auto before = memory.certified_physical_snapshot_stats();
      std::vector<Token> result;
      snapshot_batch_fixture::ArmAllocationFailure(failure);
      try { result = memory.TryGetCertifiedPhysicalSnapshots(pending); }
      catch (...) {snapshot_batch_fixture::DisarmAllocationFailure(); throw;}
      snapshot_batch_fixture::DisarmAllocationFailure();
      if (!result.empty() && result[1]) {
        Indexed(result, 2); Check(result[0] == requests[1].previous, "OOM sweep successful neighbor identity changed");
        allocation_complete = true; break;
      }
      ++reached_oom; ++local_failures;
      Check(snapshot_batch_fixture::reached_allocations == failure + 1, "SDK allocation failure gate was not reached exactly");
      // The platform permission query may itself convert bad_alloc into a
      // failed preflight. That preserves already validated neighbors; the
      // outer Memory catch instead returns an empty vector. Neither may mint
      // the cold view or mutate prior permissions.
      Check(result.empty() || (result.size() == 2 && result[0] == requests[1].previous && !result[1]),
            "failed SDK allocation minted cold output or changed valid neighbor");
      const auto after = memory.certified_physical_snapshot_stats();
      Check(after.protect_calls == before.protect_calls && after.armed_pages == before.armed_pages &&
            after.metadata_bytes == before.metadata_bytes, "SDK allocation failure changed permissions or metadata owners");
      Check(memory.ValidatePhysicalSnapshot(requests[1].address, requests[1].length, requests[1].previous) == requests[1].previous,
            "SDK allocation failure invalidated valid neighbor");
    }
    Check(allocation_complete && local_failures >= 5, "SDK allocation sweep did not reach fresh batch preparation");

    surviving = requests[1].previous;
    immutable = *surviving->bytes();
    const auto stats = memory.certified_physical_snapshot_stats();
    Check(stats.snapshot_batches && stats.snapshot_batch_views && stats.snapshot_batch_cold_views && stats.faults,
          "actual SDK batch route or write faults were vacuous");
    for (auto count : stats.alias_faults) Check(count > 0, "actual SDK omitted an alias fault");
    Check(early ? stats.owned_fault_dispatches > 0 : stats.owned_fault_dispatches == 0, "early fault mode mismatch");
    Check(permission_batch ? stats.batch_queries > 0 && stats.query_calls == 0 :
          stats.batch_queries == 0 && stats.query_calls > 0, "permission-query mode mismatch");
    Check(memory.SetCertifiedPhysicalSnapshotsEnabled(false), "SDK batch shutdown restriction repair failed");
    auto disabled = memory.TryGetCertifiedPhysicalSnapshots(requests);
    Check(disabled.empty(), "disabled Memory admitted batch");
    Check(memory.SetCertifiedPhysicalSnapshotsEnabled(true), "SDK re-enable failed");
    auto epoch = memory.TryGetCertifiedPhysicalSnapshots(requests); Indexed(epoch, 3);
    Check(std::none_of(epoch.begin(), epoch.end(), [](const auto& token){return bool(token);}),
          "disable/enable lifecycle reused old active token");
    memory.Reset();
    auto reset = memory.TryGetCertifiedPhysicalSnapshots(requests);
    Check(reset.empty() || std::none_of(reset.begin(), reset.end(), [](const auto& token){return bool(token);}),
          "Memory Reset certified previous allocation");
  }
  Check(surviving && *surviving->bytes() == immutable, "SDK token/owner failed Memory teardown lifetime");
  {
    Memory replacement;
    Check(replacement.Initialize(), "replacement Memory initialization failed");
    auto* heap = replacement.LookupHeap(Guest);
    Check(heap && heap->AllocFixed(Guest, Length, Length, Commit, RW), "replacement physical allocation failed");
    std::memset(replacement.TranslatePhysical(Address), 0x89, Length);
    Check(replacement.SetCertifiedPhysicalSnapshotsEnabled(true), "replacement certificates refused");
    const std::array<Request, 1> foreign{{{Address + 9 * Page + 11, Page + 13, surviving, Validate}}};
    auto wrong_memory = replacement.TryGetCertifiedPhysicalSnapshots(foreign); Indexed(wrong_memory, 1);
    Check(!wrong_memory[0], "replacement Memory identity accepted previous owner/token");
    Check(*surviving->bytes() == immutable, "replacement altered old retained bytes");
  }
}
}
int main() try {
#if REX_PLATFORM_LINUX
  for (bool query_batch : {false, true}) for (bool early : {false, true}) Run(early, query_batch);
  std::cout << "PASS: actual SDK Memory batch checks=" << checks << " independent byte oracle=" << oracle_bytes
            << " actual alias writes=" << actual_alias_writes << " reached OOM=" << reached_oom
            << "; exact scalar/batch owners, invalid active no-rearm, gap metadata, external writers, free/reuse/reset/teardown\n";
#else
  Memory memory;
  const std::array<Request, 1> request{{{Address, Page, {}, Capture}}};
  Check(!memory.SetCertifiedPhysicalSnapshotsEnabled(true) && memory.TryGetCertifiedPhysicalSnapshots(request).empty(),
        "unsupported platform minted batch certificate");
  std::cout << "PASS: unsupported certification platform requires exact fallback\n";
#endif
} catch (const std::exception& error) {
  snapshot_batch_fixture::DisarmAllocationFailure();
  std::cerr << error.what() << '\n'; return 1;
}
