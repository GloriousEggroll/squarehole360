#include "gow3_settings_menu.h"
#include "menu_guide_latch.h"
#include "gow3_settings_navigation.h"
#include "gow3_settings.h"
#include "gow3_vrr.h"
#include "gow3_settings_schema.h"
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/hdr_display.h>
#include <rex/runtime.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <imgui.h>
#include <toml++/toml.hpp>
#include "gow3_host.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include <cmath>
#include <iomanip>
#include <optional>

namespace {
ImFont* title_font = nullptr;
const ImVec4 red(0.72f, 0.035f, 0.055f, 1);
std::string ReadFile(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return {};
  std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("Unable to read settings.");
  return {std::istreambuf_iterator<char>(f), {}};
}
std::string Hash(const std::string& text) {
  return Gow3Sha256Hex(text);
}
class GameSettings final : public rex::ui::ImGuiDialog {
 public:
  GameSettings(rex::ui::ImGuiDrawer* drawer, std::filesystem::path path, std::function<void()> close)
      : ImGuiDialog(drawer), path_(std::move(path)), close_(std::move(close)) {
    // Controller navigation is an explicit category/options hierarchy. ImGui's
    // default navigation would also consume A/B and move between child windows.
    navigation_flags_ = GetIO().ConfigFlags & kNavigationFlags;
    GetIO().ConfigFlags &= ~kNavigationFlags;
    for (const auto& setting : kMenuSettings) values_[setting.key] = setting.value;
    RebuildOptions();
    saved_ = values_;
    Load();
  }
  ~GameSettings() override {
    GetIO().ConfigFlags = (GetIO().ConfigFlags & ~kNavigationFlags) | navigation_flags_;
  }
 private:
  static constexpr int kNavigationFlags = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
  void Load() {
    try {
      original_exists_ = std::filesystem::exists(path_);
      original_ = ReadFile(path_); table_ = toml::parse(original_);
      const auto mode = ParseGow3RendererMode(table_["gow3_renderer"].value_or(std::string("native")));
      for (const auto& s : kMenuSettings) {
        auto* node = table_.get(s.key);
        std::string value(s.value);
        bool valid = !node;
        if (node) {
          if (s.type == 's' && node->is_string()) { value = node->as_string()->get(); valid = true; }
          else if (s.type == 'b' && node->is_boolean()) { value = node->as_boolean()->get() ? "true" : "false"; valid = true; }
          else if (s.type == 'i' && node->is_integer()) { value = std::to_string(node->as_integer()->get()); valid = true; }
          else if (s.type == 'f' && node->is_number()) { value = std::to_string(node->value<double>().value()); valid = true; }
        }
        if (std::string_view(s.key) == "gow3_renderer") value = Gow3RendererName(mode);
        valid = valid && Gow3MenuValidValue(s, value);
        if (!valid && Gow3MenuSettingApplies(s, mode))
          throw std::runtime_error(std::string("Invalid setting: ") + s.label);
        values_[s.key] = valid ? value : s.value;
      }
      RebuildOptions();
      saved_ = values_; error_.clear();
    } catch (const std::exception& e) { error_ = e.what(); }
  }
  void Apply() {
    try {
      if (std::filesystem::exists(path_) != original_exists_ || ReadFile(path_) != original_) throw std::runtime_error("Settings changed in the other editor. Select Reload before applying.");
      const auto mode = ParseGow3RendererMode(values_.at("gow3_renderer"));
      if (mode == Gow3RendererMode::Xenia && std::stoi(values_["texture_cache_memory_limit_soft"]) > std::stoi(values_["texture_cache_memory_limit_hard"]))
        throw std::runtime_error("Texture cache target must not exceed its maximum.");
      if (values_["gow3_frame_limit_mode"] == "custom" && std::stod(values_["framerate_limit"]) < 1)
        throw std::runtime_error("Custom frame limit must be at least 1 FPS.");
      for (auto& s : kMenuSettings) {
        if (!Gow3MenuSettingApplies(s, mode) && values_.at(s.key) == saved_.at(s.key)) continue;
        const auto& v = values_.at(s.key);
        if (s.type == 'b') table_.insert_or_assign(s.key, v == "true");
        else if (s.type == 'i') table_.insert_or_assign(s.key, std::stoi(v));
        else if (s.type == 'f') table_.insert_or_assign(s.key, std::stod(v));
        else table_.insert_or_assign(s.key, v);
      }
      table_.insert_or_assign("anisotropic_override", -1);
      auto backup = path_; backup.replace_extension(".toml.bak");
      if (original_exists_) std::filesystem::copy_file(path_, backup, std::filesystem::copy_options::overwrite_existing);
      if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
      std::ostringstream formatted; formatted << table_;
      const auto text = formatted.str();
      Gow3WriteFileAtomic(path_, text);
      original_ = text; original_exists_ = true; saved_ = values_; awaiting_ = Hash(text); error_.clear();
    } catch (const std::exception& e) { error_ = e.what(); }
  }
  bool Enabled(const MenuSetting& s) {
    const std::string key(s.key);
    if (key.starts_with("present_hdr")) return rex::ui::IsNativeHdrAvailable() &&
        (key == "present_hdr" || values_["present_hdr"] == "true");
    if (key == "framerate_limit") return values_["gow3_frame_limit_mode"] == "custom";
    if (key == "window_width" || key == "window_height") return values_["fullscreen"] == "false";
    if (key.starts_with("present_fsr_")) return values_["present_effect"] == "fsr";
    if (key == "present_cas_additional_sharpness") return values_["present_effect"] == "cas";
    if (key == "readback_memexport_fast") return values_["readback_memexport"] == "true";
    if (key == "vulkan_batch_occlusion_queries") return values_["occlusion_query_enable"] == "true" && values_["vulkan_async_occlusion_queries"] == "true";
    if (key == "vulkan_async_shader_translation") return values_["async_shader_compilation"] == "true";
    if (key == "gow3_native_shader_prewarm_threads") return values_["gow3_native_shader_prewarm"] == "true" || values_["gow3_native_pipeline_prewarm"] == "true";
    return true;
  }
  void RebuildOptions() {
    layout_.Rebuild(ParseGow3RendererMode(values_.at("gow3_renderer")), navigation_,
                    [this](const MenuSetting& setting) { return Enabled(setting); });
    visible_renderer_ = values_.at("gow3_renderer");
    drawn_section_ = -1; scroll_to_focus_ = true;
  }
  void RefreshNavigation() {
    if (visible_renderer_ != values_.at("gow3_renderer")) RebuildOptions();
    layout_.UpdateEnabled(navigation_, [this](const MenuSetting& setting) { return Enabled(setting); });
  }
  void ChangeValue(const MenuSetting& s, int direction, bool fast = false) {
    if (!Enabled(s)) return;
    auto& value = values_.at(s.key);
    if (!s.choices.empty()) {
      auto selected = std::find_if(s.choices.begin(), s.choices.end(),
          [&](const auto& choice) { return value == choice.value; });
      const int count = int(s.choices.size());
      const int index = selected == s.choices.end() ? 0 : int(selected - s.choices.begin());
      value = s.choices[(index + direction + count) % count].value;
    } else if (s.type == 'b') {
      value = value == "true" ? "false" : "true";
    } else {
      const std::string key(s.key);
      double step = s.type == 'i' ? 1 : .01;
      if (key == "window_width" || key == "window_height" ||
          key == "texture_cache_memory_limit_soft" || key == "texture_cache_memory_limit_hard") step = 16;
      if (key == "framerate_limit" || key == "present_hdr_paper_white") step = 1;
      if (key == "present_hdr_peak_nits") step = 10;
      if (fast) step *= 10;
      double minimum = key == "framerate_limit" ? 1 : s.minimum;
      double changed = std::clamp(std::stod(value) + direction * step, minimum, s.maximum);
      value = s.type == 'i' ? std::to_string(int(changed)) : std::to_string(changed);
    }
  }
  std::string DisplayValue(const MenuSetting& s) {
    const auto& value = values_.at(s.key);
    for (const auto& choice : s.choices) if (value == choice.value) return choice.label;
    if (s.type == 'b') {
      if (std::string(s.key) == "present_hdr" && !rex::ui::IsNativeHdrAvailable()) return "UNAVAILABLE";
      return value == "true" ? "ON" : "OFF";
    }
    if (s.type == 'f') {
      std::ostringstream formatted;
      formatted << std::fixed << std::setprecision(2) << std::stod(value);
      return formatted.str();
    }
    if (std::string(s.key) == "vulkan_pipeline_creation_threads" && value == "-1") return "ALL AVAILABLE";
    if (std::string(s.key) == "monitor" && value == "0") return "AUTOMATIC";
    return value;
  }
  std::string VrrStatusText() {
    const auto status = Gow3GetVrrStatus();
    std::string text = "DISPLAY VRR: ";
    if (!status.available) text += "UNAVAILABLE";
    else text += status.enabled ? "ENABLED" : "DISABLED";
    if (status.pending) text += " (checking)";
    if (!status.output.empty()) text += " (" + status.output + ")";
    if (!status.error.empty()) text += " — " + status.error;
    text += ". The compositor controls VRR automatically; desktop policy is unchanged. Reported Enabled does not confirm variable scanout on every frame.";
    return text;
  }
  void HandleNavigation(ImGuiIO& io) {
    using Command = MenuNavigation::Command;
    auto pressed = [](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    std::optional<Command> command;
    if (pressed(ImGuiKey_GamepadFaceRight) || pressed(ImGuiKey_Escape)) command = Command::Back;
    else if (pressed(ImGuiKey_GamepadFaceLeft) || (io.KeyCtrl && pressed(ImGuiKey_Enter))) command = Command::Apply;
    else if (pressed(ImGuiKey_GamepadFaceUp) || pressed(ImGuiKey_F6)) command = Command::Reload;
    else if (pressed(ImGuiKey_GamepadFaceDown) || pressed(ImGuiKey_Enter) || pressed(ImGuiKey_Space)) command = Command::Select;
    else if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_GamepadLStickUp, true) || pressed(ImGuiKey_UpArrow, true)) command = Command::Up;
    else if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_GamepadLStickDown, true) || pressed(ImGuiKey_DownArrow, true)) command = Command::Down;
    else if (pressed(ImGuiKey_GamepadDpadLeft, true) || pressed(ImGuiKey_GamepadLStickLeft, true) || pressed(ImGuiKey_LeftArrow, true)) command = Command::Left;
    else if (pressed(ImGuiKey_GamepadDpadRight, true) || pressed(ImGuiKey_GamepadLStickRight, true) || pressed(ImGuiKey_RightArrow, true)) command = Command::Right;
    if (!command) return;
    const auto action = navigation_.Handle(*command);
    using Action = MenuNavigation::Action;
    if (action == Action::Close) close_();
    else if (action == Action::Apply) Apply();
    else if (action == Action::Reload) Load();
    else if (action == Action::Decrease || action == Action::Increase) {
      ChangeValue(*layout_.options[navigation_.Section()][navigation_.Option()], action == Action::Decrease ? -1 : 1,
          ImGui::IsKeyDown(ImGuiKey_GamepadR1) || io.KeyShift);
    }
    scroll_to_focus_ = true;
  }
  void OnDraw(ImGuiIO& io) override {
    RefreshNavigation();
    HandleNavigation(io);
    RefreshNavigation();
    const float width = std::min(1050.0f, io.DisplaySize.x - 48);
    const float height = std::min(780.0f, io.DisplaySize.y - 48);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_Always, ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28,24));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10,7));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(14,12));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.95f,.95f,.93f,1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.025f,.025f,.025f,.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, red);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.12f,.12f,.12f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.25f,.06f,.07f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, red);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.18f,.025f,.035f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, red);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, red);
    ImGui::PushStyleColor(ImGuiCol_Header, red);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(.45f,.03f,.04f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, red);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1,1,1,1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, red);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(.95f,.1f,.12f,1));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, red);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(.06f,.06f,.06f,1));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(.35f,.025f,.035f,1));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, red);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, red);
    ImGui::Begin("##gow3_settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    if (first_) { ImGui::SetWindowFocus(); first_ = false; }
    if (title_font) ImGui::PushFont(title_font);
    ImGui::TextUnformatted("GEARS OF WAR");
    ImGui::SameLine();
    ImGui::TextColored(red, "3");
    if (title_font) ImGui::PopFont();
    ImGui::TextUnformatted("SETTINGS");
    auto pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + ImGui::GetContentRegionAvail().x, pos.y + 4), ImGui::ColorConvertFloat4ToU32(red));
    ImGui::Dummy(ImVec2(0,12));
    const float footer = 180;
    ImGui::BeginChild("sections", ImVec2(165, -footer), false);
    for (int index = 0; index < int(layout_.sections.size()); ++index) {
      std::string label = layout_.sections[index]; for (auto& c : label) c = char(std::toupper(c));
      bool selected = navigation_.Section() == index;
      if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0,34))) {
        navigation_.EnterSection(index);
        scroll_to_focus_ = true;
      }
      if (selected && !navigation_.InOptions()) {
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255,255,255,255), 0, 0, 2);
        if (scroll_to_focus_) ImGui::SetScrollHereY();
      }
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("options", ImVec2(0,-footer), false);
    if (drawn_section_ != navigation_.Section()) { ImGui::SetScrollY(0); drawn_section_ = navigation_.Section(); }
    if (layout_.sections[navigation_.Section()] == "Display") {
      ImGui::TextWrapped("%s", VrrStatusText().c_str());
      ImGui::Separator();
    }
    const auto& options = layout_.options[navigation_.Section()];
    for (int index = 0; index < int(options.size()); ++index) {
      const auto& s = *options[index];
      ImGui::PushID(s.key);
      bool enabled = Enabled(s);
      if (!enabled) ImGui::BeginDisabled();
      const auto position = ImGui::GetCursorScreenPos();
      const ImVec2 size(ImGui::GetContentRegionAvail().x, 70);
      if (ImGui::InvisibleButton("##option", size)) {
        navigation_.FocusOption(index);
        const float mouse_x = io.MousePos.x - position.x;
        if (mouse_x < 38) ChangeValue(s, -1, io.KeyShift);
        else if (mouse_x > size.x - 38 || s.type == 'b' || !s.choices.empty()) ChangeValue(s, 1, io.KeyShift);
      }
      const bool focused = navigation_.InOptions() && navigation_.Option() == index;
      auto* draw = ImGui::GetWindowDrawList();
      const ImVec2 end(position.x + size.x, position.y + size.y);
      draw->AddRectFilled(position, end, ImGui::GetColorU32(focused ? ImVec4(.32f,.025f,.04f,1) : ImVec4(.1f,.1f,.1f,1)));
      if (focused) draw->AddRect(position, end, IM_COL32(255,255,255,255), 0, 0, 2);
      const auto color = ImGui::GetColorU32(ImGuiCol_Text);
      draw->AddText(ImVec2(position.x+14, position.y+7), color, s.label);
      draw->AddText(ImVec2(position.x+44, position.y+39), color, DisplayValue(s).c_str());
      if (enabled) {
        draw->AddText(ImVec2(position.x+14, position.y+39), color, "<");
        draw->AddText(ImVec2(end.x-26, position.y+39), color, ">");
      }
      if (focused && scroll_to_focus_) ImGui::SetScrollHereY();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s\n%s", s.help, s.live ? "Applies while playing" : "Requires a game restart");
      }
      if (!enabled) ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndChild();
    scroll_to_focus_ = false;
    ImGui::BeginChild("help", ImVec2(0,60), false);
    if (navigation_.InOptions()) {
      const auto& setting = *options[navigation_.Option()];
      ImGui::TextWrapped("%s %s", setting.help, setting.live ? "Applies while playing." : "Requires a game restart.");
    } else if (layout_.sections[navigation_.Section()] == "Display") {
      ImGui::TextWrapped("%s", VrrStatusText().c_str());
    } else {
      ImGui::TextWrapped("Select a category with A. Move through options with the D-pad or left stick, then use left/right to change a value.");
    }
    ImGui::EndChild();
    auto status = Gow3GetLiveSettingsStatus();
    if (!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
    else if (!status.error.empty()) ImGui::TextWrapped("%s", status.error.c_str());
    else if (!awaiting_.empty() && (status.pending || status.digest != awaiting_)) ImGui::TextUnformatted("APPLYING CHANGES...");
    else if (!status.restart.empty()) ImGui::TextUnformatted("GAME RESTART NEEDED FOR SOME CHANGES");
    else if (values_ != saved_) ImGui::TextUnformatted("UNAPPLIED CHANGES");
    else ImGui::TextUnformatted("READY");
    if (ImGui::Button("X  APPLY", ImVec2(150,38))) Apply(); ImGui::SameLine();
    if (ImGui::Button("Y  RELOAD", ImVec2(150,38))) Load(); ImGui::SameLine();
    if (ImGui::Button(navigation_.InOptions() ? "B  BACK" : "B  CLOSE", ImVec2(150,38))) {
      if (navigation_.Handle(MenuNavigation::Command::Back) == MenuNavigation::Action::Close) close_();
    }
    ImGui::TextUnformatted("A  SELECT     B  BACK     RB  FASTER ADJUSTMENT     L3+R3 / F4  CLOSE");
    ImGui::End();
    ImGui::PopStyleColor(20); ImGui::PopStyleVar(6);
  }
  std::filesystem::path path_;
  std::function<void()> close_;
  toml::table table_;
  std::string original_, error_, awaiting_, visible_renderer_;
  std::map<std::string,std::string> values_, saved_;
  Gow3SettingsNavigation layout_;
  MenuNavigation navigation_;
  int navigation_flags_ = 0, drawn_section_ = -1;
  bool scroll_to_focus_ = true;
  bool first_ = true, original_exists_ = false;
};
struct MenuInput {
  rex::input::InputSystem* input;
  rex::ui::Window* window;
  rex::ui::ImGuiDrawer* drawer;
  std::function<void()> toggle;
  std::function<bool()> is_open;
  MenuStickChordLatch shortcut;
  Gow3UiTimer timer;
};
std::unique_ptr<MenuInput> menu_input;
void PollMenu() {
  auto& m = *menu_input;
  rex::input::X_INPUT_STATE state{};
  bool connected = m.input->GetHostState(0, &state) == 0;
  uint16_t buttons = connected ? uint16_t(state.gamepad.buttons) : 0;
  const bool left_stick = (buttons & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) != 0;
  const bool right_stick = (buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB) != 0;
  if (m.shortcut.Update(left_stick, right_stick, m.window->HasFocus())) {
    m.toggle();
    REXLOG_INFO("L3+R3 toggled the settings menu");
  }
  auto& io = m.drawer->GetIO();
  const bool active = connected && m.is_open() && m.window->HasFocus();
  if (m.is_open()) {
    if (connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  }
  const std::pair<ImGuiKey,uint16_t> keys[] = {
    {ImGuiKey_GamepadFaceDown,rex::input::X_INPUT_GAMEPAD_A},
    {ImGuiKey_GamepadFaceRight,rex::input::X_INPUT_GAMEPAD_B},
    {ImGuiKey_GamepadFaceLeft,rex::input::X_INPUT_GAMEPAD_X},
    {ImGuiKey_GamepadFaceUp,rex::input::X_INPUT_GAMEPAD_Y},
    {ImGuiKey_GamepadDpadUp,rex::input::X_INPUT_GAMEPAD_DPAD_UP},
    {ImGuiKey_GamepadDpadDown,rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
    {ImGuiKey_GamepadDpadLeft,rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
    {ImGuiKey_GamepadDpadRight,rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
    {ImGuiKey_GamepadL1,rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {ImGuiKey_GamepadR1,rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER}};
  for (auto [key, mask] : keys) io.AddKeyEvent(key, active && (buttons & mask));
  const float x = active ? int16_t(state.gamepad.thumb_lx) / 32767.0f : 0;
  const float y = active ? int16_t(state.gamepad.thumb_ly) / 32767.0f : 0;
  const std::pair<ImGuiKey, float> stick[] = {
    {ImGuiKey_GamepadLStickLeft, -x}, {ImGuiKey_GamepadLStickRight, x},
    {ImGuiKey_GamepadLStickUp, y}, {ImGuiKey_GamepadLStickDown, -y}};
  for (auto [key, value] : stick) io.AddKeyAnalogEvent(key, value > .25f,
      std::clamp((value - .25f) / .75f, 0.0f, 1.0f));
  return;
}
}
std::unique_ptr<rex::ui::ImGuiDialog> Gow3CreateSettingsMenu(rex::ui::ImGuiDrawer* drawer,
    const std::filesystem::path& path, std::function<void()> close) {
  return std::make_unique<GameSettings>(drawer,path,std::move(close));
}
void Gow3ConfigureMenuFonts(ImFontAtlas* atlas) {
  const auto regular = Gow3MenuFontPath(false).string();
  const auto bold = Gow3MenuFontPath(true).string();
  // The default ImGui range omits curly apostrophes, dashes and other characters
  // in the shared settings schema. Keep the ranges alive until the atlas builds.
  static ImVector<ImWchar> ranges;
  ImFontGlyphRangesBuilder builder;
  builder.AddRanges(atlas->GetGlyphRangesDefault());
  for (const auto& setting : kMenuSettings) {
    builder.AddText(setting.label);
    builder.AddText(setting.help);
    builder.AddText(setting.section);
    for (const auto& choice : setting.choices) builder.AddText(choice.label);
  }
  builder.BuildRanges(&ranges);
  if (!regular.empty()) ImGui::GetIO().FontDefault = atlas->AddFontFromFileTTF(regular.c_str(),20,nullptr,ranges.Data);
  if (!bold.empty()) title_font = atlas->AddFontFromFileTTF(bold.c_str(),42);
}
void Gow3StartMenuInput(rex::Runtime* runtime, rex::ui::Window* window,
    rex::ui::ImGuiDrawer* drawer, std::function<void()> toggle, std::function<bool()> is_open) {
  if (!runtime || !drawer || !window) return;
  auto* input = dynamic_cast<rex::input::InputSystem*>(runtime->input_system());
  if (!input) return;
  menu_input = std::make_unique<MenuInput>(input,window,drawer,std::move(toggle),std::move(is_open));
  menu_input->timer.Start(window->app_context(), std::chrono::milliseconds(16), PollMenu);
}
void Gow3StopMenuInput() { if(menu_input) {menu_input->timer.Stop();menu_input.reset();} }
