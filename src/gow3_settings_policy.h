#pragma once
#include "gow3_settings_schema.h"
#include <map>
#include <string>

// The saved custom ceiling is distinct from the effective 0 / monitor-Hz host
// ceiling. Acknowledging the latter as the former creates a false restart notice.
template <typename ActualValue, typename IsCommandLine>
std::map<std::string, std::string> Gow3SettingsStartupBaseline(
    const std::map<std::string, std::string>& requested, Gow3RendererMode renderer,
    ActualValue actual_value, IsCommandLine is_command_line) {
  std::map<std::string, std::string> active;
  for (const auto& [key, value] : requested)
    if (Gow3MenuKeyApplies(key, renderer)) active[key] = actual_value(key);
  for (const auto& setting : kMenuSettings)
    if (Gow3MenuSettingApplies(setting, renderer)) active[setting.key] = actual_value(setting.key);
  active["gow3_renderer"] = Gow3RendererName(renderer);
  if (actual_value("gow3_frame_limit_mode") != "custom" && !is_command_line("framerate_limit")) {
    const auto found = requested.find("framerate_limit");
    if (found != requested.end()) active["framerate_limit"] = found->second;
    else for (const auto& setting : kMenuSettings)
      if (std::string_view(setting.key) == "framerate_limit") {
        active["framerate_limit"] = std::to_string(std::stod(setting.value));
        break;
      }
  }
  return active;
}
