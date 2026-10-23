#include "draw_timestamps.h"
#include "plume_vulkan.h"
#include <chrono>
#include <cmath>
#include <exception>
#include <nlohmann/json.hpp>
#include <vector>

namespace gow3::renderer {
namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
double Milliseconds(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}
Json Tag(const NativeGpuWorkTag &tag) {
  return {{"event", tag.event},
          {"frame", tag.frame},
          {"pair", tag.pair},
          {"depth_control", tag.depth_control},
          {"color_mask", tag.color_mask},
          {"blend_mask", tag.blend_mask},
          {"width", tag.width},
          {"height", tag.height},
          {"indices", tag.indices},
          {"queried", tag.queried},
          {"discarded", tag.discarded}};
}
Json Sample(const NativeDrawTimestampLedger::Sample &sample) {
  Json out = Tag(sample.tag);
  out["ordinal"] = sample.ordinal;
  out["submission"] = sample.submission;
  out["retired_steady_ns"] = sample.retired_steady_ns;
  out["available"] = bool(sample.interval);
  if (sample.interval) {
    out["begin_tick"] = sample.interval->begin_tick;
    out["end_tick"] = sample.interval->end_tick;
    out["inclusive_ms"] = sample.interval->milliseconds;
  }
  return out;
}
} // namespace
struct NativeDrawTimestamps::State {
  VkDevice device = VK_NULL_HANDLE;
  VkQueryPool pool = VK_NULL_HANDLE;
  plume::RenderCommandQueue *queue = nullptr;
  std::unique_ptr<NativeDrawTimestampLedger> ledger;
  std::array<Clock::time_point, NativeDrawTimestampLedger::kSlots> recorded{};
  uint32_t valid_bits = 0;
  double period_ns = 0, record_ms = 0, read_ms = 0;
  uint64_t query_reads = 0;
  std::string status = "unavailable";
  NativeDrawTimestampHealth health;
  ~State() {
    // Scene destroys this helper only after its normal full GPU drain.
    // Failed diagnostics retain the pool (and possibly stale ledger tokens)
    // until here; they never reset/reuse pairs after disabling.
    if (pool)
      vkDestroyQueryPool(device, pool, nullptr);
  }
  plume::VulkanCommandList &Commands(plume::RenderCommandList &commands) const {
    auto *vk = dynamic_cast<plume::VulkanCommandList *>(&commands);
    if (!vk || vk->queue != queue)
      throw std::logic_error("Foreign draw timestamp command list");
    return *vk;
  }
};
NativeDrawTimestamps::NativeDrawTimestamps(plume::RenderDevice &device,
                                           plume::RenderCommandQueue &queue,
                                           uint32_t capacity, bool every_frame)
    : state_(std::make_unique<State>()) {
  if (capacity != 64 && capacity != 128)
    throw std::invalid_argument("Invalid draw timestamp capacity");
  auto &s = *state_;
  auto *vk_device = dynamic_cast<plume::VulkanDevice *>(&device);
  auto *vk_queue = dynamic_cast<plume::VulkanCommandQueue *>(&queue);
  if (!vk_device || !vk_queue || vk_queue->device != vk_device ||
      !vk_device->vk || !vk_device->physicalDevice) {
    s.status = "unavailable: queue/device is not Vulkan";
    return;
  }
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(vk_device->physicalDevice, &count,
                                           nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(vk_device->physicalDevice, &count,
                                           families.data());
  if (vk_queue->familyIndex >= count ||
      !families[vk_queue->familyIndex].timestampValidBits) {
    s.status = "unavailable: queue family has no timestamp counter";
    return;
  }
  s.valid_bits = families[vk_queue->familyIndex].timestampValidBits;
  s.period_ns = vk_device->physicalDeviceProperties.limits.timestampPeriod;
  if (!DecodeNativeGpuTimestamps(0, 0, s.valid_bits, s.period_ns)) {
    s.status = "unavailable: invalid timestamp properties";
    return;
  }
  s.device = vk_device->vk;
  s.queue = &queue;
  // Separate from occlusion and submission timing pools; each existing draw
  // slot has two fixed queries, reset only by its next retired generation.
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = capacity * 2;
  const auto result = vkCreateQueryPool(s.device, &info, nullptr, &s.pool);
  if (result != VK_SUCCESS) {
    s.status = "unavailable: timestamp pool result " + std::to_string(result);
    return;
  }
  s.ledger = std::make_unique<NativeDrawTimestampLedger>(capacity,every_frame);
  s.status = "enabled";
}
NativeDrawTimestamps::~NativeDrawTimestamps() = default;
void NativeDrawTimestamps::RejectMetadata() noexcept {
  state_->health.Disable("draw metadata unavailable");
}
bool NativeDrawTimestamps::SamplesFrame(uint32_t frame) const noexcept {
  return state_->ledger && !state_->health.disabled &&
         state_->ledger->ShouldSampleFrame(frame);
}
uint32_t NativeDrawTimestamps::InternPair(
    std::string_view vertex, std::optional<std::string_view> pixel) noexcept {
  return state_->ledger && !state_->health.disabled
             ? state_->ledger->InternPair(vertex, pixel)
             : 0;
}
bool NativeDrawTimestamps::Begin(Token token, uint32_t frame,
                                 plume::RenderCommandList &commands) noexcept {
  auto &s = *state_;
  if (!SamplesFrame(frame))
    return false;
  try {
    auto &vk = s.Commands(commands);
    if (vk.activeRenderPass != VK_NULL_HANDLE)
      throw std::logic_error("Draw timestamp reset inside a render pass");
    s.ledger->Begin(token, frame);
    const auto start = Clock::now();
    s.recorded[token.slot] = start;
    vkCmdResetQueryPool(vk.vk, s.pool, token.slot * 2, 2);
    vkCmdWriteTimestamp(vk.vk, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.pool,
                        token.slot * 2);
    s.record_ms += Milliseconds(start, Clock::now());
    return true;
  } catch (...) {
    s.health.Disable("begin ownership or command-list failure");
    return false;
  }
}
void NativeDrawTimestamps::End(Token token, const NativeGpuWorkTag &tag,
                               plume::RenderCommandList &commands) noexcept {
  auto &s = *state_;
  if (!s.ledger || s.health.disabled)
    return;
  try {
    auto &vk = s.Commands(commands);
    s.ledger->End(token, tag);
    const auto start = Clock::now();
    // Scene was about to call end(), which ends this exact render pass itself.
    // Keep its store operations inside the measured interval, before BOTTOM.
    vk.endActiveRenderPass();
    vkCmdWriteTimestamp(vk.vk, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool,
                        token.slot * 2 + 1);
    s.record_ms += Milliseconds(start, Clock::now());
  } catch (...) {
    s.health.Disable("end ownership or command-list failure");
  }
}
void NativeDrawTimestamps::Accept(Token token) noexcept {
  auto &s = *state_;
  if (!s.ledger || s.health.disabled)
    return;
  try {
    s.ledger->Accept(token);
  } catch (...) {
    s.health.Disable("accepted token mismatch");
  }
}
void NativeDrawTimestamps::Cancel(Token token) noexcept {
  auto &s = *state_;
  if (!s.ledger || s.health.disabled)
    return;
  try {
    s.ledger->Cancel(token);
  } catch (...) {
    s.health.Disable("cancelled token mismatch");
  }
}
void NativeDrawTimestamps::Retire(std::span<const Token> tokens,
                                  uint64_t submission) noexcept {
  auto &s = *state_;
  if (!s.ledger || s.health.disabled)
    return;
  try {
    // Validate the whole fenced token list before reading/retiring any pair.
    for (const auto token : tokens)
      (void)s.ledger->Pending(token);
    struct Value {
      uint64_t ticks, available;
    };
    std::array<Value, NativeDrawTimestampLedger::kSlots * 2> values{};
    for (size_t i = 0; i < tokens.size();) {
      if (!s.ledger->Pending(tokens[i])) {
        ++i;
        continue;
      }
      size_t end = i + 1;
      while (end < tokens.size() &&
             tokens[end].slot == tokens[end - 1].slot + 1 &&
             s.ledger->Pending(tokens[end]))
        ++end;
      const uint32_t count = uint32_t(end - i) * 2;
      const auto start = Clock::now();
      const auto result = vkGetQueryPoolResults(
          s.device, s.pool, tokens[i].slot * 2, count, count * sizeof(Value),
          values.data(), sizeof(Value),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      const auto retired = Clock::now();
      s.read_ms += Milliseconds(start, retired);
      ++s.query_reads;
      if (result != VK_SUCCESS && result != VK_NOT_READY) {
        s.health.Disable("timestamp result read failed");
        return;
      }
      const auto retired_ns =
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       retired.time_since_epoch())
                       .count());
      for (size_t j = i; j < end; ++j) {
        const auto &begin = values[(j - i) * 2];
        const auto &finish = values[(j - i) * 2 + 1];
        std::optional<NativeGpuTimestampInterval> interval;
        const auto age = std::chrono::duration<double, std::nano>(
                             retired - s.recorded[tokens[j].slot])
                             .count();
        if (result == VK_SUCCESS && begin.available && finish.available &&
            age < std::ldexp(s.period_ns, int(s.valid_bits)))
          interval = DecodeNativeGpuTimestamps(begin.ticks, finish.ticks,
                                               s.valid_bits, s.period_ns);
        s.ledger->Retire(tokens[j], interval, submission, retired_ns);
      }
      i = end;
    }
  } catch (...) {
    s.health.Disable("fenced timestamp token mismatch");
  }
}
Json NativeDrawTimestamps::Report(bool details) const {
  const auto &s = *state_;
  Json report{{"enabled", true},
              {"supported", bool(s.ledger)},
              {"status", s.health.disabled ? "disabled after diagnostic failure"
                                           : s.status},
              {"semantics",
               "TOP_OF_PIPE to BOTTOM_OF_PIPE inclusive draw-command-list "
               "latency; dependencies and overlapping execution are included. "
               "Not exclusive shader time; sums are not frame GPU time."},
              {"sample_period_frames", s.ledger ? s.ledger->sample_period() : NativeDrawTimestampLedger::kPeriod},
              {"sample_frame_remainder", 0},
              {"valid_bits", s.valid_bits},
              {"period_ns", s.period_ns},
              {"record_ms", s.record_ms},
              {"read_ms", s.read_ms},
              {"query_reads", s.query_reads}};
  report["disabled"] = s.health.disabled;
  report["diagnostic_failures"] = s.health.failures;
  report["failure_reason"] =
      s.health.reason ? Json(s.health.reason) : Json(nullptr);
  report["incomplete"] = !s.ledger || s.health.disabled;
  if (!s.ledger)
    return report;
  const auto &ledger = *s.ledger;
  const auto &t = ledger.totals();
  report["incomplete"] = s.health.disabled || ledger.pending() ||
                         t.unavailable || t.pair_overflows || t.pair_failures ||
                         t.aggregate_overflows || t.frame_group_overflows;
  report["pending"] = ledger.pending();
  report["capacity"] = ledger.capacity();
  report["begun"] = t.begun;
  report["accepted"] = t.accepted;
  report["completed"] = t.completed;
  report["cancelled"] = t.cancelled;
  report["available"] = t.available;
  report["unavailable"] = t.unavailable;
  report["first_frame"] = t.first_frame;
  report["last_frame"] = t.last_frame;
  report["inclusive_sum_ms"] = t.inclusive_ms;
  report["pair_overflows"] = t.pair_overflows;
  report["pair_failures"] = t.pair_failures;
  report["aggregate_overflows"] = t.aggregate_overflows;
  report["recent_evictions"] = t.recent_evictions;
  report["pair_count"] = ledger.pairs().size();
  report["aggregate_count"] = ledger.aggregates().size();
  report["frame_group_overflows"] = t.frame_group_overflows;
  report["ledger_bytes"] = sizeof(NativeDrawTimestampLedger);
  report["frame_group_limit"] = NativeDrawTimestampLedger::kFrameGroups;
  report["retained_frames"] = ledger.frame_count();
  report["frame_evictions"] = t.frame_evictions;
  if (!details)
    return report;
  report["pairs"] = Json::array();
  uint32_t id = 0;
  for (const auto &pair : ledger.pairs())
    report["pairs"].push_back(
        {{"id", ++id},
         {"vertex", pair.vertex},
         {"pixel", pair.pixel.empty() ? Json(nullptr) : Json(pair.pixel)}});
  report["aggregates"] = Json::array();
  for (const auto &aggregate : ledger.aggregates()) {
    auto row = Tag(aggregate.key);
    row.erase("event");
    row.erase("frame");
    row.erase("indices");
    row["samples"] = aggregate.samples;
    row["available"] = aggregate.available;
    row["first_frame"] = aggregate.first_frame;
    row["last_frame"] = aggregate.last_frame;
    row["first_ordinal"] = aggregate.first_ordinal;
    row["last_ordinal"] = aggregate.last_ordinal;
    row["inclusive_ms"] = aggregate.inclusive_ms;
    row["maximum_ms"] = aggregate.maximum_ms;
    report["aggregates"].push_back(std::move(row));
  }
  report["frames"] = Json::array();
  for (size_t i = 0; i < ledger.frame_count(); ++i) {
    const auto &frame = ledger.frame(i);
    Json row = {{"frame", frame.frame},
                {"samples", frame.samples},
                {"available", frame.available},
                {"first_ordinal", frame.first_ordinal},
                {"last_ordinal", frame.last_ordinal},
                {"first_event", frame.first_event},
                {"last_event", frame.last_event},
                {"inclusive_ms", frame.inclusive_ms},
                {"group_overflows", frame.group_overflows},
                {"groups", Json::array()}};
    for (size_t j = 0; j < frame.groups; ++j) {
      const auto &aggregate = frame.aggregates[j];
      auto group = Tag(aggregate.key);
      group.erase("event");
      group.erase("frame");
      group.erase("indices");
      group["samples"] = aggregate.samples;
      group["available"] = aggregate.available;
      group["inclusive_ms"] = aggregate.inclusive_ms;
      group["maximum_ms"] = aggregate.maximum_ms;
      row["groups"].push_back(std::move(group));
    }
    report["frames"].push_back(std::move(row));
  }
  report["recent"] = Json::array();
  for (size_t i = 0; i < ledger.recent_count(); ++i)
    report["recent"].push_back(Sample(ledger.recent(i)));
  report["slowest"] = Json::array();
  for (const auto &sample : ledger.slowest())
    report["slowest"].push_back(Sample(sample));
  return report;
}
} // namespace gow3::renderer
