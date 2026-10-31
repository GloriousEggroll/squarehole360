#include "gow3_tmap_find.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}

struct Memory {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(1 << 20);
  void Put(uint32_t address, uint32_t value) {
    const uint32_t big = std::byteswap(value);
    std::memcpy(bytes.data() + address, &big, 4);
  }
  uint32_t Raw(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, bytes.data() + address, 4);
    return value;
  }
  uint32_t Get(uint32_t address) const { return std::byteswap(Raw(address)); }
};

// The game's loop (0x822AC700) transcribed instruction by instruction.
uint32_t Reference(const Memory& m, uint32_t map, uint32_t key) {
  const uint32_t count = m.Get(map + 56);
  if (!count) return UINT32_MAX;
  const uint32_t hash = m.Get(map + 52);
  const uint32_t mask = hash == 0 ? 0xFFFFFFFFu : 0;
  const uint32_t slot = ((mask & (map + 48)) | hash) + (((count - 1) & key) << 2);
  uint32_t entry = m.Get(slot);
  if (entry == UINT32_MAX) return UINT32_MAX;
  const uint32_t elements = m.Get(map);
  for (;;) {
    const uint32_t record = (entry << 4) + elements;
    if (m.Get(record) == key) return entry;
    entry = m.Get(record + 8);
    if (entry == UINT32_MAX) return UINT32_MAX;
  }
}

constexpr uint32_t kMap = 0x100, kHash = 0x1000, kElements = 0x10000, kBits = 0x8000;

// Allocation bit of an element, as the game's add (0x827A90F0) sets it.
uint32_t BitsAddress(const Memory& m) { return m.Get(kMap + 28) ? m.Get(kMap + 28) : kMap + 12; }
void SetAllocated(Memory& m, uint32_t index, bool allocated) {
  const uint32_t address = BitsAddress(m) + (index >> 5) * 4;
  const uint32_t bit = 1u << (index & 31);
  m.Put(address, allocated ? m.Get(address) | bit : m.Get(address) & ~bit);
}

// Builds a map of `count` elements over `buckets` buckets. Keys are aligned
// like texture addresses, or random; insertion links each new element at its
// bucket's head; removals unlink elements, leaving gaps, and their slots may
// be reused by later insertions as a sparse array's free list does.
std::vector<uint32_t> Build(Memory& m, std::mt19937& random, uint32_t buckets, uint32_t count,
                            bool aligned, bool inline_hash) {
  m.Put(kMap + 56, buckets);
  m.Put(kMap + 52, inline_hash ? 0 : kHash);
  m.Put(kMap, kElements);
  m.Put(kMap + 4, count);
  // Few elements keep their allocation bits inline, as the bit array does.
  m.Put(kMap + 28, count > 128 || random() % 2 ? kBits : 0);
  const uint32_t hash = inline_hash ? kMap + 48 : kHash;
  for (uint32_t b = 0; b < buckets; ++b) m.Put(hash + b * 4, UINT32_MAX);
  std::vector<uint32_t> keys, slots(count);
  std::iota(slots.begin(), slots.end(), 0u);
  if (random() % 2) std::shuffle(slots.begin(), slots.end(), random);
  const auto link = [&](uint32_t index, uint32_t key) {
    const uint32_t bucket = hash + ((buckets - 1) & key) * 4;
    m.Put(kElements + index * 16, key);
    m.Put(kElements + index * 16 + 4, ~key);
    m.Put(kElements + index * 16 + 8, m.Get(bucket));
    m.Put(kElements + index * 16 + 12, (buckets - 1) & key);
    m.Put(bucket, index);
    SetAllocated(m, index, true);
  };
  const auto make_key = [&](uint32_t i) {
    return aligned ? 0x40000000u + i * 0x1000u : uint32_t(random());
  };
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t key = make_key(i);
    keys.push_back(key);
    link(slots[i], key);
  }
  // Unlink a few elements, then reuse their slots for new keys.
  std::vector<uint32_t> freed;
  for (uint32_t r = 0, removals = count ? random() % (count / 4 + 1) : 0; r < removals; ++r) {
    const uint32_t victim = slots[random() % count];
    const uint32_t key = m.Get(kElements + victim * 16);
    uint32_t at = hash + ((buckets - 1) & key) * 4;
    bool found = false;
    for (uint32_t e = m.Get(at); e != UINT32_MAX; at = kElements + e * 16 + 8, e = m.Get(at))
      if (e == victim) { m.Put(at, m.Get(kElements + e * 16 + 8)); found = true; break; }
    if (!found) continue;
    // The free-list link overwrites the key; sometimes with a value that
    // equals a live key, which only the allocation bit tells apart.
    m.Put(kElements + victim * 16, random() % 4 == 0 && !keys.empty() ? keys[random() % keys.size()] : uint32_t(random() % count));
    SetAllocated(m, victim, false);
    freed.push_back(victim);
  }
  for (uint32_t i = 0; i < freed.size(); i += 2) {
    const uint32_t key = make_key(count + i);
    keys.push_back(key);
    link(freed[i], key);
  }
  return keys;
}
}  // namespace

int main() try {
  std::mt19937 random(11);
  uint64_t checks = 0;
  const auto raw = [](const Memory& m) { return [&m](uint32_t address) { return m.Raw(address); }; };
  for (int round = 0; round < 400; ++round) {
    Memory m;
    const bool inline_hash = round % 7 == 0;
    const uint32_t buckets = inline_hash ? 1 : 1u << (random() % 12);
    const uint32_t count = random() % 2200;
    const bool aligned = round % 3 != 2;
    const auto keys = Build(m, random, buckets, count, aligned, inline_hash);
    std::vector<uint32_t> probes = keys;
    for (int i = 0; i < 64; ++i) probes.push_back(uint32_t(random()));
    for (int i = 0; i < 64; ++i) probes.push_back(0x40000000u + uint32_t(random() % 4096) * 0x1000u);
    for (int i = 0; i < 16; ++i) probes.push_back(uint32_t(random() % (count + 1)));
    for (const uint32_t key : probes) {
      const uint32_t expected = Reference(m, kMap, key);
      Check(gow3::TMapIntFind(raw(m), kMap, key) == expected, "Lookup differs from the game's walk");
      // A bounded walk either gives the same answer or defers.
      const uint64_t bounded = gow3::TMapIntFind(raw(m), kMap, key, 32);
      Check(bounded == expected || bounded == gow3::kTMapUndecided, "Bounded walk gave a different answer");
      const uint64_t scanned = gow3::TMapIntFindByScan(raw(m), kMap, key);
      Check(scanned != gow3::kTMapUndecided && scanned == expected, "Scan differs from the game's walk");
      ++checks;
    }
  }
  // An empty map and an empty bucket return none.
  Memory empty;
  Check(gow3::TMapIntFind(raw(empty), kMap, 5) == UINT32_MAX, "Empty map found an element");
  {
    // A chain longer than the bound defers; one within it answers.
    Memory chain;
    std::mt19937 local(5);
    Build(chain, local, 1, 100, true, false);
    const uint32_t newest = chain.Get(kHash);  // head of the single chain
    const uint32_t oldest_key = 0x40000000u;   // inserted first, far down the chain
    Check(gow3::TMapIntFind(raw(chain), kMap, chain.Get(kElements + newest * 16), 32) == newest,
          "Bounded walk missed the chain head");
    Check(gow3::TMapIntFind(raw(chain), kMap, oldest_key, 32) == gow3::kTMapUndecided ||
              Reference(chain, kMap, oldest_key) == UINT32_MAX,
          "Bounded walk did not defer a long chain");
  }
  Check(gow3::TMapIntFindByScan(raw(empty), kMap, 5) == gow3::kTMapUndecided, "Unhashed map was answered by the scan");
  // Two live elements with one key (not a map): the scan defers to the walk.
  {
    Memory twice;
    std::mt19937 local(3);
    Build(twice, local, 4, 8, true, false);
    const uint32_t key = twice.Get(kElements + 3 * 16);
    twice.Put(kElements + 5 * 16, key);
    Check(gow3::TMapIntFindByScan(raw(twice), kMap, key) == gow3::kTMapUndecided, "Duplicate keys were not deferred");
  }
  std::cout << "TMap lookup matched the game's walk for " << checks << " probes\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
