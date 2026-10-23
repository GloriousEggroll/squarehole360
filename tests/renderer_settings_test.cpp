#include "gow3_settings_navigation.h"
#include "gow3_settings_policy.h"
#include <cassert>
#include <iostream>
#include <set>

int main() {
  assert(ParseGow3RendererMode("native") == Gow3RendererMode::Native);
  assert(ParseGow3RendererMode("reference") == Gow3RendererMode::Xenia);
  assert(ParseGow3RendererMode("xenia") == Gow3RendererMode::Xenia);
  for (auto bad : {"", "Native", "reference2"}) {
    bool rejected = false;
    try { (void)ParseGow3RendererMode(bad); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
  auto find = [](std::string_view key) -> const MenuSetting& {
    for (const auto& setting : kMenuSettings) if (setting.key == key) return setting;
    throw std::runtime_error("Missing schema key");
  };
  const auto n = Gow3RendererMode::Native, x = Gow3RendererMode::Xenia;
  for (const auto& setting : kMenuSettings)
    assert(std::string_view(setting.key) != "anisotropic_override");
  assert(!Gow3MenuLiveApplies(find("gow3_renderer"), n, x));
  assert(Gow3MenuLiveApplies(find("framerate_limit"), n, x));
  assert(!Gow3MenuLiveApplies(find("present_effect"), n, x));
  assert(!Gow3MenuLiveApplies(find("present_effect"), x, n));
  assert(Gow3MenuLiveApplies(find("present_effect"), x, x));
  assert(!Gow3MenuKeyApplies("gow3_native_raster", x));
  assert(!Gow3MenuValidValue(find("texture_cache_memory_limit_soft"), "8192"));
  assert(Gow3MenuValidValue(find("texture_cache_memory_limit_soft"), "4096"));
  assert(!Gow3MenuValidValue(find("texture_cache_memory_limit_soft"), "4096.5"));
  assert(!Gow3MenuValidValue(find("framerate_limit"), "nan"));
  assert(!Gow3MenuValidValue(find("present_effect"), "fsr4"));
  assert(!Gow3MenuValidValue(find("gow3_native_shader_prewarm_threads"), "33"));
  std::map<std::string,std::string> actual;
  for (const auto& setting : kMenuSettings)
    actual[setting.key] = setting.type == 'f' ? std::to_string(std::stod(setting.value)) : setting.value;
  for (const auto mode : {"unlimited", "display", "custom"}) {
    actual["gow3_frame_limit_mode"] = mode;
    actual["framerate_limit"] = std::string_view(mode) == "unlimited" ? "0.000000" :
        std::string_view(mode) == "display" ? "144.000000" : "60.000000";
    const std::map<std::string,std::string> requested{{"gow3_renderer","native"},
        {"gow3_frame_limit_mode",mode},{"framerate_limit","60.000000"}};
    const auto active = Gow3SettingsStartupBaseline(requested,n,
        [&](std::string_view key) { return actual.at(std::string(key)); },
        [](std::string_view) { return false; });
    for (const auto& [key,value] : requested) assert(active.at(key) == value);
    // Newly added options omitted by old config files are already acknowledged.
    assert(active.at("gow3_native_pipeline_prewarm") == "true");
    assert(active.at("gow3_native_shader_prewarm") == "true");
    assert(active.at("gow3_native_shader_prewarm_threads") == "0");
    assert(!active.contains("present_effect"));
    const auto cli = Gow3SettingsStartupBaseline(requested,n,
        [&](std::string_view key) { return actual.at(std::string(key)); },
        [](std::string_view key) { return key == "framerate_limit"; });
    assert(cli.at("framerate_limit") == actual.at("framerate_limit"));
  }
  actual["gow3_frame_limit_mode"] = "unlimited";
  actual["framerate_limit"] = "0.000000";
  const auto fresh = Gow3SettingsStartupBaseline({},n,
      [&](std::string_view key) { return actual.at(std::string(key)); },
      [](std::string_view) { return false; });
  assert(fresh.at("framerate_limit") == "60.000000");
  Gow3SettingsNavigation layout;
  MenuNavigation navigation;
  auto enabled = [](const MenuSetting& setting) { return std::string_view(setting.key) != "present_hdr_peak_nits"; };
  layout.Rebuild(x, navigation, enabled);
  auto focus = [&](std::string_view key) {
    for (size_t section = 0; section < layout.options.size(); ++section)
      for (size_t row = 0; row < layout.options[section].size(); ++row)
        if (layout.options[section][row]->key == key) {
          navigation.EnterSection(int(section)); navigation.FocusOption(int(row)); return;
        }
    assert(false);
  };
  focus("present_dither");
  layout.Rebuild(n, navigation, enabled);
  assert(navigation.InOptions());
  assert(std::string_view(layout.options[navigation.Section()][navigation.Option()]->key) == "present_dither");
  for (int repeat = 0; repeat < 20; ++repeat) {
    const auto mode = repeat % 2 ? n : x;
    layout.Rebuild(mode, navigation, enabled);
    std::set<std::string> keys;
    for (const auto& section : layout.options) {
      assert(!section.empty());
      for (auto* setting : section) {
        assert(Gow3MenuSettingApplies(*setting, mode));
        assert(keys.insert(setting->key).second);
      }
    }
    assert(keys.contains("gow3_renderer") && keys.contains("present_hdr") && keys.contains("mnk_mode"));
    assert(!keys.contains("anisotropic_override"));
    assert(keys.contains("present_effect") == (mode == x));
    assert(keys.contains("gow3_native_pipeline_prewarm") == (mode == n));
    for (int press = 0; press < 120; ++press) {
      navigation.Handle(MenuNavigation::Command::Down);
      if (navigation.InOptions()) {
        assert(navigation.Option() >= 0);
        assert(enabled(*layout.options[navigation.Section()][navigation.Option()]));
      }
    }
  }
  layout.Rebuild(x, navigation, enabled);
  focus("texture_cache_memory_limit_soft");
  layout.Rebuild(n, navigation, enabled);
  assert(navigation.InOptions());
  assert(Gow3MenuSettingApplies(*layout.options[navigation.Section()][navigation.Option()], n));
  navigation.Handle(MenuNavigation::Command::Back);
  layout.Rebuild(x, navigation, enabled);
  assert(!navigation.InOptions());
  std::cout << "Renderer settings policy and filtered controller navigation PASS\n";
}
