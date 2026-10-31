#!/usr/bin/env python3
"""Generate the in-game menu from the standalone editor's settings schema."""
import json
from pathlib import Path
import sys
from model import SETTINGS, LIVE_SETTINGS
q = lambda value: json.dumps(str(value), ensure_ascii=True)
value = lambda v: ('true' if v else 'false') if isinstance(v, bool) else str(v)
lines = ['// Generated from tools/settings/model.py; do not edit.', '#pragma once',
 '#include <vector>', '#include <string>', '#include <string_view>', '#include <cmath>', '#include "gow3_renderer_mode.h"',
 'struct MenuChoice { const char* value; const char* label; };',
 'struct MenuSetting { const char* key; const char* label; const char* section; const char* help; const char* value; char type; double minimum, maximum; bool live; const char* renderer; std::vector<MenuChoice> choices; bool menu; };',
 'inline const std::vector<MenuSetting> kMenuSettings = {']
for s in SETTINGS:
    kind = 'b' if isinstance(s.default, bool) else 'f' if isinstance(s.default, float) else 'i' if isinstance(s.default, int) else 's'
    choices = ','.join('{' + q(value(v)) + ',' + q(label) + '}' for v,label in s.choices)
    lines.append('{' + ','.join(q(x) for x in (s.key,s.label,s.section,s.help,value(s.default))) +
                 f",'{kind}',{s.minimum},{s.maximum}," + ('true' if s.key in LIVE_SETTINGS else 'false') + ',' + q(s.renderer) + ',{' + choices + '},' + ('true' if s.menu else 'false') + '},')
lines.append('};')
lines.append(r'''inline bool Gow3MenuSettingApplies(const MenuSetting& setting, Gow3RendererMode mode) {
  return setting.renderer == std::string_view("shared") || setting.renderer == Gow3RendererName(mode);
}
// The menus offer only the native renderer and its settings. The renderer
// selection and Xenia-only settings stay in the schema, so the config loader
// still drops them for the other renderer, but only dev/test configs set them.
inline bool Gow3MenuSettingListed(const MenuSetting& setting) {
  return setting.menu && setting.key != std::string_view("gow3_renderer") && setting.renderer != std::string_view("xenia");
}
inline bool Gow3MenuKeyApplies(std::string_view key, Gow3RendererMode mode) {
  for (const auto& setting : kMenuSettings)
    if (setting.key == key) return Gow3MenuSettingApplies(setting, mode);
  // Unlisted native diagnostics must not activate a renderer selected as Xenia.
  if (key.starts_with("gow3_native_")) return mode == Gow3RendererMode::Native;
  return true;
}
inline bool Gow3MenuLiveApplies(const MenuSetting& setting, Gow3RendererMode running,
                               Gow3RendererMode requested) {
  return setting.live && Gow3MenuSettingApplies(setting, running) &&
      Gow3MenuSettingApplies(setting, requested);
}
inline bool Gow3MenuValidValue(const MenuSetting& setting, std::string_view value) {
  if (!setting.choices.empty()) {
    for (const auto& choice : setting.choices) if (choice.value == value) return true;
    return false;
  }
  if (setting.type == 'b') return value == "true" || value == "false";
  if (setting.type == 's') return true;
  try {
    size_t end = 0;
    const std::string text(value);
    const double number = std::stod(text, &end);
    return end == text.size() && std::isfinite(number) && number >= setting.minimum &&
        number <= setting.maximum && (setting.type != 'i' || std::floor(number) == number);
  } catch (...) { return false; }
}
''')
Path(sys.argv[1]).write_text('\n'.join(lines) + '\n')
