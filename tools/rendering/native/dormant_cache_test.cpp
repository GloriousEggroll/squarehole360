#include "renderer/native/dormant_cache.h"

#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using gow3::renderer::DormantCache;
using gow3::renderer::DormantCacheEntry;
namespace {
void Check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
struct Resource { uint32_t identity; bool pending = false; };
struct Entry {
  std::shared_ptr<Resource> resource;
  DormantCacheEntry residency;
};
using Entries = std::map<uint32_t, Entry>;
struct Fixture {
  DormantCache policy;
  Entries entries;
  uint64_t removed = 0;
  explicit Fixture(DormantCache::Limits limits, uint64_t first = 1) : policy(limits, first) {}
  std::weak_ptr<Resource> Touch(uint32_t identity, uint64_t bytes = 16) {
    auto [it, fresh] = entries.try_emplace(identity);
    policy.Access(it->second.residency, !fresh);
    if (fresh) {
      it->second.resource = std::make_shared<Resource>(Resource{identity, false});
      policy.Created(it->second.residency, bytes);
    }
    it->second.resource->pending = true;
    return it->second.resource;
  }
  void BeforeErase(const Entry& entry) {
    Check(!entry.resource->pending, "Cache retired a pending/preparing resource");
    ++removed;
  }
  void End() {
    // This is the external existing frame fence; the policy itself never waits.
    for (auto& [key, entry] : entries) entry.resource->pending = false;
    policy.EndFrame(entries, [&](const Entry& entry) { BeforeErase(entry); });
    const auto used = policy.occupancy(entries);
    Check(used.dormant_entries <= policy.limits().entries &&
          used.dormant_bytes <= policy.limits().bytes, "Dormant budget exceeded");
    uint64_t bytes = 0;
    for (const auto& [key, entry] : entries) bytes += entry.residency.allocation_bytes;
    Check(bytes == policy.statistics().resident_bytes, "Resident allocation accounting changed");
  }
};

void LifetimeAndBounds() {
  Fixture keep({32, 2, 120});
  const auto a = keep.Touch(1); keep.End(); keep.End();
  Check(!a.expired(), "One absent frame discarded a resident owner");
  Check(keep.Touch(1).lock() == a.lock() && keep.policy.statistics().dormant_hits == 1,
        "Unchanged dormant identity failed to reuse its owner");
  keep.End();
  Fixture zero({0, 0, 120});
  const auto discarded = zero.Touch(1); zero.End();
  Check(!discarded.expired(), "Zero dormant budget evicted the current working set");
  zero.End(); Check(discarded.expired(), "Zero dormant budget did not reproduce old eviction");

  Fixture pressure({32, 2, 120});
  auto first = pressure.Touch(1), second = pressure.Touch(2); pressure.End();
  auto third = pressure.Touch(3); pressure.End();
  auto fourth = pressure.Touch(4); pressure.End();
  Check(first.expired() && !second.expired() && !third.expired() && !fourth.expired(),
        "Byte/count pressure did not evict the least recent inactive resource");
  // Both used and preparing resources are protected without any policy fence.
  pressure.Touch(2);
  const auto current = pressure.Touch(5);
  Check(pressure.policy.EvictUnusedForBindings(pressure.entries,
        [&](const Entry& entry) { pressure.BeforeErase(entry); },
        [](const Entry&) { return true; }), "Binding pressure found no inactive resource");
  Check(!current.expired() && !second.expired() && third.expired(),
        "Binding pressure invalidated a current/preparing owner");
  Check(pressure.policy.statistics().binding_evictions == 1, "Binding eviction was not counted");
  Check(!pressure.policy.EvictUnusedForBindings(pressure.entries,
        [&](const Entry& entry) { pressure.BeforeErase(entry); },
        [](const Entry&) { return false; }), "Binding eviction discarded an image with no reclaimable binding");
  pressure.End();

  Fixture count_only({1024, 1, 120});
  const auto count_first = count_only.Touch(1), count_second = count_only.Touch(2);
  count_only.End(); count_only.End();
  Check(count_first.expired() && !count_second.expired(), "Entry limit did not act independently of bytes");
  Fixture bytes_only({16, 8, 120});
  const auto byte_first = bytes_only.Touch(1), byte_second = bytes_only.Touch(2);
  bytes_only.End(); bytes_only.End();
  Check(byte_first.expired() && !byte_second.expired(), "Byte limit did not act independently of entries");
  Fixture oversized({1, 1, 120});
  auto large = oversized.Touch(1, 4096); oversized.End();
  Check(!large.expired(), "Oversized active working set was rejected");
  oversized.End(); Check(large.expired(), "Oversized dormant resource escaped its byte bound");
  Fixture age({4096, 8, 2});
  const auto aged = age.Touch(1); age.End(); age.End(); age.End();
  Check(!aged.expired(), "Idle age retired an owner too early");
  age.End(); Check(aged.expired() && age.policy.statistics().age_evictions == 1,
                  "Idle epoch limit failed to retire an owner");
}

void WrapAndFailure() {
  Fixture wrap({4096, 8, 120}, UINT64_MAX - 1);
  const auto old = wrap.Touch(1); wrap.End();
  const auto recent = wrap.Touch(2); wrap.End();
  Check(old.expired() && !recent.expired() && wrap.policy.statistics().wrap_evictions == 1,
        "Epoch wrap resurrected an inactive entry or lost the recent working set");
  wrap.End();
  Check(wrap.Touch(2).lock() == recent.lock(), "Wrapped epoch failed exact owner reuse");
  wrap.End();
  Fixture failure({0, 0, 120});
  auto retained = failure.Touch(1); failure.End();
  const auto bytes = failure.policy.statistics().resident_bytes;
  bool threw = false;
  try {
    failure.policy.EndFrame(failure.entries, [](const Entry&) { throw std::runtime_error("Injected hook failure"); });
  } catch (const std::runtime_error&) { threw = true; }
  Check(threw && !retained.expired() && failure.policy.statistics().resident_bytes == bytes &&
        failure.policy.statistics().evictions == 0, "Failure changed unretired ownership/accounting");
  failure.End(); Check(retained.expired(), "Failed retirement could not retry safely");
  threw = false;
  try { DormantCache invalid({1, 1, 1}, 0); } catch (const std::invalid_argument&) { threw = true; }
  Check(threw, "Zero internal epoch was accepted");
}

void DiagnosticTextureEntryBudget() {
  constexpr uint64_t bytes_limit = 128u * 1024 * 1024;
  constexpr uint32_t shipping_entries = 512, diagnostic_entries = 2048;
  constexpr uint64_t small_bytes = 16u * 1024;
  Fixture shipping({bytes_limit, shipping_entries, 120});
  Fixture extended({bytes_limit, diagnostic_entries, 120});
  std::vector<std::weak_ptr<Resource>> shipping_owners, extended_owners;
  for (uint32_t key = 0; key < 1024; ++key) {
    shipping_owners.push_back(shipping.Touch(key, small_bytes));
    extended_owners.push_back(extended.Touch(key, small_bytes));
  }
  shipping.End(); extended.End();
  // Both budgets protect the complete current/preparing working set. Entry
  // limits govern only the extra owners absent after the original frame fence.
  for (uint32_t key = 0; key < 1024; ++key)
    Check(!shipping_owners[key].expired() && !extended_owners[key].expired(),
          "Entry budget discarded the current working set");
  shipping.End(); extended.End();
  for (uint32_t key = 0; key < 1024; ++key) {
    Check(shipping_owners[key].expired() == (key < shipping_entries),
          "Shipping entry budget or deterministic equal-age order changed");
    Check(!extended_owners[key].expired(), "2048 entry option failed to retain an eligible exact owner");
  }
  Check(shipping.policy.statistics().budget_evictions == shipping_entries &&
        extended.policy.statistics().budget_evictions == 0 &&
        shipping.policy.statistics().resident_bytes == shipping_entries * small_bytes &&
        extended.policy.statistics().resident_bytes == 1024 * small_bytes,
        "Entry-only pressure changed the unchanged byte budget or accounting");
  const auto creations = extended.policy.statistics().creations;
  Check(extended.Touch(0, small_bytes).lock() == extended_owners[0].lock() &&
        extended.policy.statistics().creations == creations &&
        extended.policy.statistics().dormant_hits == 1,
        "Diagnostic entry budget did not reuse the exact retained owner");
  extended.End();

  constexpr uint64_t full_bytes = bytes_limit / diagnostic_entries;
  Fixture capped({bytes_limit, diagnostic_entries, 120});
  std::vector<std::weak_ptr<Resource>> capped_owners;
  for (uint32_t key = 0; key <= diagnostic_entries; ++key)
    capped_owners.push_back(capped.Touch(key, full_bytes));
  capped.End();
  Check(!capped_owners[0].expired() && capped.entries.size() == diagnostic_entries + 1,
        "Hard dormant ceiling incorrectly capped the protected active working set");
  capped.End();
  Check(capped_owners[0].expired() && !capped_owners.back().expired() &&
        capped.entries.size() == diagnostic_entries &&
        capped.policy.statistics().resident_bytes == bytes_limit &&
        capped.policy.statistics().budget_evictions == 1,
        "2048 entry/128 MiB combined ceiling failed to retire its oldest inactive owner");
  capped.Touch(1, full_bytes);
  const auto preparing = capped.Touch(4096, 16);
  Check(capped.policy.EvictUnusedForBindings(capped.entries,
        [&](const Entry& entry) { capped.BeforeErase(entry); },
        [](const Entry&) { return true; }), "Expanded cache failed original binding pressure eviction");
  Check(capped_owners[2].expired() && !capped_owners[1].expired() && !preparing.expired() &&
        capped.policy.statistics().binding_evictions == 1,
        "Expanded entry budget retired a pending/preparing owner under binding pressure");
  capped.End();

  // A higher entry allowance still cannot retain extra byte residency.
  Fixture bytes_only({bytes_limit, diagnostic_entries, 120});
  const auto oldest = bytes_only.Touch(1, bytes_limit / 2 + 1);
  const auto newer = bytes_only.Touch(2, bytes_limit / 2 + 1);
  bytes_only.End(); bytes_only.End();
  Check(oldest.expired() && !newer.expired() && bytes_only.entries.size() == 1 &&
        bytes_only.policy.statistics().resident_bytes == bytes_limit / 2 + 1,
        "2048 entry option bypassed the unchanged 128 MiB dormant byte ceiling");
}

// Independent reference: maps contain immutable IDs and a vector of distinct
// frame-use sets. Rebuild expected retention from complete history each frame,
// rather than using production entry stamps or eviction helpers.
uint64_t ReferenceOracle() {
  constexpr uint64_t limit = 640;
  constexpr size_t count_limit = 11;
  constexpr uint32_t age_limit = 7;
  Fixture actual({limit, count_limit, age_limit});
  std::map<uint32_t, uint64_t> expected;
  std::vector<std::set<uint32_t>> history;
  std::mt19937 random(0x670cb88);
  uint64_t checks = 0;
  for (size_t frame = 0; frame < 700; ++frame) {
    auto& used = history.emplace_back();
    for (uint32_t i = 0, count = frame % 13 ? random() % 21 : 0; i < count; ++i) {
      const auto key = random() % 43;
      const uint64_t bytes = 16 + (key % 9) * 16;
      actual.Touch(key, bytes); actual.Touch(key, bytes);
      expected.try_emplace(key, bytes); used.insert(key);
    }
    std::vector<std::pair<size_t, uint32_t>> dormant;
    uint64_t bytes = 0;
    for (auto it = expected.begin(); it != expected.end();) {
      if (used.contains(it->first)) { ++it; continue; }
      size_t last = frame;
      while (last && !history[last].contains(it->first)) --last;
      if (frame - last > age_limit) { it = expected.erase(it); continue; }
      dormant.emplace_back(last, it->first); bytes += it->second; ++it;
    }
    std::sort(dormant.begin(), dormant.end());
    size_t retired = 0;
    while (dormant.size() - retired > count_limit || bytes > limit) {
      auto it = expected.find(dormant[retired++].second);
      bytes -= it->second; expected.erase(it);
    }
    actual.End();
    Check(actual.entries.size() == expected.size(), "History oracle resident count differs");
    for (const auto& [key, allocation] : expected) {
      Check(actual.entries.contains(key) &&
            actual.entries.at(key).residency.allocation_bytes == allocation,
            "History oracle resource identity or allocation differs");
      ++checks;
    }
  }
  return checks;
}
}  // namespace


// One-scan binding eviction erases exactly the victims, in the same order, that
// repeated single evictions would, and stops as soon as its condition holds.
uint64_t BatchedBindingEviction() {
  std::mt19937 random(7);
  uint64_t checks = 0;
  for (int round = 0; round < 200; ++round) {
    Fixture single({1u << 30, 4096, 1000}), batched({1u << 30, 4096, 1000});
    std::set<uint32_t> ineligible;
    const int frames = 2 + int(random() % 6);
    for (int frame = 0; frame < frames; ++frame) {
      for (int touch = 0, count = 1 + int(random() % 12); touch < count; ++touch) {
        const uint32_t identity = random() % 40;
        single.Touch(identity); batched.Touch(identity);
        if (random() % 5 == 0) ineligible.insert(identity);
      }
      if (frame + 1 < frames) { single.End(); batched.End(); }
    }
    const auto eligible = [&](const Entry& entry) { return !ineligible.count(entry.resource->identity); };
    std::vector<uint32_t> expected, actual;
    const size_t want = random() % 16;
    while (expected.size() < want && single.policy.EvictUnusedForBindings(single.entries,
           [&](const Entry& entry) { single.BeforeErase(entry); expected.push_back(entry.resource->identity); },
           eligible)) {}
    const auto erased = batched.policy.EvictUnusedForBindingsUntil(batched.entries,
        [&](const Entry& entry) { batched.BeforeErase(entry); actual.push_back(entry.resource->identity); },
        eligible, [&] { return actual.size() >= want; });
    Check(actual == expected && erased == actual.size(), "Batched binding eviction chose different victims");
    Check(single.policy.statistics().binding_evictions == batched.policy.statistics().binding_evictions,
          "Batched binding eviction miscounted");
    Check(single.entries.size() == batched.entries.size(), "Batched binding eviction left a different cache");
    ++checks;
  }
  return checks;
}

int main() try {
  LifetimeAndBounds(); WrapAndFailure(); DiagnosticTextureEntryBudget();
  const auto batched = BatchedBindingEviction();
  std::cout << "Dormant cache passed ownership, zero/byte/count/age bounds, preparing protection, wrap, "
               "failure retry, shipping512/diagnostic2048 texture entry bounds and "
            << ReferenceOracle() << " independent history checks, " << batched << " batched binding evictions\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
