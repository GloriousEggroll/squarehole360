// Exercise the actual GTKWindow against the compositor, including startup in
// fullscreen before a normal size has ever been configured by the compositor.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window_gtk.h>
#include <rex/ui/windowed_app_context_gtk.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/ui_drawer.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>

struct Test : rex::ui::UIDrawer {
  rex::ui::GTKWindowedAppContext context;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider;
  std::unique_ptr<rex::ui::Presenter> presenter;
  std::unique_ptr<rex::ui::ImmediateDrawer> drawer;
  std::unique_ptr<rex::ui::GTKWindow> window;
  std::pair<int,int> rendered_size{0,0};
  double scale = 1.0;
  unsigned stage = 0;
  bool passed = false;
  std::chrono::steady_clock::time_point deadline;
  std::chrono::steady_clock::time_point stable_since{};

  void Draw(rex::ui::UIDrawContext& draw) override {
    rendered_size = {draw.render_target_width(), draw.render_target_height()};
    float w = rendered_size.first, h = rendered_size.second;
    std::vector<rex::ui::ImmediateVertex> vertices;
    auto rectangle = [&](float x, float y, float right, float bottom, uint32_t color) {
      for (auto p : {std::pair{x,y}, {right,y}, {x,bottom}, {x,bottom}, {right,y}, {right,bottom}})
        vertices.push_back({p.first,p.second,0,0,color});
    };
    rectangle(0,0,w,h,0xFF00FF00);
    rectangle(0,0,w,8,0xFF0000FF);
    rectangle(0,h-8,w,h,0xFF0000FF);
    rectangle(0,0,8,h,0xFF0000FF);
    rectangle(w-8,0,w,h,0xFF0000FF);
    rectangle(w/2-10,h/2-10,w/2+10,h/2+10,0xFFFF0000);
    drawer->Begin(draw,w,h);
    drawer->BeginDrawBatch({vertices.data(),int(vertices.size())});
    rex::ui::ImmediateDraw command;
    command.count = vertices.size();
    drawer->Draw(command);
    drawer->EndDrawBatch();
    drawer->End();
  }
  bool CaptureStage() {
    auto now = std::chrono::steady_clock::now();
    if (stable_since == std::chrono::steady_clock::time_point{}) stable_since = now;
    if (now - stable_since < std::chrono::milliseconds(300)) return false;
    if (const char* folder = std::getenv("GOW3_WINDOW_TEST_SCREENSHOTS")) {
      std::string base = std::string(folder) + "/stage-" + std::to_string(stage);
      std::string png = base + ".png";
      gchar* args[] = {const_cast<char*>("grim"),const_cast<char*>("-o"),
                      const_cast<char*>("HEADLESS-1"),png.data(),nullptr};
      int status = -1;
      if (!g_spawn_sync(nullptr,args,nullptr,G_SPAWN_SEARCH_PATH,nullptr,nullptr,
                        nullptr,nullptr,&status,nullptr) || status != 0) {
        std::cerr << "FAIL: screenshot capture\n";
        context.QuitFromUIThread();
        return false;
      }
      std::ofstream(base + ".json") << "{\"width\":" << rendered_size.first
          << ",\"height\":" << rendered_size.second << "}\n";
    }
    return true;
  }
  bool SurfaceMatches(std::pair<int,int> logical) const {
    return rendered_size == std::pair{int(std::lround(logical.first * scale)),
                                     int(std::lround(logical.second * scale))};
  }

  std::pair<int,int> ClientSize() {
    auto* box = gtk_bin_get_child(GTK_BIN(window->window()));
    auto* children = gtk_container_get_children(GTK_CONTAINER(box));
    auto* drawing = GTK_WIDGET(g_list_last(children)->data);
    GtkAllocation allocation;
    gtk_widget_get_allocation(drawing, &allocation);
    g_list_free(children);
    return {allocation.width, allocation.height};
  }
  bool Tick() {
    if (std::chrono::steady_clock::now() > deadline) {
      auto [width, height] = ClientSize();
      std::cerr << "FAIL at stage " << stage << ": client " << width << 'x' << height
                << ", rendered surface " << rendered_size.first << 'x' << rendered_size.second << '\n';
      context.QuitFromUIThread();
      return false;
    }
    auto* gdk_window = gtk_widget_get_window(window->window());
    bool fullscreen = gdk_window && (gdk_window_get_state(gdk_window) & GDK_WINDOW_STATE_FULLSCREEN);
    const auto size = ClientSize();
    // Request painting through the normal window path, without forcing a
    // surface resize. Reading GTK allocations alone misses a stale swapchain.
    presenter->RequestUIPaintFromUIThread();
    if ((stage == 0 || stage == 3) && fullscreen &&
        rendered_size == std::pair{2560,1440} && SurfaceMatches(size)) {
      if (!CaptureStage()) return true;
      std::cout << "PASS: borderless renders at 2560x1440 (stage " << stage << ")\n";
      window->SetLogicalSize(1280, 720);
      window->SetFullscreen(false);
    } else if (stage == 1 && !fullscreen && size == std::pair{1280,720} && SurfaceMatches(size)) {
      if (!CaptureStage()) return true;
      std::cout << "PASS: startup fullscreen -> 1280x720\n";
      window->SetLogicalSize(1024,576);
    } else if (stage == 2 && !fullscreen && size == std::pair{1024,576} && SurfaceMatches(size)) {
      if (!CaptureStage()) return true;
      std::cout << "PASS: live resize -> 1024x576\n";
      window->SetFullscreen(true);
    } else if (stage == 4 && !fullscreen && size == std::pair{1280,720} && SurfaceMatches(size)) {
      if (!CaptureStage()) return true;
      std::cout << "PASS: second fullscreen -> configured 1280x720\n";
      passed = true;
      context.QuitFromUIThread();
      return false;
    } else if (stage == 5 && !fullscreen && size == std::pair{1280,720} && SurfaceMatches(size)) {
      if (!CaptureStage()) return true;
      std::cout << "PASS: windowed startup\n";
      window->SetLogicalSize(1519,793);
    } else if (stage == 6 && !fullscreen && size == std::pair{1519,793} && SurfaceMatches(size)) {
      if (!CaptureStage()) return true;
      std::cout << "PASS: windowed expansion\n";
      window->SetFullscreen(true);
    } else {
      stable_since = {};
      return true;
    }
    ++stage;
    if (stage == 7) stage = 0;
    stable_since = {};
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    return true;
  }
};

int main(int argc, char** argv) {
  if (!gtk_init_check(&argc, &argv)) return 2;
  rex::cvar::Init(argc, argv);
  rex::InitLoggingEarly();
  Test test;
  if (const char* scale = std::getenv("GOW3_WINDOW_TEST_SCALE")) test.scale = std::stod(scale);
  test.provider = rex::ui::vulkan::VulkanProvider::Create(false, true);
  if (!test.provider) return 4;
  test.presenter = test.provider->CreatePresenter();
  if (!test.presenter) return 5;
  test.drawer = test.provider->CreateImmediateDrawer();
  if (!test.drawer) return 6;
  test.drawer->SetPresenter(test.presenter.get());
  test.presenter->AddUIDrawerFromUIThread(&test, 0);
  test.window = std::make_unique<rex::ui::GTKWindow>(test.context, "Gears window-size validation", 1280, 720);
  bool start_windowed = std::getenv("GOW3_WINDOW_TEST_START_WINDOWED") != nullptr;
  test.stage = start_windowed ? 5 : 0;
  test.window->SetFullscreen(!start_windowed);
  if (!test.window->Open()) return 3;
  GdkRGBA background{0,1,1,1};
  gtk_widget_override_background_color(test.window->window(),GTK_STATE_FLAG_NORMAL,&background);
  test.window->SetPresenter(test.presenter.get());
  test.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  g_timeout_add(100, [](void* value) -> gboolean {
    return static_cast<Test*>(value)->Tick() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
  }, &test);
  test.context.RunMainGTKLoop();
  test.window.reset();
  test.presenter->RemoveUIDrawerFromUIThread(&test);
  test.drawer.reset();
  test.presenter.reset();
  test.provider.reset();
  rex::ShutdownLogging();
  return test.passed ? 0 : 1;
}
