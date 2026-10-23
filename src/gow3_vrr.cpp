#include "gow3_vrr.h"
#include <rex/ui/adaptive_sync.h>
#include <rex/ui/window.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>

namespace {
using Clock = std::chrono::steady_clock;
std::mutex status_mutex;
Gow3VrrStatus status;
void Publish(Gow3VrrStatus value) {
  std::lock_guard lock(status_mutex);
  status = std::move(value);
}
Gow3VrrStatus Probe(const std::string& output) {
  Gow3VrrStatus result;
  result.output = output;
  if (output.empty()) {
    result.error = "VRR status is unavailable on this window system.";
    return result;
  }
  const auto displays = rex::ui::ProbeAdaptiveSyncDisplays();
  const auto found = std::find_if(displays.begin(), displays.end(),
      [&](const auto& d) { return d.name == output; });
  if (found == displays.end()) {
    result.error = "The compositor did not report VRR status for " + output + ".";
    return result;
  }
  result.available = found->available;
  result.enabled = found->enabled;
  if (!result.available) result.error = found->reason;
  return result;
}
struct Session {
  rex::ui::Window* window = nullptr;
  std::future<Gow3VrrStatus> job;
  std::string output;
  Clock::time_point next_poll{};
};
std::unique_ptr<Session> session;
}

Gow3VrrStatus Gow3GetVrrStatus() {
  std::lock_guard lock(status_mutex);
  return status;
}
void Gow3StartVrr(rex::ui::Window* window) {
  Gow3StopVrr();
  if (!window) return;
  session = std::make_unique<Session>();
  session->window = window;
  Gow3PollVrr();
}
bool Gow3PollVrr() {
  if (!session) return false;
  auto& s = *session;
  const auto output = s.window->GetDisplayOutputName();
  bool changed = false;
  if (s.job.valid()) {
    if (s.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    auto result = s.job.get();
    if (s.output == output) { Publish(std::move(result)); changed = true; }
  }
  const auto now = Clock::now();
  if (s.output == output && now < s.next_poll) return changed;
  if (s.output != output || s.next_poll == Clock::time_point{}) {
    Gow3VrrStatus pending; pending.output = output; pending.pending = true;
    Publish(std::move(pending)); changed = true;
  }
  s.output = output;
  s.next_poll = now + std::chrono::seconds(10);
  s.job = std::async(std::launch::async, [output] {
    try { return Probe(output); }
    catch (const std::exception& e) {
      Gow3VrrStatus failed; failed.output = output;
      failed.error = std::string("VRR status unavailable: ") + e.what();
      return failed;
    }
  });
  return changed;
}
void Gow3StopVrr() {
  if (!session) return;
  if (session->job.valid()) session->job.wait();
  session.reset();
  Publish({});
}
