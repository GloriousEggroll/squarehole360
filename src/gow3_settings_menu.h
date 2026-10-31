#pragma once
#include <functional>
#include <memory>
#include <filesystem>
namespace rex { class Runtime; namespace ui { class Window; class ImGuiDrawer; class ImGuiDialog; } }
struct ImFontAtlas;
// exit_game closes the game window, the same clean shutdown as closing it.
std::unique_ptr<rex::ui::ImGuiDialog> Gow3CreateSettingsMenu(rex::ui::ImGuiDrawer* drawer,
    const std::filesystem::path& path, std::function<void()> close, std::function<void()> exit_game);
void Gow3ConfigureMenuFonts(ImFontAtlas* atlas);
void Gow3StartMenuInput(rex::Runtime* runtime, rex::ui::Window* window,
    rex::ui::ImGuiDrawer* drawer, std::function<void()> toggle, std::function<bool()> is_open);
void Gow3StopMenuInput();
