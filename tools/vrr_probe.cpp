// Read-only compositor VRR status for the standalone settings editor.
#include <rex/ui/adaptive_sync.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

static std::string Quote(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
    else out << c;
  }
  return out.str() + '"';
}
int main() {
  const auto displays = rex::ui::ProbeAdaptiveSyncDisplays();
  const bool available = std::any_of(displays.begin(), displays.end(),
      [](const auto& d) { return d.available; });
  std::cout << "{\"available\":" << (available ? "true" : "false") << ",\"displays\":[";
  for (size_t i = 0; i < displays.size(); ++i) {
    const auto& d = displays[i];
    if (i) std::cout << ',';
    std::cout << "{\"name\":" << Quote(d.name) << ",\"description\":" << Quote(d.description)
              << ",\"enabled\":" << (d.enabled ? "true" : "false")
              << ",\"available\":" << (d.available ? "true" : "false")
              << ",\"reason\":" << Quote(d.reason) << '}';
  }
  std::cout << "],\"reason\":" << Quote(available ?
      "VRR is managed by the compositor; this reports configuration, not per-frame scanout." :
      "The current display server does not report VRR status.") << "}\n";
}
