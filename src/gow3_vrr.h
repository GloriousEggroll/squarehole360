#pragma once
#include <string>

namespace rex::ui { class Window; }
struct Gow3VrrStatus {
  bool available = false;
  bool enabled = false;
  bool pending = false;
  std::string output;
  std::string error;
};

// Read-only compositor status. Probes run off the UI thread; nothing changes
// monitor policy, VSync, presentation mode, or the frame-rate ceiling.
void Gow3StartVrr(rex::ui::Window* window);
bool Gow3PollVrr();
void Gow3StopVrr();
Gow3VrrStatus Gow3GetVrrStatus();
