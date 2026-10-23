#pragma once
#include <functional>
#include <memory>
#include <filesystem>
namespace rex { class Runtime; namespace ui { class Window; class ImGuiDrawer; class ImGuiDialog; } }
struct ImFontAtlas;
std::unique_ptr<rex::ui::ImGuiDialog> Gow3CreateSettingsMenu(rex::ui::ImGuiDrawer* drawer,
    const std::filesystem::path& path, std::function<void()> close);
void Gow3ConfigureMenuFonts(ImFontAtlas* atlas);
void Gow3StartMenuInput(rex::Runtime* runtime, rex::ui::Window* window,
    rex::ui::ImGuiDrawer* drawer, std::function<void()> toggle, std::function<bool()> is_open);
void Gow3StopMenuInput();
