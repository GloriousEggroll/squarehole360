#include "edram_inventory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

namespace gow3::renderer::edram_inventory {
namespace {
constexpr size_t kRingSize = 1 << 16;
constexpr uint32_t kFlushInterval = 600;

struct RingEntry {
  uint64_t sequence = UINT64_MAX;
  GuestStack stack{};
};
struct Totals {
  uint64_t count = 0, pixels = 0;
  uint32_t first_frame = UINT32_MAX, last_frame = 0;
};
using Key = std::tuple<std::string, std::string, std::string, std::string, std::string, GuestStack>;

std::atomic<bool> enabled{false};
std::string report_path;
std::mutex ring_mutex;
std::vector<RingEntry> ring;
std::mutex totals_mutex;
std::map<Key, Totals> totals;
uint64_t current_event = UINT64_MAX;
uint32_t current_frame = 0, last_flush_frame = 0;
std::string current_type;
std::mutex sections_mutex;
std::map<std::string, std::string> sections;
std::mutex targets_mutex;
std::map<uint32_t, std::string> resolve_targets;

std::string Hex(uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", value);
  return text;
}
}  // namespace

void Configure(const std::string& path) {
  report_path = path;
  if (path.empty()) return;
  ring.assign(kRingSize, {});
  enabled.store(true, std::memory_order_release);
}

bool Enabled() { return enabled.load(std::memory_order_relaxed); }

void RecordStack(uint64_t sequence, const GuestStack& stack) {
  if (!Enabled()) return;
  std::lock_guard lock(ring_mutex);
  ring[sequence % kRingSize] = {sequence, stack};
}

void BeginOperation(uint64_t event, uint32_t frame, std::string_view type) {
  if (!Enabled()) return;
  current_event = event;
  current_frame = frame;
  current_type = type;
}

void Note(std::string_view kind, std::string_view surface, std::string_view other,
          std::string_view outcome, uint64_t pixels) {
  if (!Enabled()) return;
  GuestStack stack{};
  {
    std::lock_guard lock(ring_mutex);
    const auto& entry = ring[current_event % kRingSize];
    if (entry.sequence == current_event) stack = entry.stack;
  }
  std::lock_guard lock(totals_mutex);
  auto& t = totals[Key{std::string(kind), current_type, std::string(surface), std::string(other),
                       std::string(outcome), stack}];
  ++t.count;
  t.pixels += pixels;
  t.first_frame = std::min(t.first_frame, current_frame);
  t.last_frame = std::max(t.last_frame, current_frame);
}

void RecordResolveTarget(uint32_t base, std::string_view description) {
  if (!Enabled()) return;
  std::lock_guard lock(targets_mutex);
  resolve_targets[base] = description;
}

std::string ResolveTarget(uint32_t base) {
  if (!Enabled()) return {};
  std::lock_guard lock(targets_mutex);
  const auto found = resolve_targets.find(base);
  return found == resolve_targets.end() ? std::string("no resolve") : found->second;
}

void SetSection(std::string_view name, std::string json) {
  if (!Enabled()) return;
  std::lock_guard lock(sections_mutex);
  sections[std::string(name)] = std::move(json);
}

void FrameCompleted(uint32_t frame) {
  if (!Enabled() || frame < last_flush_frame + kFlushInterval) return;
  last_flush_frame = frame;
  Flush();
}

void Flush() {
  if (!Enabled()) return;
  nlohmann::json records = nlohmann::json::array();
  {
    std::lock_guard lock(totals_mutex);
    for (const auto& [key, t] : totals) {
      const auto& [kind, op, surface, other, outcome, stack] = key;
      nlohmann::json frames = nlohmann::json::array();
      for (uint32_t address : stack)
        if (address) frames.push_back(Hex(address));
      records.push_back({{"kind", kind}, {"operation", op}, {"surface", surface}, {"other", other},
                         {"outcome", outcome}, {"stack", frames}, {"count", t.count},
                         {"pixels", t.pixels}, {"first_frame", t.first_frame},
                         {"last_frame", t.last_frame}});
    }
  }
  nlohmann::json report{{"schema", "native-edram-inventory-v1"},
                        {"last_frame", current_frame},
                        {"records", std::move(records)}};
  {
    std::lock_guard lock(sections_mutex);
    for (const auto& [name, json] : sections)
      report[name] = nlohmann::json::parse(json, nullptr, false);
  }
  const std::filesystem::path path(report_path);
  std::error_code error;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    out << report.dump(1) << '\n';
  }
  std::filesystem::rename(temporary, path, error);
}
}  // namespace gow3::renderer::edram_inventory
