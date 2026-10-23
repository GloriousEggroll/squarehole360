#include "gow3_host.h"
#include "renderer/native/content_hash.h"
#include <rex/ui/windowed_app_context.h>
#include <cstdlib>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

int64_t Gow3ProcessId() {
#ifdef _WIN32
  return int64_t(GetCurrentProcessId());
#else
  return int64_t(getpid());
#endif
}

std::string Gow3ProcessStartTime() {
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return {};
  return std::to_string((uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime);
#else
  // Field 22 of /proc/self/stat: start time in clock ticks since boot.
  std::ifstream stream("/proc/self/stat");
  std::string stat{std::istreambuf_iterator<char>(stream), {}};
  const auto paren = stat.rfind(')');
  if (paren == std::string::npos) return {};
  std::istringstream fields(stat.substr(paren + 2));
  std::string value;
  for (int i = 3; i <= 22; ++i) fields >> value;
  return value;
#endif
}

std::string Gow3Sha256Hex(std::string_view text) {
  return gow3::renderer::ContentHash(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

void Gow3WriteFileAtomic(const std::filesystem::path& path, std::string_view text) {
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Unable to save settings");
    stream.write(text.data(), std::streamsize(text.size()));
    stream.flush();
    if (!stream) throw std::runtime_error("Unable to save settings");
  }
  std::error_code ec;
#ifndef _WIN32
  std::filesystem::permissions(temporary,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
#endif
  // Replaces an existing target on every platform (MoveFileEx on Windows).
  // Windows refuses while the settings editor has the file open for reading.
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::filesystem::rename(temporary, path, ec);
    if (!ec) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (ec) {
    std::filesystem::remove(temporary, ec);
    throw std::runtime_error("Unable to save settings");
  }
}

std::filesystem::path Gow3MenuFontPath(bool bold) {
#ifdef _WIN32
  const char* windir = std::getenv("WINDIR");
  std::filesystem::path fonts = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts";
  for (const char* name : bold ? std::initializer_list<const char*>{"segoeuib.ttf", "arialbd.ttf"}
                               : std::initializer_list<const char*>{"segoeui.ttf", "arial.ttf"})
    if (std::filesystem::exists(fonts / name)) return fonts / name;
#else
  std::filesystem::path font = bold
      ? "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed-Bold.ttf"
      : "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed.ttf";
  if (std::filesystem::exists(font)) return font;
#endif
  return {};
}

void Gow3UiTimer::Start(rex::ui::WindowedAppContext& context, std::chrono::milliseconds period,
                        std::function<void()> tick) {
  Stop();
  auto state = std::make_shared<State>();
  state->tick = std::move(tick);
  state_ = state;
  thread_ = std::thread([state, period, &context] {
    std::unique_lock lock(state->mutex);
    while (!state->wake.wait_for(lock, period, [&] { return state->stop; })) {
      if (state->queued.exchange(true)) continue;
      context.CallInUIThread([state] {
        state->queued = false;
        if (!state->stopped) state->tick();
      });
    }
  });
}

void Gow3UiTimer::Stop() {
  if (!state_) return;
  state_->stopped = true;
  {
    std::lock_guard lock(state_->mutex);
    state_->stop = true;
  }
  state_->wake.notify_all();
  if (thread_.joinable()) thread_.join();
  state_.reset();
}
