#include "gow3_settings.h"
#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <filesystem>
#include <fstream>
#include <map>

REXCVAR_DEFINE_BOOL(gow3_native_settings_test, true, "Test", "Renderer scope fixture");

TEST_CASE("Renderer settings load only the selected backend before validation", "[renderer-settings]") {
  const auto path = std::filesystem::temp_directory_path() / "gow3-renderer-settings-test.toml";
  const char* keys[] = {"gow3_renderer", "resolution_scale", "draw_resolution_scale_x",
      "draw_resolution_scale_y", "render_target_path_vulkan", "occlusion_query_enable",
      "present_effect", "swap_post_effect", "native_2x_msaa",
      "texture_cache_memory_limit_soft", "gow3_native_settings_test", "anisotropic_override"};
  std::map<std::string, std::string> previous;
  for (const auto* key : keys) previous[key] = rex::cvar::GetFlagByName(key);
  char program[] = "renderer-settings-test";
  char* initial[] = {program};
  struct Restore {
    const std::filesystem::path& path;
    const std::map<std::string,std::string>& previous;
    char** args;
    ~Restore() {
      rex::cvar::Init(1, args);
      for (const auto& [key, value] : previous) rex::cvar::SetFlagByName(key, value);
      std::error_code error; std::filesystem::remove(path, error);
    }
  } restore{path,previous,initial};
  rex::cvar::Init(1, initial);
  for (const auto* key : keys) rex::cvar::ResetToDefault(key);
  { std::ofstream file(path); file << R"(gow3_renderer = "native"
resolution_scale = ["ignored-invalid"]
draw_resolution_scale_x = 3
render_target_path_vulkan = "fbo"
occlusion_query_enable = false
present_effect = "fsr"
swap_post_effect = "fxaa"
texture_cache_memory_limit_soft = 8192
anisotropic_override = 5
)"; }
  REQUIRE_NOTHROW(Gow3LoadSettingsConfig(path));
  CHECK(rex::cvar::GetFlagByName("gow3_renderer") == "native");
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "1");
  CHECK(rex::cvar::GetFlagByName("draw_resolution_scale_x") == "1");
  CHECK(rex::cvar::GetFlagByName("render_target_path_vulkan") == "fsi");
  CHECK(rex::cvar::GetFlagByName("occlusion_query_enable") == "true");
  CHECK(rex::cvar::GetFlagByName("present_effect") == "bilinear");
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
  CHECK(rex::cvar::GetFlagByName("swap_post_effect") == "none");
  CHECK(rex::cvar::GetFlagByName("texture_cache_memory_limit_soft") == "384");
  { std::ofstream file(path); file << R"(gow3_renderer = "reference"
resolution_scale = 2
present_effect = "fsr"
gow3_native_settings_test = false
texture_cache_memory_limit_soft = 1024
anisotropic_override = 3
)"; }
  REQUIRE_NOTHROW(Gow3LoadSettingsConfig(path));
  CHECK(rex::cvar::GetFlagByName("gow3_renderer") == "xenia");
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "2");
  CHECK(rex::cvar::GetFlagByName("present_effect") == "fsr");
  CHECK(REXCVAR_GET(gow3_native_settings_test));
  CHECK(rex::cvar::GetFlagByName("texture_cache_memory_limit_soft") == "1024");
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
  char forced_filter[] = "--anisotropic_override=5";
  char* forced_args[] = {program, forced_filter};
  rex::cvar::Init(2, forced_args);
  REQUIRE(rex::cvar::GetFlagByName("anisotropic_override") == "5");
  Gow3LoadSettingsConfig(path);
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
  char legacy[] = "--gow3_renderer=reference";
  char* alias_args[] = {program, legacy};
  rex::cvar::Init(2, alias_args);
  { std::ofstream file(path); file << "gow3_renderer = \"native\"\nresolution_scale = 3\n"; }
  Gow3LoadSettingsConfig(path);
  CHECK(rex::cvar::GetFlagByName("gow3_renderer") == "xenia");
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "3");
  char native[] = "--gow3_renderer=native", scaled[] = "--resolution_scale=2";
  char* incompatible[] = {program,native,scaled};
  rex::cvar::Init(3,incompatible);
  CHECK_THROWS_AS(Gow3LoadSettingsConfig(path), std::runtime_error);
  rex::cvar::Init(1,initial);
  std::filesystem::remove(path);
  REQUIRE(rex::cvar::SetFlagByName("anisotropic_override", "5"));
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "5");
  REQUIRE_NOTHROW(Gow3LoadSettingsConfig(path));
  CHECK(rex::cvar::GetFlagByName("gow3_renderer") == "native");
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "1");
  CHECK(rex::cvar::GetFlagByName("present_effect") == "bilinear");
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
  { std::ofstream file(path); file << "gow3_renderer = [\"invalid\"]\n"; }
  CHECK_THROWS_AS(Gow3LoadSettingsConfig(path), std::invalid_argument);
}
