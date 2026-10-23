#include <rex/system/physical_snapshot.h>
#include "snapshot_batch_allocation_gate.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using Tracker = rex::memory::PhysicalSnapshotTracker;
using Token = rex::memory::CertifiedPhysicalSnapshotPtr;
using Request = rex::memory::PhysicalSnapshotBatchRequest;
using Access = Tracker::Access;
constexpr auto Capture = Request::Mode::Capture;
constexpr auto Validate = Request::Mode::ValidateOnly;
constexpr uint32_t Page = Tracker::kPage;
size_t checks = 0, oracle_bytes = 0, allocation_failures = 0;
void Check(bool value, const char* why) {
  ++checks;
  if (!value) throw std::runtime_error(why);
}
struct Fake {
  static constexpr size_t PageCount = 32;
  static constexpr uint32_t Base = Tracker::kFirst;
  std::vector<uint8_t> bytes = std::vector<uint8_t>(PageCount * Page);
  std::array<uintptr_t, 4> aliases{};
  std::array<std::array<Access, PageCount>, 4> access{};
  struct Call { unsigned alias; uint32_t first, count; Access access; };
  std::array<Call, 8192> protected_calls{};
  std::array<std::array<bool, PageCount>, 4> queried{};
  size_t protects = 0, queries = 0, fail_protect = 0, fail_through = 0;
  size_t fail_query = 0;
  bool partial_protect = false, throw_query = false;
  std::unique_ptr<Tracker> tracker;
  explicit Fake(bool batched_permissions, size_t limit = rex::memory::PhysicalSnapshotMetadata::kLimit) {
    for (size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = uint8_t((i * 73u) ^ (i >> 3) ^ (i >> 11) ^ 0x5au);
    aliases = {reinterpret_cast<uintptr_t>(bytes.data()) - Base,
               0x1000000000ull, 0x2000000000ull, 0x3000000000ull};
    for (auto& row : access) row.fill(Access::ReadWrite);
    Tracker::Host host{};
    host.context = this;
    host.query = [](void* context, uintptr_t p, size_t& extent, Access& out) {
      auto& f = *static_cast<Fake*>(context);
      ++f.queries;
      if (f.throw_query) throw std::runtime_error("injected permission query");
      if (f.queries == f.fail_query) return false;
      const auto [alias, first] = f.Locate(p);
      out = f.access[alias][first];
      extent = Page;
      while (first + extent / Page < PageCount && f.access[alias][first + extent / Page] == out)
        extent += Page;
      // Callback may return a suffix longer than the requested union run.
      // CaptureBatch must clamp it before selecting additional pages.
      f.queried[alias][first] = true;
      return true;
    };
    host.query_pages = [](void* context, std::span<const Tracker::PageQuery> requests) {
      auto& f = *static_cast<Fake*>(context);
      ++f.queries;
      if (f.throw_query) throw std::runtime_error("injected permission batch");
      for (const auto& request : requests) for (size_t i = 0; i < request.pages.size(); ++i) {
        const auto [alias, page] = f.Locate(request.address + i * Page);
        f.queried[alias][page] = true;
        request.pages[i] = f.access[alias][page];
        if (f.queries == f.fail_query) return false; // Deliberately partial output.
      }
      return true;
    };
    host.protect = [](void* context, uintptr_t p, size_t bytes, Access access) noexcept {
      auto& f = *static_cast<Fake*>(context);
      const auto [alias, first] = f.Locate(p);
      const auto count = uint32_t(bytes / Page);
      if (f.protects >= f.protected_calls.size() || first + count > PageCount) std::terminate();
      f.protected_calls[f.protects++] = {alias, first, count, access};
      if (f.protects == f.fail_protect ||
          (f.fail_through && f.protects >= f.fail_protect && f.protects <= f.fail_through)) {
        if (f.partial_protect) f.access[alias][first] = access;
        return false;
      }
      for (uint32_t i = 0; i < count; ++i) f.access[alias][first + i] = access;
      return true;
    };
    tracker = std::make_unique<Tracker>(host, aliases, limit);
    tracker->SetBatchQueriesEnabled(batched_permissions);
    tracker->Enable();
  }
  std::pair<unsigned, uint32_t> Locate(uintptr_t p) const noexcept {
    for (unsigned a = 0; a < aliases.size(); ++a)
      if (p >= aliases[a] + Base && p < aliases[a] + Base + bytes.size())
        return {a, uint32_t((p - aliases[a] - Base) / Page)};
    std::terminate();
  }
  uintptr_t Host(unsigned alias, uint32_t physical) const { return aliases[alias] + physical; }
  void Write(unsigned alias, uint32_t physical, uint8_t value) {
    const auto [a, page] = Locate(Host(alias, physical));
    if (access[a][page] != Access::ReadWrite)
      Check(tracker->HandleFault(Host(alias, physical), true) == Tracker::Fault::Handled,
            "owned alias write failed exact fault repair");
    bytes[physical - Base] = value;
  }
  void Bytes(const Token& token, uint32_t address, uint32_t length) const {
    Check(token && token->bytes() && token->bytes()->size() == length, "wrong independent view extent");
    for (size_t i = 0; i < length; ++i) {
      ++oracle_bytes;
      Check(token->bytes()->at(i) == bytes[address - Base + i], "per-byte physical oracle mismatch");
    }
  }
};

void Differential(bool permission_batch) {
  Fake scalar(permission_batch), batch(permission_batch);
  std::array<Request, 5> requests{{
    {Fake::Base + 31, Page + 19, {}, Capture},
    {Fake::Base + 9 * Page + 317, 2 * Page + 73, {}, Capture},
    {Fake::Base + 13 * Page + 99, Page + 3, {}, Capture},
    {Fake::Base + 9 * Page + 317, 2 * Page + 73, {}, Capture},
    {Fake::Base + 10 * Page - 5, Page + 11, {}, Capture}}};
  std::array<Token, 5> original_scalar{};
  for (size_t i = 0; i < requests.size(); ++i)
    original_scalar[i] = scalar.tracker->Capture(requests[i].address, requests[i].length, {});
  auto original_batch = batch.tracker->CaptureBatch(requests);
  Check(original_batch.size() == requests.size(), "cold batch lost indexed outputs");
  for (size_t i = 0; i < requests.size(); ++i) {
    batch.Bytes(original_batch[i], requests[i].address, requests[i].length);
    Check(*original_batch[i]->bytes() == *original_scalar[i]->bytes(), "scalar/batch payload difference");
    requests[i].previous = original_batch[i]; requests[i].mode = Validate;
  }
  const auto calls = batch.protects, queries = batch.queries;
  auto reused = batch.tracker->CaptureBatch(requests);
  for (size_t i = 0; i < requests.size(); ++i)
    Check(reused[i] == original_batch[i] && reused[i]->bytes() == original_batch[i]->bytes(),
          "valid batch changed original token or immutable owner");
  Check(batch.protects == calls && batch.queries == queries, "ValidateOnly touched cold host APIs");
  // All aliases mutate the same physical storage. Each old immutable token
  // remains fixed; only the intersected original-sized views lose validity.
  for (unsigned alias = 0; alias < 4; ++alias) {
    const auto changed = Fake::Base + 10 * Page + 7 + alias;
    batch.Write(alias, changed, uint8_t(0xe0 + alias));
    scalar.Write(alias, changed, uint8_t(0xe0 + alias));
    const auto before = batch.tracker->stats();
    const auto invalid_calls = batch.protects, invalid_queries = batch.queries;
    auto invalid = batch.tracker->CaptureBatch(requests);
    Check(invalid[0] == requests[0].previous && invalid[2] == requests[2].previous,
          "invalid active member revoked independent valid neighbor");
    for (size_t i : {size_t(1), size_t(3), size_t(4)}) Check(!invalid[i], "invalid active member rearmed");
    Check(batch.tracker->stats().captures == before.captures && batch.protects == invalid_calls && batch.queries == invalid_queries,
          "invalid ValidateOnly captured or protected");
    for (auto& request : requests) request.mode = Capture;
    auto next = batch.tracker->CaptureBatch(requests);
    for (size_t i = 0; i < requests.size(); ++i) {
      auto scalar_next = scalar.tracker->Capture(requests[i].address, requests[i].length, original_scalar[i]);
      batch.Bytes(next[i], requests[i].address, requests[i].length);
      Check(*next[i]->bytes() == *scalar_next->bytes(), "all-alias scalar/batch differential mismatch");
      original_scalar[i] = scalar_next;
      requests[i].previous = next[i]; requests[i].mode = Validate;
    }
    // Refresh absolute host baselines after the legitimate explicit captures.
    // A later invalid member is still never captured by ValidateOnly.
    for (size_t i = 0; i < original_batch.size(); ++i) for (size_t byte = 0; byte < requests[i].length; ++byte) {
      const auto position = requests[i].address - Fake::Base + byte;
      ++oracle_bytes;
      Check(original_batch[i]->bytes()->at(byte) == uint8_t((position * 73u) ^
            (position >> 3) ^ (position >> 11) ^ 0x5au), "old immutable payload changed");
    }
  }
  const auto stats = batch.tracker->stats();
  for (auto faults : stats.alias_faults) Check(faults == 1, "missing exact all-alias invalidation witness");
}

void NoncontiguousUnion(bool permission_batch) {
  Fake f(permission_batch);
  f.access[0][5] = Access::NoAccess; // Gap must never become part of either view.
  f.access[2][1] = Access::ReadOnly; f.access[3][10] = Access::NoAccess;
  const auto original = f.access;
  const std::array<Request, 3> requests{{
    {Fake::Base + Page + 13, 700, {}, Capture},
    {Fake::Base + 10 * Page + 23, 811, {}, Capture},
    {Fake::Base + Page + 131, 411, {}, Capture}}};
  auto output = f.tracker->CaptureBatch(requests);
  Check(output.size() == 3, "gap batch lost outputs");
  for (size_t i = 0; i < requests.size(); ++i) f.Bytes(output[i], requests[i].address, requests[i].length);
  for (size_t i = 0; i < f.protects; ++i) for (uint32_t page = f.protected_calls[i].first;
       page < f.protected_calls[i].first + f.protected_calls[i].count; ++page)
    Check(page == 1 || page == 10, "batch protected a noncontiguous gap");
  for (unsigned alias = 0; alias < 4; ++alias) for (uint32_t page = 0; page < Fake::PageCount; ++page)
    if (page != 1 && page != 10) Check(!f.queried[alias][page], "batch queried an unrequested gap page");
  Check(!f.tracker->OwnsFault(f.Host(2, requests[0].address), true) &&
        !f.tracker->OwnsFault(f.Host(3, requests[1].address), true), "genuine permissions became repair-owned");
  Check(f.tracker->Disable() && f.access == original, "disable relaxed a genuine restriction or left owned pages");
}

void PreflightFailures(bool permission_batch) {
  for (unsigned kind = 0; kind < 4; ++kind) {
    Fake f(permission_batch);
    auto valid = f.tracker->Capture(Fake::Base + Page, Page, {});
    const std::array<Request, 2> requests{{{Fake::Base + Page, Page, valid, Validate},
      {Fake::Base + 10 * Page, Page, {}, Capture}}};
    const auto permissions = f.access;
    const auto protects = f.protects;
    const auto metadata = f.tracker->stats().metadata_bytes;
    if (kind == 0) f.fail_query = f.queries + 1;
    if (kind == 1) f.throw_query = true;
    if (kind == 2) f.access[0][10] = Access::NoAccess;
    if (kind == 3) f.access[3][10] = Access::Unsupported;
    const auto failure_permissions = f.access;
    bool caught = false; std::vector<Token> output;
    try { output = f.tracker->CaptureBatch(requests); }
    catch (const std::runtime_error&) { caught = true; }
    Check(caught == (kind == 1), "query failure propagation changed");
    if (!caught) Check(output.size() == 2 && output[0] == valid && !output[1],
                      "failed preflight lost valid neighbor or published cold token");
    Check(f.protects == protects && f.access == failure_permissions, "preflight failure changed restrictions");
    Check(f.tracker->TryReuse(requests[0].address, Page, valid) == valid,
          "preflight failure invalidated unchanged neighbor");
    Check(f.tracker->stats().metadata_bytes == metadata, "failed preflight leaked metadata");
    f.fail_query = 0; f.throw_query = false; f.access = permissions;
    Check(f.tracker->CaptureBatch(requests)[1] != nullptr, "fresh capture after failed preflight refused");
  }
}

void ProtectionFailures(bool permission_batch) {
  for (unsigned failing_alias = 0; failing_alias < 4; ++failing_alias) {
    Fake f(permission_batch);
    auto old = f.tracker->Capture(Fake::Base + Page, Page, {});
    const std::array<Request, 2> requests{{{Fake::Base + Page, Page, old, Validate},
      {Fake::Base + 10 * Page, Page, {}, Capture}}};
    const auto metadata = f.tracker->stats().metadata_bytes;
    f.partial_protect = true; f.fail_protect = f.protects + failing_alias + 1;
    auto output = f.tracker->CaptureBatch(requests);
    Check(output.size() == 2 && !output[0] && !output[1], "partial protection failure published outputs");
    Check(f.tracker->stats().metadata_bytes == metadata, "partial protection failure leaked metadata");
    Check(f.tracker->TryReuse(requests[0].address, Page, old) == old,
          "disjoint old neighbor unexpectedly lost token validity");
    for (unsigned alias = 0; alias < 4; ++alias)
      Check(f.access[alias][10] == Access::ReadWrite, "partially armed cold page was not repaired");
    f.fail_protect = 0;
    Check(bool(f.tracker->CaptureBatch(requests)[1]), "protection rollback prevented later readmission");
  }
  {
    // The cold view partially overlaps a validated token. A failed protect on
    // its additional page must revoke the old token through the UNION, while
    // preserving repair ownership if OS rollback also fails.
    Fake overlap(permission_batch);
    auto old = overlap.tracker->Capture(Fake::Base + Page, Page, {});
    const std::array<Request, 2> requests{{{Fake::Base + Page, Page, old, Validate},
      {Fake::Base + Page + 31, 2 * Page, {}, Capture}}};
    overlap.partial_protect = true; overlap.fail_protect = overlap.protects + 1;
    auto output = overlap.tracker->CaptureBatch(requests);
    Check(output.size() == 2 && !output[0] && !output[1], "overlapping partial failure returned accepted outputs");
    Check(!overlap.tracker->TryReuse(requests[0].address, Page, old), "overlapping union failure retained stale active token");
    overlap.fail_protect = 0;
    auto cold = requests; cold[0].mode = Capture;
    auto repaired = overlap.tracker->CaptureBatch(cold);
    for (size_t i = 0; i < cold.size(); ++i) overlap.Bytes(repaired[i], cold[i].address, cold[i].length);
  }
  Fake repair(permission_batch);
  const std::array<Request, 1> request{{{Fake::Base + Page, 2 * Page, {}, Capture}}};
  repair.partial_protect = true; repair.fail_protect = 2; repair.fail_through = 10;
  auto refused = repair.tracker->CaptureBatch(request);
  Check(refused.size() == 1 && !refused[0], "failed arm and repair minted token");
  Check(repair.tracker->OwnsFault(repair.Host(0, request[0].address), true),
        "failed rollback dropped repair ownership");
  repair.fail_protect = repair.fail_through = 0;
  Check(repair.tracker->HandleFault(repair.Host(0, request[0].address), true) == Tracker::Fault::Handled,
        "retained repair ownership did not repair later fault");
  auto fresh = repair.tracker->CaptureBatch(request);
  repair.Bytes(fresh[0], request[0].address, request[0].length);
  repair.Write(3, request[0].address + 33, 0xb7);
  Check(!repair.tracker->TryReuse(request[0].address, request[0].length, fresh[0]),
        "repair-only ownership incorrectly certified a later mutation");
}

void AllocationFailures(bool permission_batch) {
  bool completed = false;
  size_t local_failures = 0;
  for (size_t failure = 0; failure < 96; ++failure) {
    Fake f(permission_batch);
    auto valid = f.tracker->Capture(Fake::Base + Page, Page, {});
    const std::array<Request, 3> requests{{{Fake::Base + Page, Page, valid, Validate},
      {Fake::Base + 10 * Page + 19, Page + 17, {}, Capture},
      {Fake::Base + 20 * Page + 37, 2 * Page + 9, {}, Capture}}};
    const auto permissions = f.access;
    const auto protects = f.protects;
    const auto metadata = f.tracker->stats().metadata_bytes;
    std::vector<Token> output; bool failed = false;
    snapshot_batch_fixture::ArmAllocationFailure(failure);
    try { output = f.tracker->CaptureBatch(requests); }
    catch (const std::bad_alloc&) { failed = true; }
    catch (...) { snapshot_batch_fixture::DisarmAllocationFailure(); throw; }
    snapshot_batch_fixture::DisarmAllocationFailure();
    if (!failed) {
      Check(output.size() == 3 && output[0] == valid && output[1] && output[2], "allocation sweep completion missing");
      completed = true; break;
    }
    ++allocation_failures;
    ++local_failures;
    Check(snapshot_batch_fixture::reached_allocations == failure + 1, "allocation failure gate was not reached exactly");
    Check(f.protects == protects && f.access == permissions, "allocation failure occurred after protection started");
    Check(f.tracker->stats().metadata_bytes == metadata, "allocation failure leaked partially prepared metadata");
    Check(f.tracker->TryReuse(requests[0].address, Page, valid) == valid,
          "allocation failure revoked already valid neighbor");
  }
  Check(completed && local_failures >= 10, "allocation sweep did not reach cold preparation failures");
}

void WritersAndLifecycle(bool permission_batch) {
  Fake f(permission_batch);
  const std::array<Request, 2> cold{{{Fake::Base + Page, Page + 13, {}, Capture},
    {Fake::Base + 10 * Page, Page, {}, Capture}}};
  auto tokens = f.tracker->CaptureBatch(cold);
  auto requests = cold;
  for (size_t i = 0; i < requests.size(); ++i) {requests[i].previous = tokens[i]; requests[i].mode = Validate;}
  for (unsigned alias = 0; alias < 4; ++alias) {
    Check(f.tracker->BeginExternalWrite(f.Host(alias, cold[0].address + 17), 19), "external writer admission");
    Check(f.tracker->BeginExternalWrite(f.Host(alias, cold[0].address + 17), 19), "duplicate writer admission");
    auto during = f.tracker->CaptureBatch(requests);
    Check(!during[0] && during[1] == requests[1].previous, "active external writer batch validity");
    auto attempted_capture = requests; attempted_capture[0].mode = Capture;
    auto rejected_capture = f.tracker->CaptureBatch(attempted_capture);
    Check(!rejected_capture[0] && rejected_capture[1] == requests[1].previous,
          "cold batch captured active external writer");
    f.tracker->EndExternalWrite(f.Host(alias, cold[0].address + 17), 19);
    Check(!f.tracker->CaptureBatch(requests)[0], "one remaining writer accepted token");
    f.bytes[cold[0].address - Fake::Base + 17] = uint8_t(0xa0 + alias);
    f.tracker->EndExternalWrite(f.Host(alias, cold[0].address + 17), 19);
    Check(!f.tracker->CaptureBatch(requests)[0], "writer retirement rearmed invalid active token");
    requests[0].mode = Capture;
    requests[0].previous = f.tracker->CaptureBatch(requests)[0]; requests[0].mode = Validate;
    f.Bytes(requests[0].previous, cold[0].address, cold[0].length);
  }
  auto survivor = requests[0].previous;
  auto owner = survivor->bytes(); const auto bytes = *owner;
  Check(f.tracker->BeforeHostMutation(f.Host(2, cold[0].address), Page), "host mutation rollback");
  Check(!f.tracker->CaptureBatch(requests)[0], "host mutation did not invalidate active token");
  f.tracker->RevokeAll();
  Check(!f.tracker->CaptureBatch(requests)[1], "global epoch revocation survived batch validation");
  Check(f.tracker->Disable(), "batch lifecycle disable failed");
  f.tracker->Enable();
  Check(!f.tracker->CaptureBatch(requests)[0], "disable/enable reaccepted old token");
  f.tracker.reset();
  Check(*owner == bytes && *survivor->bytes() == bytes, "token/bytes did not outlive tracker");
  Fake replacement(permission_batch);
  Check(!replacement.tracker->CaptureBatch(requests)[0], "replacement Memory identity reused old token");
  survivor.reset(); owner.reset();
}

void IndependentTokenOwnership(bool permission_batch) {
  Fake f(permission_batch);
  const std::array<Request, 3> requests{{{Fake::Base + Page + 19, 1031, {}, Capture},
    {Fake::Base + 10 * Page + 37, 2053, {}, Capture},
    {Fake::Base + 20 * Page + 59, 3079, {}, Capture}}};
  auto output = f.tracker->CaptureBatch(requests);
  Check(output.size() == 3, "ownership batch missing output");
  std::weak_ptr<const rex::memory::CertifiedPhysicalSnapshot> discarded = output[1];
  std::weak_ptr<const std::vector<uint8_t>> discarded_bytes = output[1]->bytes();
  auto retained = output[0];
  auto retained_owner = retained->bytes();
  const auto retained_bytes = *retained_owner;
  const auto charge = f.tracker->stats().metadata_bytes;
  output[1].reset();
  Check(discarded.expired() && discarded_bytes.expired(), "one token retained unrelated batch cohort");
  Check(f.tracker->stats().metadata_bytes < charge, "independent token metadata did not release immediately");
  output.clear();
  Check(f.tracker->stats().metadata_bytes > 0 && *retained_owner == retained_bytes,
        "releasing batch outputs invalidated retained independent owner");
  retained.reset();
  Check(f.tracker->stats().metadata_bytes == 0 && *retained_owner == retained_bytes,
        "byte-only owner retained token metadata/cohort or changed payload");
  Check(f.tracker->Disable(), "ownership fixture shutdown repair failed");
  f.tracker.reset();
  Check(*retained_owner == retained_bytes, "independent original-sized bytes failed SDK tracker destruction");
}

void BoundsAndBudget(bool permission_batch) {
  Fake f(permission_batch);
  const auto before = f.tracker->stats();
  Check(f.tracker->CaptureBatch({}).empty(), "empty batch admitted");
  std::array<Request, 65> too_many{};
  for (auto& request : too_many) request = {Fake::Base, 1, {}, Capture};
  Check(f.tracker->CaptureBatch(too_many).empty(), "65 views admitted");
  auto exact = f.tracker->CaptureBatch(std::span<const Request>(too_many).first(64));
  Check(exact.size() == 64 && std::all_of(exact.begin(), exact.end(), [](const auto& token){return bool(token);}),
        "exact64 request bound refused");
  for (auto& request : too_many) request.length = uint32_t(f.bytes.size());
  auto byte_bound = f.tracker->CaptureBatch(std::span<const Request>(too_many).first(64));
  Check(byte_bound.size() == 64 && byte_bound.front() && byte_bound.back(), "exact8MiB summed bound refused");
  ++too_many[0].length;
  Check(f.tracker->CaptureBatch(std::span<const Request>(too_many).first(64)).empty(), "summed bound exceeded");
  const std::array<Request, 4> invalid{{{0x1000, Page, {}, Capture},
    {Tracker::kEnd - 1, 2, {}, Capture}, {UINT32_MAX - 17, 99, {}, Capture},
    {Fake::Base, 0, {}, Capture}}};
  auto refused = f.tracker->CaptureBatch(invalid);
  Check(refused.size() == 4 && std::none_of(refused.begin(), refused.end(), [](const auto& token){return bool(token);}),
        "invalid view bounds minted token");
  Check(f.tracker->stats().snapshot_batches > before.snapshot_batches, "batch route was not actually reached");
  Fake limited(permission_batch, 512);
  std::array<Request, 8> requests{};
  for (size_t i = 0; i < requests.size(); ++i) requests[i] = {Fake::Base + uint32_t(i) * Page, Page, {}, Capture};
  auto held = limited.tracker->CaptureBatch(requests);
  Check(std::any_of(held.begin(), held.end(), [](const auto& token){return bool(token);}) &&
        std::any_of(held.begin(), held.end(), [](const auto& token){return !token;}), "metadata limit did not refuse individually");
  Check(limited.tracker->stats().metadata_bytes <= 512 && limited.tracker->stats().peak_metadata_bytes <= 512,
        "metadata retained/peak exceeded limit");
  held.clear();
  Check(limited.tracker->stats().metadata_bytes == 0, "independent token metadata lifetime leaked");
  Check(bool(limited.tracker->CaptureBatch(std::span<const Request>(requests).first(1))[0]),
        "metadata release did not permit readmission");
}
}
int main() try {
  static_assert(!std::is_copy_constructible_v<Tracker> && !std::is_move_constructible_v<Tracker>);
  for (bool permissions : {false, true}) {
    Differential(permissions); NoncontiguousUnion(permissions); PreflightFailures(permissions);
    ProtectionFailures(permissions); AllocationFailures(permissions); WritersAndLifecycle(permissions);
    IndependentTokenOwnership(permissions); BoundsAndBudget(permissions);
  }
  std::cout << "PASS: batch tracker independent checks=" << checks << " per-byte oracle=" << oracle_bytes
            << " reached allocation failures=" << allocation_failures
            << "; scalar/batch, four aliases, exact owners, gaps, invalid active no-rearm, preflight/partial repair, writers and lifetime\n";
} catch (const std::exception& error) {
  snapshot_batch_fixture::DisarmAllocationFailure();
  std::cerr << error.what() << '\n'; return 1;
}
