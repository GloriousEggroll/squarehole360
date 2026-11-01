#include "gow3_content.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xex_module.h>

namespace {
constexpr uint32_t kTitleUpdateVersion = 0x0000060B;  // 11.0.6.0, Title Update 6
constexpr char kTitleUpdateMissing[] =
    "Gears of War 3 Title Update 6 is not installed.\n\n"
    "This version of squarehole360 is built for the updated game. Start squarehole360 "
    "again to run the setup wizard, or run tools/setup-wizard.py, and add your Title Update 6 "
    "package (TU_16L625B_000001C000000.00000000001GB, or an archive that contains it).";

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

std::string Gow3TitleUpdateProblem(const std::filesystem::path& update_root) {
  std::error_code ec;
  if (update_root.empty() || !std::filesystem::is_regular_file(update_root / "default.xexp", ec))
    return kTitleUpdateMissing;
  return {};
}

std::string Gow3TitleUpdateProblem(rex::system::KernelState* kernel_state) {
  auto module = kernel_state ? kernel_state->GetExecutableModule() : nullptr;
  const auto* xex = module ? module->xex_module() : nullptr;
  const auto* info = xex ? xex->opt_execution_info() : nullptr;
  if (!info) return "The game executable could not be read.";
  if (uint32_t(info->version_value) != kTitleUpdateVersion) {
    REXLOG_ERROR("Executable version {:08X}, expected {:08X} (Title Update 6)", uint32_t(info->version_value),
                 kTitleUpdateVersion);
    return kTitleUpdateMissing;
  }
  REXLOG_INFO("Title Update 6 applied (executable version {:08X})", uint32_t(info->version_value));
  return {};
}

namespace {
// Header: version (u32 BE), map name length (u32 BE, with terminator), map
// name, then the map package GUID. Only well-formed version 3 headers change.
bool IsVersion3Checkpoint(const std::vector<char>& bytes) {
  const auto u32 = [&](size_t at) {
    return uint32_t(uint8_t(bytes[at])) << 24 | uint32_t(uint8_t(bytes[at + 1])) << 16 |
           uint32_t(uint8_t(bytes[at + 2])) << 8 | uint8_t(bytes[at + 3]);
  };
  if (bytes.size() < 8 || u32(0) != 3) return false;
  const uint32_t length = u32(4);
  if (length < 2 || length > 256 || bytes.size() < 8 + length + 16 || bytes[8 + length - 1] != 0) return false;
  for (uint32_t i = 0; i + 1 < length; ++i)
    if (bytes[8 + i] < 0x20 || bytes[8 + i] > 0x7E) return false;
  return true;
}
}  // namespace

void Gow3UpgradeCheckpoints(rex::system::KernelState* kernel_state, const std::filesystem::path& user_root) {
  std::error_code ec;
  if (!kernel_state || !std::filesystem::is_directory(user_root, ec)) return;
  const auto title = fmt::format("{:08X}", kernel_state->title_id());
  for (const auto& profile : std::filesystem::directory_iterator(user_root, ec)) {
    const auto name = profile.path().filename().string();
    if (!profile.is_directory(ec) || name.size() != 16) continue;
    const auto folder = profile.path() / title / "00000001" / "GearsCheckpoint";
    if (!std::filesystem::is_directory(folder, ec)) continue;
    const auto backup = profile.path() / title / "checkpoints-before-tu6";
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
      const auto& path = entry.path();
      if (!entry.is_regular_file(ec) || path.extension() != ".sav") continue;
      std::vector<char> bytes;
      {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), {});
      }
      if (!IsVersion3Checkpoint(bytes)) continue;
      std::filesystem::create_directories(backup, ec);
      const auto saved = backup / path.filename();
      if (!std::filesystem::exists(saved, ec) &&
          !std::filesystem::copy_file(path, saved, ec)) {
        REXLOG_ERROR("Checkpoint {}: backup failed ({}); left unchanged", path.string(), ec.message());
        continue;
      }
      bytes[3] = 4;
      const auto temporary = path.string() + ".tu6";
      {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), std::streamsize(bytes.size()));
        if (!file) {
          REXLOG_ERROR("Checkpoint {}: could not write the update", path.string());
          continue;
        }
      }
      std::filesystem::rename(temporary, path, ec);
      if (ec) {
        REXLOG_ERROR("Checkpoint {}: could not replace ({})", path.string(), ec.message());
        std::filesystem::remove(temporary, ec);
        continue;
      }
      REXLOG_INFO("Checkpoint {} updated for Title Update 6 (original in {})", path.string(), backup.string());
    }
  }
}
