#pragma once
#include "gow3_settings_schema.h"
#include "menu_navigation.h"
#include <algorithm>
#include <string>
#include <vector>

// Rebuilding a renderer-specific menu keeps focus by setting identity, never by
// the old row number. Removed categories return to the first category.
struct Gow3SettingsNavigation {
  std::vector<std::string> sections;
  std::vector<std::vector<const MenuSetting*>> options;

  template <typename Enabled>
  void UpdateEnabled(MenuNavigation& navigation, Enabled enabled) const {
    std::vector<std::vector<bool>> flags;
    for (const auto& section : options) {
      flags.emplace_back();
      for (const auto* setting : section) flags.back().push_back(enabled(*setting));
    }
    navigation.SetOptions(std::move(flags));
  }

  template <typename Enabled>
  void Rebuild(Gow3RendererMode mode, MenuNavigation& navigation, Enabled enabled) {
    const std::string section_name = sections.empty() ? "" : sections[navigation.Section()];
    const bool in_options = navigation.InOptions();
    const std::string key = in_options && !options.empty()
        ? options[navigation.Section()][navigation.Option()]->key : "";
    sections.clear(); options.clear();
    for (const auto& setting : kMenuSettings) {
      if (!Gow3MenuSettingApplies(setting, mode) || !Gow3MenuSettingListed(setting)) continue;
      auto section = std::find(sections.begin(), sections.end(), setting.section);
      if (section == sections.end()) {
        sections.emplace_back(setting.section); options.emplace_back();
        section = std::prev(sections.end());
      }
      options[section - sections.begin()].push_back(&setting);
    }
    navigation = MenuNavigation{};
    UpdateEnabled(navigation, enabled);
    auto found = std::find(sections.begin(), sections.end(), section_name);
    const int index = found == sections.end() ? 0 : int(found - sections.begin());
    navigation.EnterSection(index);
    if (!in_options) navigation.Handle(MenuNavigation::Command::Back);
    else for (size_t i = 0; i < options[index].size(); ++i)
      if (options[index][i]->key == key) navigation.FocusOption(int(i));
  }
};
