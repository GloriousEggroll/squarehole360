#include "gow3_settings.h"
#include "gow3_settings_schema.h"
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_STRING(gow3_config, "", "Game", "Path to the game settings TOML file");
REXCVAR_DEFINE_STRING(gow3_renderer, "native", "Graphics",
                     "Renderer: Native or Emulated (Xenia) (restart required)")
    .allowed({"native", "xenia", "reference"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gow3_frame_limit_mode, "custom", "Game",
                     "Frame-rate ceiling: custom, display, or unlimited")
    .allowed({"custom", "display", "unlimited"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

std::filesystem::path Gow3SettingsPath() {
  if (!REXCVAR_GET(gow3_config).empty()) return REXCVAR_GET(gow3_config);
  return rex::filesystem::GetExecutableFolder().parent_path().parent_path() / "config/gow3.toml";
}

void Gow3ConfigureDisplay(rex::ui::Window* window) {
  auto mode = REXCVAR_GET(gow3_frame_limit_mode);
  if (mode == "unlimited") {
    rex::cvar::SetFlagByName("framerate_limit", "0");
  } else if (mode == "display") {
    double refresh = window ? window->GetMonitorRefreshRateHz() : 60.0;
    rex::cvar::SetFlagByName("framerate_limit", std::to_string(refresh));
  }
  REXLOG_INFO("Frame-rate ceiling: {} FPS ({}); VSync={}; upscaler={}",
              rex::cvar::GetFlagByName("framerate_limit"), mode,
              rex::cvar::GetFlagByName("vsync"), rex::cvar::GetFlagByName("present_effect"));
}

bool Gow3SettingAppliesToRenderer(std::string_view key, Gow3RendererMode mode) {
  return Gow3MenuKeyApplies(key, mode);
}

// Release builds (GOW3_XENIA_RENDERER=0) offer only the native renderer; the
// Xenia renderer is for dev and test builds, which select it in the config or
// with --gow3_renderer=xenia.
static Gow3RendererMode AvailableRenderer(Gow3RendererMode mode) {
#if !GOW3_XENIA_RENDERER
  if (mode == Gow3RendererMode::Xenia) {
    REXLOG_WARN("This build includes only the native renderer; ignoring gow3_renderer = xenia");
    return Gow3RendererMode::Native;
  }
#endif
  return mode;
}

void Gow3LoadSettingsConfig(const std::filesystem::path& path) {
  // Renderer selection is resolved before unrelated backend flags are validated.
  if (rex::cvar::IsCommandLineOverride("gow3_renderer")) {
    const auto mode = AvailableRenderer(ParseGow3RendererMode(rex::cvar::GetFlagByName("gow3_renderer")));
    rex::cvar::SetFlagByName("gow3_renderer", std::string(Gow3RendererName(mode)));
  }
  rex::cvar::LoadConfig(path, [](rex::cvar::ConfigValues& values) {
    const auto selected = rex::cvar::IsCommandLineOverride("gow3_renderer")
        ? rex::cvar::GetFlagByName("gow3_renderer")
        : values.contains("gow3_renderer") ? values.at("gow3_renderer") : "native";
    const auto mode = AvailableRenderer(ParseGow3RendererMode(selected));
    values["gow3_renderer"] = Gow3RendererName(mode);
    // Gyro settings from before per-controller settings belong to player 1
    // (tools/settings/model.py migrates them the same way).
    for (const char* key : {"gyro_mode", "gyro_activation", "gyro_turn_axis", "gyro_sensitivity",
                            "gyro_invert_y", "gyro_stick_deadzone"}) {
      if (const auto found = values.find(key); found != values.end()) {
        values.try_emplace(std::string("controller1_") + key, found->second);
        values.erase(found);
      }
    }
    for (auto it = values.begin(); it != values.end();) {
      if (!Gow3SettingAppliesToRenderer(it->first, mode)) it = values.erase(it);
      else ++it;
    }
    // Both renderers use the original per-texture filtering. Normalize legacy
    // saved overrides before validation, including settings from older GUIs.
    values["anisotropic_override"] = "-1";
    if (mode == Gow3RendererMode::Native) {
      const std::pair<const char*, const char*> required[] = {
          {"resolution_scale", "1"}, {"draw_resolution_scale_x", "1"},
          {"draw_resolution_scale_y", "1"}, {"render_target_path_vulkan", "fsi"},
          {"occlusion_query_enable", "true"},
          {"swap_post_effect", "none"}, {"native_2x_msaa", "false"}};
      for (const auto& [key, value] : required) {
        if (rex::cvar::IsCommandLineOverride(key) && rex::cvar::GetFlagByName(key) != value)
          throw std::runtime_error(std::string("Native renderer requires --") + key + "=" + value);
        values[key] = value;
      }
    }
  });
  // Mesa's anti-lag layer crashes in vkQueuePresentKHR with the Xenia
  // renderer, so it stays off there even when requested on the command line.
  if (ParseGow3RendererMode(rex::cvar::GetFlagByName("gow3_renderer")) == Gow3RendererMode::Xenia &&
      rex::cvar::GetFlagByName("present_anti_lag") == "true") {
    REXLOG_WARN("AMD Anti-Lag is unavailable with the Xenia renderer; turning it off");
    rex::cvar::SetFlagByName("present_anti_lag", "false");
  }
  // CLI and environment overrides must not restore forced filtering either.
  if (!rex::cvar::SetFlagByName("anisotropic_override", "-1"))
    throw std::runtime_error("Unable to select original game texture filtering");
}
