#pragma once
#include <filesystem>
#include "gow3_renderer_mode.h"
namespace rex::ui { class Window; }
std::filesystem::path Gow3SettingsPath();
void Gow3ConfigureDisplay(rex::ui::Window* window);

namespace rex { class Runtime; }
void Gow3StartSettingsWatch(rex::ui::Window* window, rex::Runtime* runtime);
void Gow3StopSettingsWatch();

#include <string>
#include <vector>
struct Gow3LiveSettingsStatus { std::string digest, error; std::vector<std::string> restart; bool pending = false; };
Gow3LiveSettingsStatus Gow3GetLiveSettingsStatus();

bool Gow3SettingAppliesToRenderer(std::string_view key, Gow3RendererMode mode);

void Gow3LoadSettingsConfig(const std::filesystem::path& path);
