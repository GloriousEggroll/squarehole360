// Apply reviewed settings on their owning threads. Resource layout changes stay
// in the saved config until restart. The GUI reads an acknowledgement, never
// assumes that a successful file save means the renderer applied it.
#include "gow3_settings.h"
#include "gow3_settings_policy.h"
#include "gow3_vrr.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/command_processor.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <toml++/toml.hpp>
#include "gow3_host.h"
#include <fstream>
#include <sstream>
#include <future>
#include <map>
#include <set>
#include <cmath>

namespace {
using Values = std::map<std::string, std::string>;
// Keep tools/settings/model.py LIVE_SETTINGS in sync; checked by settings tests.
const std::set<std::string> live_keys = {
  "fullscreen", "window_width", "window_height", "gow3_frame_limit_mode", "framerate_limit",
  "present_effect", "present_fsr_sharpness_reduction", "present_fsr3_ghosting_reduction",
  "present_fsr_max_upsampling_passes", "present_cas_additional_sharpness",
  "present_dither", "swap_post_effect", "present_frame_generation",
  "gow3_low_latency", "present_anti_lag", "mnk_mode", "mnk_mouse_mode", "mnk_sensitivity",
  // The keyboard/mouse driver keeps its own copy of these, refreshed on change.
  "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_trigger",
  "keybind_right_trigger", "keybind_left_shoulder", "keybind_right_shoulder",
  "keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left", "keybind_lstick_right",
  "keybind_lstick_press", "keybind_rstick_up", "keybind_rstick_down", "keybind_rstick_left",
  "keybind_rstick_right", "keybind_rstick_press", "keybind_dpad_up", "keybind_dpad_down",
  "keybind_dpad_left", "keybind_dpad_right", "keybind_back", "keybind_start", "keybind_guide"
};
struct Watch {
  rex::ui::Window* window;
  rex::graphics::GraphicsSystem* graphics;
  std::filesystem::path config, status;
  Values active, requested;
  Gow3RendererMode renderer = Gow3RendererMode::Native;
  std::string digest, error, start_time;
  std::set<std::string> ignored;
  std::future<void> pending;
  Gow3UiTimer timer;
};
std::unique_ptr<Watch> watch;
std::string Read(const std::filesystem::path& path) {
  // Binary: the digest must match the settings editor's hash of the raw bytes.
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read settings file");
  return {std::istreambuf_iterator<char>(stream), {}};
}
std::string ReadConfig(const std::filesystem::path& path) {
  return std::filesystem::exists(path) ? Read(path) : std::string{};
}
std::string Digest(const std::string& text) {
  return Gow3Sha256Hex(text);
}
Values Parse(const std::string& text) {
  Values values;
  auto table = toml::parse(text);
  const auto* renderer_node = table.get("gow3_renderer");
  if (renderer_node && !renderer_node->is_string()) throw std::runtime_error("Invalid renderer setting");
  const auto mode = ParseGow3RendererMode(renderer_node ? renderer_node->as_string()->get() : "native");
  values["gow3_renderer"] = Gow3RendererName(mode);
  values["anisotropic_override"] = "-1";
  for (auto&& [key, node] : table) {
    std::string name(key);
    if (!rex::cvar::GetFlagInfo(name) || !Gow3MenuKeyApplies(name, mode)) continue;
    if (name == "gow3_renderer" || name == "anisotropic_override") continue;
    if (node.is_string()) values[name] = node.as_string()->get();
    else if (node.is_boolean()) values[name] = node.as_boolean()->get() ? "true" : "false";
    else if (node.is_number()) {
      auto value = node.value<double>().value();
      if (!std::isfinite(value)) throw std::runtime_error("Non-finite setting: " + name);
      values[name] = rex::cvar::GetFlagInfo(name)->type == rex::cvar::FlagType::Double
          ? std::to_string(value) : std::to_string(node.value<int64_t>().value());
    } else throw std::runtime_error("Unsupported settings type: " + name);
  }
  for (const auto& setting : kMenuSettings) {
    if (!Gow3MenuSettingApplies(setting, mode)) continue;
    const auto found = values.find(setting.key);
    if (found != values.end() && !Gow3MenuValidValue(setting, found->second))
      throw std::runtime_error(std::string("Invalid setting: ") + setting.label);
  }
  auto get = [&](const char* key) { return values.contains(key) ? values.at(key) : rex::cvar::GetFlagInfo(key)->default_value; };
  if (mode == Gow3RendererMode::Xenia && std::stoi(get("texture_cache_memory_limit_soft")) > std::stoi(get("texture_cache_memory_limit_hard")))
    throw std::runtime_error("Texture cache target must not exceed its maximum");
  if (get("gow3_frame_limit_mode") == "custom" && std::stod(get("framerate_limit")) < 1)
    throw std::runtime_error("Custom frame limit must be at least 1 FPS");
  return values;
}
void Publish(Watch& w) {
  const auto vrr = Gow3GetVrrStatus();
  toml::table state;
  state.insert("pid", Gow3ProcessId());
  state.insert("start_time", w.start_time);
  state.insert("digest", w.digest);
  state.insert("error", w.error);
  state.insert("pending", w.pending.valid());
  state.insert("vrr_output", vrr.output);
  state.insert("vrr_available", vrr.available);
  state.insert("vrr_enabled", vrr.enabled);
  state.insert("vrr_pending", vrr.pending);
  state.insert("config", w.config.string());
  toml::array restart, ignored, live;
  for (auto& [key, value] : w.requested) {
    if (w.ignored.contains(key)) ignored.push_back(key);
    else if (!w.active.contains(key) || w.active.at(key) != value) restart.push_back(key);
  }
  const auto requested_renderer = ParseGow3RendererMode(w.requested.at("gow3_renderer"));
  for (const auto& setting : kMenuSettings)
    if (Gow3MenuLiveApplies(setting, w.renderer, requested_renderer)) live.push_back(setting.key);
  state.insert("renderer", std::string(Gow3RendererName(w.renderer)));
  state.insert("restart", std::move(restart));
  state.insert("ignored", std::move(ignored));
  state.insert("live_keys", std::move(live));
  const auto temporary = w.status.string() + ".tmp";
  { std::ofstream file(temporary); file << state; if (!file) return; }
  std::error_code ec;
  std::filesystem::rename(temporary, w.status, ec);
}
void Apply(Watch& w, const Values& desired) {
  bool paint_changed = false, fps_changed = false, window_changed = false;
  auto* presenter = w.graphics->presenter();
  auto* processor = w.graphics->command_processor();
  auto paint = presenter->GetGuestOutputPaintConfigFromUIThread();
  const auto requested_renderer = ParseGow3RendererMode(desired.at("gow3_renderer"));
  for (const auto& [key, value] : desired) {
    if (rex::cvar::IsCommandLineOverride(key)) { w.ignored.insert(key); continue; }
    if (!Gow3MenuKeyApplies(key, w.renderer) || !Gow3MenuKeyApplies(key, requested_renderer)) continue;
    if (!live_keys.contains(key) || (w.active.contains(key) && w.active.at(key) == value)) continue;
    if (key == "gow3_frame_limit_mode" || key == "framerate_limit") { fps_changed = true; continue; }
    if (!rex::cvar::SetFlagByName(key, value)) {
      w.error += "Rejected setting: " + key + ". "; continue;
    }
    if (key == "fullscreen" || key == "window_width" || key == "window_height") window_changed = true;
    else if (key.starts_with("mnk_") || key.starts_with("keybind_")) {
      // Input settings take effect on the next controller poll.
    } else if (key == "swap_post_effect") {
      using Effect = rex::graphics::CommandProcessor::SwapPostEffect;
      processor->SetDesiredSwapPostEffect(value == "fxaa" ? Effect::kFxaa :
          value == "fxaa_extreme" ? Effect::kFxaaExtreme : Effect::kNone);
    } else {
      using Effect = rex::ui::Presenter::GuestOutputPaintConfig::Effect;
      if (key == "present_effect")
        paint.SetEffect(value == "fsr" ? Effect::kFsr
                        : value == "fsr3" || value == "dlss" || value == "xess" ? Effect::kFsr3
                        : value == "cas" ? Effect::kCas : Effect::kBilinear);
      if (key == "present_fsr_sharpness_reduction") paint.SetFsrSharpnessReduction(std::stof(value));
      if (key == "present_fsr_max_upsampling_passes") paint.SetFsrMaxUpsamplingPasses(std::stoi(value));
      if (key == "present_cas_additional_sharpness") paint.SetCasAdditionalSharpness(std::stof(value));
      if (key == "present_dither") paint.SetDither(value == "true");
      paint_changed = true;
    }
    w.active[key] = value;
    REXLOG_INFO("Live setting applied: {} = {}", key, value);
  }
  if (window_changed) {
    auto width = std::stoul(rex::cvar::GetFlagByName("window_width"));
    auto height = std::stoul(rex::cvar::GetFlagByName("window_height"));
    // Zero retains the app's desired size (the SDK's documented default).
    w.window->SetLogicalSize(width ? width : w.window->GetDesiredLogicalWidth(),
                             height ? height : w.window->GetDesiredLogicalHeight());
    w.window->SetFullscreen(rex::cvar::GetFlagByName("fullscreen") == "true");
  }
  if (paint_changed) presenter->SetGuestOutputPaintConfigFromUIThread(paint);
  double fps = -1;
  if (fps_changed) {
    auto get = [&](const char* key) {
      return desired.contains(key) && !rex::cvar::IsCommandLineOverride(key)
          ? desired.at(key) : rex::cvar::GetFlagByName(key);
    };
    const auto mode = get("gow3_frame_limit_mode");
    double custom = std::stod(get("framerate_limit"));
    if ((mode != "custom" && mode != "display" && mode != "unlimited") ||
        !std::isfinite(custom) || custom < 0 || custom > 1000) {
      w.error += "Invalid frame-rate limit. ";
    } else {
      fps = mode == "unlimited" ? 0 : mode == "display" ? w.window->GetMonitorRefreshRateHz() : custom;
      rex::cvar::SetFlagByName("gow3_frame_limit_mode", mode);
      for (const char* key : {"gow3_frame_limit_mode", "framerate_limit"})
        if (desired.contains(key) && !rex::cvar::IsCommandLineOverride(key)) w.active[key] = desired.at(key);
    }
  }
  auto done = std::make_shared<std::promise<void>>();
  w.pending = done->get_future();
  processor->CallInThread([done, fps] {
    if (fps >= 0) {
      rex::cvar::SetFlagByName("framerate_limit", std::to_string(fps));
      REXLOG_INFO("Live frame-rate ceiling applied: {} FPS", fps);
    }
    done->set_value();
  });
}
void Tick() {
  auto& w = *watch;
  try {
    if (Gow3PollVrr()) Publish(w);
    if (w.pending.valid()) {
      if (w.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
      w.pending.get(); Publish(w);
    }
    auto text = ReadConfig(w.config);
    auto digest = Digest(text);
    if (digest == w.digest) return;
    w.error.clear(); w.ignored.clear(); w.digest = digest;
    // Parse completely before applying any settings. Failed edits keep the game running.
    w.requested = Parse(text);
    // Removing an option restores its default (GUI saves always include all keys).
    const auto requested_renderer = ParseGow3RendererMode(w.requested.at("gow3_renderer"));
    for (auto& [key, value] : w.active)
      if (!w.requested.contains(key) && Gow3MenuKeyApplies(key, requested_renderer))
        w.requested[key] = rex::cvar::GetFlagInfo(key)->default_value;
    Apply(w, w.requested);
  } catch (const std::exception& e) { w.error = e.what(); Publish(w); }
  return;
}
}
void Gow3StartSettingsWatch(rex::ui::Window* window, rex::Runtime* runtime) {
  if (!window || !runtime) return;
  auto* graphics = dynamic_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system());
  if (!graphics || !graphics->presenter() || !graphics->command_processor()) return;
  auto w = std::make_unique<Watch>();
  w->window = window; w->graphics = graphics;
  w->config = std::filesystem::absolute(Gow3SettingsPath());
  w->status = w->config.string() + ".runtime." + std::to_string(Gow3ProcessId()) + ".toml";
  try {
    auto text = ReadConfig(w->config); w->digest = Digest(text);
    w->renderer = ParseGow3RendererMode(rex::cvar::GetFlagByName("gow3_renderer"));
    w->requested = Parse(text);
    w->active = Gow3SettingsStartupBaseline(w->requested, w->renderer,
        [](std::string_view key) { return rex::cvar::GetFlagByName(key); },
        [](std::string_view key) { return rex::cvar::IsCommandLineOverride(key); });
    for (const auto& [key, value] : w->requested)
      if (rex::cvar::IsCommandLineOverride(key)) w->ignored.insert(key);
    w->start_time = Gow3ProcessStartTime();
  } catch (const std::exception& e) { REXLOG_WARN("Live settings unavailable: {}", e.what()); return; }
  watch = std::move(w);
  Gow3StartVrr(window);
  Publish(*watch);
  watch->timer.Start(window->app_context(), std::chrono::milliseconds(500), Tick);
  REXLOG_INFO("Watching settings for live changes: {}", watch->config.string());
}
void Gow3StopSettingsWatch() {
  Gow3StopVrr();
  if (!watch) return;
  watch->timer.Stop();
  std::error_code ec; std::filesystem::remove(watch->status, ec);
  watch.reset();
}

Gow3LiveSettingsStatus Gow3GetLiveSettingsStatus() {
  Gow3LiveSettingsStatus status;
  if (!watch) { status.error = "Settings are not ready yet."; return status; }
  status.digest = watch->digest; status.error = watch->error;
  status.pending = watch->pending.valid();
  for (auto& [key, value] : watch->requested)
    if (!watch->ignored.contains(key) && (!watch->active.contains(key) || watch->active.at(key) != value))
      status.restart.push_back(key);
  return status;
}
