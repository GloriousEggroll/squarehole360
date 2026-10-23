#pragma once
#include "gpu_batch_attribution.h"
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gow3::renderer {
// Diagnostic failure never transfers, releases, or waits on render ownership.
// A disabled recorder stays disabled until destruction after the normal drain.
struct NativeDrawTimestampHealth {
  bool disabled = false;
  uint64_t failures = 0;
  const char *reason = nullptr;
  void Disable(const char *why) noexcept {
    disabled = true;
    ++failures;
    if (!reason)
      reason = why;
  }
};
// Pure ownership/aggregation policy. A timestamp belongs to one existing
// DrawSlot generation, never to a logical occlusion query. Only its actual
// fence retires it.
class NativeDrawTimestampLedger {
public:
  static constexpr uint32_t kSlots = 128, kPeriod = 16, kPairs = 2048,
                            kAggregates = 2048, kRecent = 256, kSlowest = 32,
                            kFrames = 256, kFrameGroups = 512;
  struct Token {
    uint32_t slot;
    uint64_t generation;
  };
  enum class Phase : uint8_t { Free, Recording, Ended, Accepted };
  struct Slot {
    uint64_t generation = 0;
    Phase phase = Phase::Free;
    NativeGpuWorkTag tag;
  };
  struct Sample {
    uint64_t ordinal = 0, submission = 0, retired_steady_ns = 0;
    NativeGpuWorkTag tag;
    std::optional<NativeGpuTimestampInterval> interval;
  };
  struct Pair {
    std::string vertex, pixel;
  };
  struct Aggregate {
    NativeGpuWorkTag key;
    uint64_t samples = 0, available = 0, first_ordinal = 0, last_ordinal = 0;
    uint32_t first_frame = UINT32_MAX, last_frame = 0;
    double inclusive_ms = 0, maximum_ms = 0;
  };
  struct Frame {
    uint32_t frame = UINT32_MAX;
    uint64_t samples = 0, available = 0, first_ordinal = 0, last_ordinal = 0,
             group_overflows = 0, first_event = 0, last_event = 0;
    double inclusive_ms = 0;
    size_t groups = 0;
    std::array<Aggregate, kFrameGroups> aggregates{};
  };
  struct Totals {
    uint64_t begun = 0, accepted = 0, completed = 0, cancelled = 0,
             available = 0, unavailable = 0;
    uint64_t pair_overflows = 0, pair_failures = 0, aggregate_overflows = 0,
             recent_evictions = 0, frame_evictions = 0,
             frame_group_overflows = 0;
    uint32_t first_frame = UINT32_MAX, last_frame = 0;
    double inclusive_ms = 0;
  };
  explicit NativeDrawTimestampLedger(uint32_t slots, bool every_frame=false) : capacity_(slots), every_frame_(every_frame) {
    if (slots != 64 && slots != 128)
      throw std::invalid_argument("Draw timestamp capacity must be64 or128");
    pairs_.reserve(kPairs);
  }
  static bool SamplesFrame(uint32_t frame) noexcept {
    return frame != UINT32_MAX && frame % kPeriod == 0;
  }
  bool ShouldSampleFrame(uint32_t frame) const noexcept {
    return frame != UINT32_MAX && (every_frame_ || SamplesFrame(frame));
  }
  uint32_t sample_period() const noexcept { return every_frame_ ? 1 : kPeriod; }
  uint32_t InternPair(std::string_view vertex,
                      std::optional<std::string_view> pixel) noexcept {
    try {
      const auto p = pixel.value_or(std::string_view{});
      for (size_t i = 0; i < pairs_.size(); ++i)
        if (pairs_[i].vertex == vertex && pairs_[i].pixel == p)
          return uint32_t(i + 1);
      if (pairs_.size() == kPairs) {
        ++totals_.pair_overflows;
        return 0;
      }
      Pair pair;
      pair.vertex = vertex;
      pair.pixel = p;
      pairs_.push_back(std::move(pair));
      return uint32_t(pairs_.size());
    } catch (...) {
      ++totals_.pair_failures;
      return 0;
    }
  }
  void Begin(Token token, uint32_t frame) {
    auto &slot = At(token.slot);
    if (!token.generation || slot.phase != Phase::Free || !ShouldSampleFrame(frame))
      throw std::logic_error("Draw timestamp slot is not retired or sampled");
    slot = {token.generation, Phase::Recording, {}};
    slot.tag.frame = frame;
    ++totals_.begun;
  }
  void End(Token token, const NativeGpuWorkTag &tag) {
    auto &slot = Match(token, Phase::Recording);
    if (tag.frame != slot.tag.frame || tag.kind != NativeGpuWorkKind::Draw)
      throw std::logic_error("Draw timestamp tag changed its sampled frame");
    slot.tag = tag;
    slot.phase = Phase::Ended;
  }
  void Accept(Token token) {
    Match(token, Phase::Ended).phase = Phase::Accepted;
    ++totals_.accepted;
  }
  void Cancel(Token token) {
    auto &slot = At(token.slot);
    if (slot.generation != token.generation ||
        (slot.phase != Phase::Recording && slot.phase != Phase::Ended))
      throw std::logic_error("Cannot cancel accepted or stale draw timestamp");
    slot = {};
    ++totals_.cancelled;
  }
  bool Pending(Token token) const {
    const auto &slot = At(token.slot);
    if (slot.phase == Phase::Free)
      return false;
    if (slot.phase != Phase::Accepted || slot.generation != token.generation)
      throw std::logic_error(
          "Timestamp fence has a stale or unaccepted draw token");
    return true;
  }
  void Retire(Token token, std::optional<NativeGpuTimestampInterval> interval,
              uint64_t submission, uint64_t retired_ns) {
    const auto tag = Match(token, Phase::Accepted).tag;
    Sample sample{++totals_.completed, submission, retired_ns, tag, interval};
    if (interval) {
      ++totals_.available;
      totals_.inclusive_ms += interval->milliseconds;
    } else
      ++totals_.unavailable;
    totals_.first_frame = std::min(totals_.first_frame, tag.frame);
    totals_.last_frame = std::max(totals_.last_frame, tag.frame);
    if (auto *aggregate = FindAggregate(tag))
      Accumulate(*aggregate, sample);
    auto &frame = FrameFor(tag.frame);
    ++frame.samples;
    frame.last_ordinal = sample.ordinal;
    frame.last_event = tag.event;
    if (frame.samples == 1)
      frame.first_event = tag.event;
    if (!frame.first_ordinal)
      frame.first_ordinal = sample.ordinal;
    if (interval) {
      ++frame.available;
      frame.inclusive_ms += interval->milliseconds;
    }
    size_t group = 0;
    while (group < frame.groups &&
           NativeGpuBatchComponent::Key(frame.aggregates[group].key) !=
               NativeGpuBatchComponent::Key(tag))
      ++group;
    if (group < frame.groups)
      Accumulate(frame.aggregates[group], sample);
    else if (group < kFrameGroups) {
      frame.aggregates[group].key = tag;
      ++frame.groups;
      Accumulate(frame.aggregates[group], sample);
    } else {
      ++frame.group_overflows;
      ++totals_.frame_group_overflows;
    }
    recent_[recent_next_] = sample;
    recent_next_ = (recent_next_ + 1) % kRecent;
    if (recent_count_ < kRecent)
      ++recent_count_;
    else
      ++totals_.recent_evictions;
    if (interval) {
      size_t index = slow_count_;
      if (slow_count_ < kSlowest)
        ++slow_count_;
      else {
        index = 0;
        for (size_t i = 1; i < kSlowest; ++i)
          if (slowest_[i].interval->milliseconds <
              slowest_[index].interval->milliseconds)
            index = i;
        if (interval->milliseconds <= slowest_[index].interval->milliseconds)
          index = kSlowest;
      }
      if (index < kSlowest)
        slowest_[index] = sample;
    }
    At(token.slot) = {};
  }
  bool pending() const noexcept {
    for (uint32_t i = 0; i < capacity_; ++i)
      if (slots_[i].phase != Phase::Free)
        return true;
    return false;
  }
  uint32_t capacity() const noexcept { return capacity_; }
  const auto &totals() const noexcept { return totals_; }
  const auto &pairs() const noexcept { return pairs_; }
  std::span<const Aggregate> aggregates() const noexcept {
    return {aggregates_.data(), aggregate_count_};
  }
  std::span<const Sample> slowest() const noexcept {
    return {slowest_.data(), slow_count_};
  }
  size_t recent_count() const noexcept { return recent_count_; }
  size_t frame_count() const noexcept { return frame_count_; }
  const Frame &frame(size_t i) const {
    if (i >= frame_count_)
      throw std::out_of_range("Draw timing frame");
    return frames_[(frame_next_ + kFrames - frame_count_ + i) % kFrames];
  }
  const Sample &recent(size_t i) const {
    if (i >= recent_count_)
      throw std::out_of_range("Draw timing sample");
    return recent_[(recent_next_ + kRecent - recent_count_ + i) % kRecent];
  }

private:
  uint32_t capacity_;
  const bool every_frame_;
  std::array<Slot, kSlots> slots_{};
  std::vector<Pair> pairs_;
  std::array<Aggregate, kAggregates> aggregates_{};
  // Open-addressed index is bounded; IDs own no borrowed string or resource.
  std::array<uint16_t, kAggregates * 2> aggregate_index_{};
  size_t aggregate_count_ = 0, recent_count_ = 0, recent_next_ = 0,
         slow_count_ = 0;
  std::array<Sample, kRecent> recent_{};
  std::array<Sample, kSlowest> slowest_{};
  Totals totals_;
  std::array<Frame, kFrames> frames_{};
  size_t frame_count_ = 0, frame_next_ = 0;
  Frame &FrameFor(uint32_t frame) {
    if (frame_count_) {
      auto &current = frames_[(frame_next_ + kFrames - 1) % kFrames];
      if (current.frame == frame)
        return current;
    }
    auto &next = frames_[frame_next_];
    next = {};
    next.frame = frame;
    frame_next_ = (frame_next_ + 1) % kFrames;
    if (frame_count_ < kFrames)
      ++frame_count_;
    else
      ++totals_.frame_evictions;
    return next;
  }
  static void Accumulate(Aggregate &aggregate, const Sample &sample) noexcept {
    ++aggregate.samples;
    aggregate.last_ordinal = sample.ordinal;
    if (!aggregate.first_ordinal)
      aggregate.first_ordinal = sample.ordinal;
    aggregate.first_frame = std::min(aggregate.first_frame, sample.tag.frame);
    aggregate.last_frame = std::max(aggregate.last_frame, sample.tag.frame);
    if (sample.interval) {
      ++aggregate.available;
      aggregate.inclusive_ms += sample.interval->milliseconds;
      aggregate.maximum_ms =
          std::max(aggregate.maximum_ms, sample.interval->milliseconds);
    }
  }
  Slot &At(uint32_t index) {
    if (index >= capacity_)
      throw std::out_of_range("Draw timestamp slot");
    return slots_[index];
  }
  const Slot &At(uint32_t index) const {
    if (index >= capacity_)
      throw std::out_of_range("Draw timestamp slot");
    return slots_[index];
  }
  Slot &Match(Token token, Phase phase) {
    auto &slot = At(token.slot);
    if (slot.phase != phase || slot.generation != token.generation)
      throw std::logic_error("Stale draw timestamp generation");
    return slot;
  }
  Aggregate *FindAggregate(const NativeGpuWorkTag &tag) noexcept {
    uint32_t hash = 2166136261u;
    for (uint32_t word : {tag.pair, tag.depth_control, tag.color_mask,
                          tag.blend_mask, tag.width, tag.height,
                          uint32_t(tag.discarded), uint32_t(tag.queried)})
      hash = (hash ^ word) * 16777619u;
    for (size_t probe = 0; probe < aggregate_index_.size(); ++probe) {
      auto &index = aggregate_index_[(hash + probe) % aggregate_index_.size()];
      if (index) {
        auto &entry = aggregates_[index - 1];
        if (NativeGpuBatchComponent::Key(entry.key) ==
            NativeGpuBatchComponent::Key(tag))
          return &entry;
      } else {
        if (aggregate_count_ == kAggregates) {
          ++totals_.aggregate_overflows;
          return nullptr;
        }
        index = uint16_t(++aggregate_count_);
        auto &entry = aggregates_[index - 1];
        entry.key = tag;
        return &entry;
      }
    }
    ++totals_.aggregate_overflows;
    return nullptr;
  }
};
} // namespace gow3::renderer
