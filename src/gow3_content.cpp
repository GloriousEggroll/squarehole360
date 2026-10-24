#include "gow3_content.h"

#include <array>
#include <cstring>
#include <fstream>
#include <system_error>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

namespace {
// Signed by Microsoft (LIVE, PIRS) or by a console (CON).
bool IsContentPackage(const std::filesystem::path& path) {
  std::array<char, 4> magic{};
  std::ifstream file(path, std::ios::binary);
  if (!file.read(magic.data(), magic.size())) return false;
  return !std::memcmp(magic.data(), "LIVE", 4) || !std::memcmp(magic.data(), "PIRS", 4) ||
         !std::memcmp(magic.data(), "CON ", 4);
}
}  // namespace

void Gow3InstallDownloadedContent(rex::system::KernelState* kernel_state,
                                  const std::filesystem::path& user_root) {
  const auto source = user_root / "dlc";
  std::error_code ec;
  if (!kernel_state || !std::filesystem::is_directory(source, ec)) return;
  const auto title = fmt::format("{:08X}", kernel_state->title_id());
  // The header is written last, so a package without one was cut short.
  const auto headers = user_root / "0000000000000000" / title / "Headers" / "00000002";
  for (const auto& entry : std::filesystem::directory_iterator(source, ec)) {
    const auto& path = entry.path();
    if (!entry.is_regular_file(ec) || !IsContentPackage(path)) continue;
    if (std::filesystem::exists(headers / (path.filename().string() + ".header"), ec)) continue;
    REXLOG_INFO("Installing downloadable content {} ({} MB)", path.filename().string(),
                entry.file_size(ec) >> 20);
    const auto result = kernel_state->content_manager()->InstallContent(path);
    if (result != 0) {
      REXLOG_ERROR("Could not install downloadable content {}: {:08X}", path.filename().string(),
                   result);
    }
  }
}
