#pragma once
#include "gears_bridge.h"
#include <array>
#include <bit>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gow3::renderer {
// Byte buffers of captured boundary blocks, returned by the planner once it
// has copied what it keeps, for the guest render thread to reuse. Every draw
// captures around a hundred blocks; allocating them on the render thread and
// freeing them on the planner's cost ~12% of the render thread in the
// allocator. Buffers are grouped by capacity class (bit width), so a request
// takes a buffer that already fits.
class BoundaryBufferRecycler {
 public:
  static constexpr size_t kMaximumBytes = 64 * 1024;  // Larger blocks are freed.
  static constexpr size_t kClasses = std::bit_width(kMaximumBytes) + 1;
  static constexpr size_t kPerClass = 4096;
  static constexpr size_t kBlockVectors = 256;

  // Planner thread, after an event has been planned. Takes the event's
  // buffers and its (emptied) block vector; the event is left without blocks.
  void Return(std::vector<GraphicsBoundaryBlock>& blocks) noexcept {
    try {
      std::lock_guard lock(mutex_);
      for (auto& block : blocks) {
        auto& bytes = block.bytes;
        const size_t capacity = bytes.capacity();
        if (!capacity || capacity > kMaximumBytes) continue;
        auto& bucket = shared_[std::bit_width(capacity)];
        if (bucket.size() >= kPerClass) continue;
        bytes.clear();
        bucket.push_back(std::move(bytes));
      }
      blocks.clear();
      if (blocks.capacity() && block_vectors_.size() < kBlockVectors) block_vectors_.push_back(std::move(blocks));
    } catch (...) {
    }
  }

  // Render thread, once per event: moves everything returned so far into the
  // caller's local stash (one lock), and hands out an empty block vector.
  struct Stash {
    std::array<std::vector<std::vector<uint8_t>>, kClasses> buffers;
    std::vector<std::vector<GraphicsBoundaryBlock>> block_vectors;
    // A buffer with capacity >= bytes, or an empty vector.
    std::vector<uint8_t> Take(size_t bytes) {
      if (!bytes || bytes > kMaximumBytes) return {};
      // Class c holds capacities in [2^(c-1), 2^c): the first class whose
      // lower bound is at least `bytes` always fits.
      for (size_t c = std::bit_width(bytes - 1) + 1, last = std::min(c + 2, kClasses - 1); c <= last; ++c) {
        auto& bucket = buffers[c];
        if (bucket.empty()) continue;
        auto out = std::move(bucket.back());
        bucket.pop_back();
        return out;
      }
      return {};
    }
  };
  void Refill(Stash& stash) noexcept {
    try {
      std::lock_guard lock(mutex_);
      for (size_t c = 0; c < kClasses; ++c) {
        auto& from = shared_[c];
        auto& to = stash.buffers[c];
        while (!from.empty() && to.size() < kPerClass) {
          to.push_back(std::move(from.back()));
          from.pop_back();
        }
        from.clear();
      }
      while (!block_vectors_.empty() && stash.block_vectors.size() < kBlockVectors) {
        stash.block_vectors.push_back(std::move(block_vectors_.back()));
        block_vectors_.pop_back();
      }
    } catch (...) {
    }
  }

 private:
  std::mutex mutex_;
  std::array<std::vector<std::vector<uint8_t>>, kClasses> shared_;
  std::vector<std::vector<GraphicsBoundaryBlock>> block_vectors_;
};

// The process-wide recycler shared by the bridge and the planner.
BoundaryBufferRecycler& GraphicsBoundaryBufferRecycler();
}  // namespace gow3::renderer
