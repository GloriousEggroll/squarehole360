// gow3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <chrono>

#include <rex/filesystem.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/memory.h>
#include <rex/graphics/graphics_system.h>
#include "gameplay_hooks.h"
#include "gow3_content.h"
#include "gow3_identity.h"
#include "gow3_settings.h"
#include "gow3_settings_menu.h"
#include "renderer/gears_bridge.h"
#ifdef GOW3_HAS_NATIVE_RENDERER
#include "renderer/native/runtime_bridge.h"
#endif
#include <rex/ui/windowed_app_context.h>

class Gow3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Gow3App>(new Gow3App(ctx, "gow3",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    paths.config_path = Gow3SettingsPath();
    if (paths.game_data_root.empty()) {
      paths.game_data_root = rex::filesystem::GetExecutableFolder() / "game";
    }
  }

  void OnLoadConfig(const std::filesystem::path& path) override {
    Gow3LoadSettingsConfig(path);
  }

  // Before the runtime creates the profile; logging is up by now.
  void OnPostInitLogging() override { Gow3PrepareIdentity(user_data_root()); }

  void OnPostLoadXexImage() override {
    Gow3ConfigureFrameRate(runtime()->memory()->virtual_membase());
    Gow3InstallDownloadedContent(runtime()->kernel_state(), user_data_root());
  }

  void OnPostSetup() override {
    auto* graphics=dynamic_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
    Gow3StartGraphicsBridge(runtime()->memory(),graphics?graphics->command_processor():nullptr);
#ifdef GOW3_HAS_NATIVE_RENDERER
    Gow3StartNativeRenderer(graphics, cache_root());
#else
    if (ParseGow3RendererMode(REXCVAR_QUERY(std::string, gow3_renderer)) == Gow3RendererMode::Native)
      throw std::runtime_error("Native renderer is not included in this build. Select Xenia and restart.");
#endif
    Gow3ConfigureDisplay(window());
    Gow3StartSettingsWatch(window(), runtime());
    Gow3StartMenuInput(runtime(), window(), imgui_drawer(),
        [this] { ToggleSettingsOverlay(); }, [this] { return IsSettingsOverlayOpen(); });
  }

  std::function<void(rex::graphics::ShaderStorageProgress&)> CreateGraphicsPreparationTask() override {
#ifdef GOW3_HAS_NATIVE_RENDERER
    return Gow3CreateNativePipelinePrewarmTask();
#else
    return {};
#endif
  }

  void OnBeforeTitleTermination() override {
    // Guest capture hooks may hold host locks or wait for renderer capacity.
    // Quiesce them while guest threads can still unwind those critical sections.
#ifdef GOW3_HAS_NATIVE_RENDERER
    Gow3StopNativeRenderer();
#endif
    Gow3StopGraphicsBridge();
    // TerminateTitle suspends guest threads wherever they are, and one that
    // holds a host lock (object table, reservation) deadlocks the shutdown.
    // Renderer state, journal and caches are saved by now, so force the exit
    // if the normal path has not finished in time. Logging could block, so
    // report through stderr only.
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(8));
      std::fputs("gow3: shutdown did not finish within 8 s (title termination deadlock); forcing exit\n", stderr);
      std::_Exit(0);
    }).detach();
  }

  void OnShutdown() override {
    Gow3StopMenuInput(); Gow3StopSettingsWatch();
#ifdef GOW3_HAS_NATIVE_RENDERER
    Gow3StopNativeRenderer();
#endif
    Gow3StopGraphicsBridge();
  }
  void OnConfigureFonts(ImFontAtlas* atlas) override { Gow3ConfigureMenuFonts(atlas); }
  std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsOverlay(
      rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& path) override {
    return Gow3CreateSettingsMenu(drawer, path, [this] {
      app_context().CallInUIThreadDeferred([this] { ToggleSettingsOverlay(); });
    });
  }

  // Override virtual hooks for further customization:
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
};
