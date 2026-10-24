#include "gow3_identity.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <system_error>

#include <array>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(std::string, user_xuid);
REXCVAR_DECLARE(std::string, user_gamertag);

#include "../vendor/rexglue-sdk/thirdparty/crypto/TinySHA1.hpp"

namespace {
constexpr char kIdentityFile[] = "identity.toml";

std::optional<uint64_t> ReadIdentity(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    // xuid = "E0001234ABCD5678"
    const auto key = line.find("xuid");
    const auto open = line.find('"');
    const auto close = line.rfind('"');
    if (key == 0 && open != std::string::npos && close > open)
      return Gow3ParseXuid(std::string_view(line).substr(open + 1, close - open - 1));
  }
  return std::nullopt;
}

bool WriteIdentity(const std::filesystem::path& path, uint64_t xuid) {
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    file << "# Generated once per installation. Multiplayer needs a different\n"
            "# XUID for every player; copying this file to another PC breaks that.\n"
         << fmt::format("xuid = \"{:016X}\"\n", xuid);
    if (!file) return false;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  return !ec;
}

std::array<uint8_t, 20> OwnerHash(uint64_t xuid, const uint8_t* data, size_t size) {
  std::array<uint8_t, 8> owner;
  for (int i = 0; i < 8; ++i) owner[i] = uint8_t(xuid >> (56 - 8 * i));
  sha1::SHA1 sha;
  sha.processBytes(owner.data(), owner.size());
  sha.processBytes(data, size);
  std::array<uint8_t, 20> digest;
  sha.finalize(digest.data());
  return digest;
}

// Gears signs player storage as SHA-1(owner XUID, big endian || data), stored
// in the first 20 bytes. Re-signs files that match the old owner exactly.
void ResignTree(const std::filesystem::path& root, uint64_t from, uint64_t to) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (!entry.is_regular_file(ec) || entry.file_size(ec) < 21) continue;
    std::vector<uint8_t> bytes(entry.file_size(ec));
    {
      std::ifstream file(entry.path(), std::ios::binary);
      if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) continue;
    }
    const auto old_hash = OwnerHash(from, bytes.data() + 20, bytes.size() - 20);
    if (!std::equal(old_hash.begin(), old_hash.end(), bytes.begin())) continue;
    const auto new_hash = OwnerHash(to, bytes.data() + 20, bytes.size() - 20);
    std::copy(new_hash.begin(), new_hash.end(), bytes.begin());
    std::ofstream file(entry.path(), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    REXLOG_INFO("Identity: re-signed {} for the new profile", entry.path().string());
  }
}

// The profile's title data spans the three title-specific settings, each a
// file in the profile directory; the signature covers them joined.
void ResignProfile(const std::filesystem::path& profile, uint64_t from, uint64_t to) {
  const char* names[] = {"63E83FFF", "63E83FFE", "63E83FFD"};
  std::vector<uint8_t> joined;
  size_t first_size = 0;
  for (const char* name : names) {
    std::ifstream file(profile / name, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (joined.empty()) first_size = bytes.size();
    joined.insert(joined.end(), bytes.begin(), bytes.end());
  }
  if (first_size < 20 || joined.size() < 21) return;
  const auto old_hash = OwnerHash(from, joined.data() + 20, joined.size() - 20);
  if (!std::equal(old_hash.begin(), old_hash.end(), joined.begin())) return;
  const auto new_hash = OwnerHash(to, joined.data() + 20, joined.size() - 20);
  std::fstream file(profile / names[0], std::ios::binary | std::ios::in | std::ios::out);
  file.write(reinterpret_cast<const char*>(new_hash.data()), std::streamsize(new_hash.size()));
  REXLOG_INFO("Identity: re-signed profile data in {}", profile.string());
}

// Copies `from` to `to` unless `to` exists; `from` stays as the backup.
bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  if (!std::filesystem::is_directory(from, ec) || std::filesystem::exists(to, ec)) return false;
  std::filesystem::create_directories(to.parent_path(), ec);
  std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ec);
  if (ec) {
    REXLOG_WARN("Identity: could not copy {} to {}: {}", from.string(), to.string(), ec.message());
    return false;
  }
  REXLOG_INFO("Identity: copied {} to {}", from.string(), to.string());
  return true;
}
}  // namespace

uint64_t Gow3GenerateXuid() {
  std::random_device device;
  const uint64_t random = (uint64_t(device()) << 32) | device();
  return 0xE000000000000000ull | (random & 0x0000FFFFFFFFFFFFull);
}

std::optional<uint64_t> Gow3ParseXuid(std::string_view text) {
  if (text.empty() || text.size() > 16) return std::nullopt;
  uint64_t value = 0;
  for (const char c : text) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    value = value * 16 + (std::isdigit(static_cast<unsigned char>(c))
                              ? c - '0'
                              : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
  }
  if (!value || (value & 0x00C0000000000000ull)) return std::nullopt;
  return value;
}

bool Gow3ValidGamertag(std::string_view gamertag) {
  if (gamertag.empty() || gamertag.size() > 15) return false;
  if (!std::isalpha(static_cast<unsigned char>(gamertag.front()))) return false;
  char previous = 0;
  for (const char c : gamertag) {
    if (c == ' ' ? previous == ' ' : !std::isalnum(static_cast<unsigned char>(c))) return false;
    previous = c;
  }
  return gamertag.back() != ' ';
}

void Gow3PrepareIdentity(const std::filesystem::path& user_root) {
  if (!Gow3ValidGamertag(REXCVAR_GET(user_gamertag))) {
    REXLOG_WARN("Identity: invalid gamertag '{}'; using 'Player'", REXCVAR_GET(user_gamertag));
    REXCVAR_SET(user_gamertag, std::string("Player"));
  }
  if (user_root.empty()) return;
  const std::string configured = REXCVAR_GET(user_xuid);
  auto xuid = configured.empty() ? std::nullopt : Gow3ParseXuid(configured);
  if (!configured.empty() && !xuid)
    REXLOG_WARN("Identity: invalid user_xuid '{}'; using this installation's identity", configured);
  std::error_code ec;
  std::filesystem::create_directories(user_root, ec);
  const auto path = user_root / kIdentityFile;
  if (!xuid) xuid = ReadIdentity(path);
  if (!xuid) {
    xuid = Gow3GenerateXuid();
    if (WriteIdentity(path, *xuid)) {
      REXLOG_INFO("Identity: created XUID {:016X}", *xuid);
    } else {
      REXLOG_ERROR("Identity: could not write {}; keeping the built-in profile ID", path.string());
      xuid = kGow3LegacyXuid;
    }
  }
  // Saves live under USER/<XUID>/; profile settings under USER/<title>/profile/,
  // which older versions keyed by the name "User" and now by XUID.
  const auto xuid_dir = fmt::format("{:016X}", *xuid);
  const auto title = fmt::format("{:08X}", kGow3TitleId);
  if (*xuid != kGow3LegacyXuid &&
      CopyTree(user_root / fmt::format("{:016X}", kGow3LegacyXuid), user_root / xuid_dir))
    ResignTree(user_root / xuid_dir, kGow3LegacyXuid, *xuid);
  if (CopyTree(user_root / title / "profile" / "User", user_root / title / "profile" / xuid_dir) &&
      *xuid != kGow3LegacyXuid)
    ResignProfile(user_root / title / "profile" / xuid_dir, kGow3LegacyXuid, *xuid);
  REXCVAR_SET(user_xuid, xuid_dir);
}
