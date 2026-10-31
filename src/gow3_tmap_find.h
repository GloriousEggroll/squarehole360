#pragma once

#include <bit>
#include <cstdint>

namespace gow3 {

// The game's TMap<int32,...> element lookup (0x822AC700, r4 = map, r5 = key):
// walk the key's hash chain and return the first element with the key, or
// UINT32_MAX. With max_steps, a chain longer than that returns kTMapUndecided
// instead. raw(address) returns the big-endian guest word at address
// unswapped.
inline constexpr uint64_t kTMapUndecided = UINT64_MAX;
template<class RawWord>
uint64_t TMapIntFind(RawWord raw, uint32_t map, uint32_t key, uint64_t max_steps = UINT64_MAX) {
  const auto word = [&](uint32_t address) { return std::byteswap(raw(address)); };
  const uint32_t buckets = word(map + 56);
  if (!buckets) return UINT32_MAX;
  // Hash storage is external, or inline at +48 for a single bucket.
  const uint32_t hash = word(map + 52);
  uint32_t entry = word((hash ? hash : map + 48) + ((buckets - 1) & key) * 4);
  const uint32_t elements = word(map), raw_key = std::byteswap(key);
  for (uint64_t steps = 0; entry != UINT32_MAX; ++steps) {
    if (steps == max_steps) return kTMapUndecided;
    const uint32_t record = elements + (entry << 4);
    if (raw(record) == raw_key) return entry;
    entry = word(record + 8);
  }
  return UINT32_MAX;
}

// The same lookup without the chain: a set's live elements (allocation bit
// set; the game's add, 0x827A90F0) are exactly the elements linked in its
// hash, and a map's keys are unique, so the walk returns the one live element
// whose key matches, or none. A freed element's first word is a free-list
// index, which its clear allocation bit excludes. The scan reads keys at a
// fixed stride, independent loads instead of one dependent load per element.
// Returns kTMapUndecided when the map is not hashed or two live elements
// match, for the caller to walk instead. A well-spread map answers faster by
// its chain, so callers scan only after a long walk.
//
// Map layout: elements +0, element count +4, allocation bits inline at +12 or
// at +28 when that is set, hash inline at +48 or at +52, hash size +56.
template<class RawWord>
uint64_t TMapIntFindByScan(RawWord raw, uint32_t map, uint32_t key) {
  const auto word = [&](uint32_t address) { return std::byteswap(raw(address)); };
  if (!word(map + 56)) return kTMapUndecided;
  const uint32_t elements = word(map), count = word(map + 4), raw_key = std::byteswap(key);
  const uint32_t bits_storage = word(map + 28), bits = bits_storage ? bits_storage : map + 12;
  uint32_t found = UINT32_MAX;
  const auto check = [&](uint32_t index) {
    if (raw(elements + (index << 4)) != raw_key) return true;
    if (!((word(bits + (index >> 5) * 4) >> (index & 31)) & 1)) return true;
    if (found != UINT32_MAX) return false;
    found = index;
    return true;
  };
  // Eight keys per test: a match is rare, so most groups take one branch.
  constexpr uint32_t kGroup = 8;
  uint32_t index = 0;
  for (; index + kGroup <= count; index += kGroup) {
    bool any = false;
    for (uint32_t i = 0; i < kGroup; ++i) any |= raw(elements + ((index + i) << 4)) == raw_key;
    if (!any) continue;
    for (uint32_t i = 0; i < kGroup; ++i)
      if (!check(index + i)) return kTMapUndecided;
  }
  for (; index < count; ++index)
    if (!check(index)) return kTMapUndecided;
  return found;
}

}  // namespace gow3
