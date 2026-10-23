#pragma once
// Small host services the settings code needs on every platform: process
// identity, file digests, atomic saves and a periodic UI-thread callback.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace rex::ui { class WindowedAppContext; }

int64_t Gow3ProcessId();
// Opaque token that, with the pid, identifies this process instance. The
// settings editor compares both to reject a status file left by a dead pid.
std::string Gow3ProcessStartTime();
std::string Gow3Sha256Hex(std::string_view text);
// Write to a sibling temporary file, then rename over the target.
void Gow3WriteFileAtomic(const std::filesystem::path& path, std::string_view text);
// System font for the in-game menu; empty when none is installed.
std::filesystem::path Gow3MenuFontPath(bool bold);

// Runs `tick` on the UI thread every `period` until Stop. A tick is not queued
// again while the previous one is still waiting, so a busy UI thread is never
// flooded.
class Gow3UiTimer {
 public:
  Gow3UiTimer() = default;
  Gow3UiTimer(const Gow3UiTimer&) = delete;
  Gow3UiTimer& operator=(const Gow3UiTimer&) = delete;
  ~Gow3UiTimer() { Stop(); }
  void Start(rex::ui::WindowedAppContext& context, std::chrono::milliseconds period,
             std::function<void()> tick);
  // Call on the UI thread; no tick runs after it returns.
  void Stop();

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    std::atomic<bool> stopped{false};
    std::atomic<bool> queued{false};
    std::function<void()> tick;
  };
  std::shared_ptr<State> state_;
  std::thread thread_;
};
