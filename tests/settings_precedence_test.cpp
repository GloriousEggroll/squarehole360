#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <filesystem>
#include <fstream>

REXCVAR_DEFINE_BOOL(settings_test_bool, false, "Test", "CLI precedence test");
REXCVAR_DEFINE_INT32(settings_test_number, 60, "Test", "CLI precedence test");
REXCVAR_DEFINE_STRING(settings_test_renderer, "native", "Test", "Backend selection test");

TEST_CASE("Config transforms see selectors before validating inactive settings", "[settings-config]") {
  auto path = std::filesystem::temp_directory_path() / "gow3-settings-filter.toml";
  { std::ofstream file(path); file << "settings_test_number = 'invalid for inactive backend'\n"
      "settings_test_bool = [1, 2]\nsettings_test_renderer = 'reference'\n"; }
  char program[] = "settings-test";
  char renderer[] = "--settings_test_renderer=native";
  char* args[] = {program, renderer};
  rex::cvar::Init(2, args);
  bool transformed = false;
  rex::cvar::LoadConfig(path, [&](rex::cvar::ConfigValues& values) {
    transformed = true;
    REQUIRE(values.at("settings_test_renderer") == "reference");
    REQUIRE(values.contains("settings_test_bool"));
    auto mode = rex::cvar::IsCommandLineOverride("settings_test_renderer")
        ? REXCVAR_GET(settings_test_renderer) : values.at("settings_test_renderer");
    REQUIRE(mode == "native");
    values.erase("settings_test_number");
    values.erase("settings_test_bool");
  });
  CHECK(transformed);
  CHECK(REXCVAR_GET(settings_test_renderer) == "native");
  CHECK(REXCVAR_GET(settings_test_number) == 60);
  CHECK_FALSE(REXCVAR_GET(settings_test_bool));
  rex::cvar::Init(1, args);
  rex::cvar::LoadConfig(path, [](rex::cvar::ConfigValues& values) {
    values["settings_test_renderer"] = "xenia";
    values["settings_test_number"] = "75";
    values["settings_test_bool"] = "true";
  });
  CHECK(REXCVAR_GET(settings_test_renderer) == "xenia");
  CHECK(REXCVAR_GET(settings_test_number) == 75);
  CHECK(REXCVAR_GET(settings_test_bool));
  std::filesystem::remove(path);
  rex::cvar::LoadConfig(path, [](rex::cvar::ConfigValues& values) {
    REQUIRE(values.empty());
    values["settings_test_renderer"] = "native";
    values["settings_test_number"] = "90";
  });
  CHECK(REXCVAR_GET(settings_test_renderer) == "native");
  CHECK(REXCVAR_GET(settings_test_number) == 90);
  { std::ofstream file(path); file << "settings_test_renderer = [\n"; }
  CHECK_THROWS(rex::cvar::LoadConfig(path, [](rex::cvar::ConfigValues&) {}));
  CHECK(REXCVAR_GET(settings_test_renderer) == "native");
  std::filesystem::remove(path);
  rex::cvar::ResetToDefault("settings_test_renderer");
  rex::cvar::ResetToDefault("settings_test_number");
  rex::cvar::ResetToDefault("settings_test_bool");
}

TEST_CASE("Explicit CLI values override config even when equal to defaults", "[settings-config]") {
  auto path = std::filesystem::temp_directory_path() / "gow3-settings-precedence.toml";
  { std::ofstream file(path); file << "settings_test_bool = true\nsettings_test_number = 144\n"; }
  char program[] = "settings-test";
  char boolean[] = "--settings_test_bool=false";
  char number[] = "--settings_test_number=60";
  char* args[] = {program, boolean, number};
  rex::cvar::Init(3, args);
  rex::cvar::LoadConfig(path);
  CHECK_FALSE(REXCVAR_GET(settings_test_bool));
  CHECK(REXCVAR_GET(settings_test_number) == 60);
  rex::cvar::Init(1, args);
  rex::cvar::LoadConfig(path);
  CHECK(REXCVAR_GET(settings_test_bool));
  CHECK(REXCVAR_GET(settings_test_number) == 144);
  std::filesystem::remove(path);
  rex::cvar::ResetToDefault("settings_test_bool");
  rex::cvar::ResetToDefault("settings_test_number");
}
