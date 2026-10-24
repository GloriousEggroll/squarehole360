#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/system/xam/lobby_protocol.h>
#include <rex/system/xam/network.h>

#include "gow3_identity.h"
#include "../vendor/rexglue-sdk/thirdparty/crypto/TinySHA1.hpp"

REXCVAR_DECLARE(std::string, user_xuid);
REXCVAR_DECLARE(std::string, user_gamertag);

namespace {
std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                              std::streamsize(bytes.size()));
}

// Gears' player-storage signature: SHA-1(owner XUID big endian || data).
std::vector<uint8_t> Signed(uint64_t xuid, const std::vector<uint8_t>& data) {
  uint8_t owner[8];
  for (int i = 0; i < 8; ++i) owner[i] = uint8_t(xuid >> (56 - 8 * i));
  sha1::SHA1 sha;
  sha.processBytes(owner, 8);
  sha.processBytes(data.data(), data.size());
  std::vector<uint8_t> out(20);
  sha.finalize(out.data());
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

bool SignedBy(uint64_t xuid, const std::vector<uint8_t>& bytes) {
  return bytes.size() > 20 &&
         Signed(xuid, std::vector<uint8_t>(bytes.begin() + 20, bytes.end())) == bytes;
}
}  // namespace

TEST_CASE("Lobby messages round-trip", "[network]") {
  using namespace rex::lobby;
  std::vector<Member> members{{0x0A450001, 0xE000123456789ABCull, "HostA", 0x7F000001, 40000, 7},
                              {0x0A450002, 0xE000000000000002ull, "ClientB", 0xC0A80014, 50000, 9}};
  const auto bytes = Writer(Type::kWelcome).U32(42).U32(0x0A450002).U64(99).Members(members).bytes();
  Reader reader(bytes.data(), bytes.size());
  REQUIRE(reader.Header() == Type::kWelcome);
  uint32_t nonce, vip;
  uint64_t token;
  std::vector<Member> decoded;
  REQUIRE(reader.U32(nonce));
  REQUIRE(reader.U32(vip));
  REQUIRE(reader.U64(token));
  REQUIRE(reader.Members(decoded));
  REQUIRE(nonce == 42);
  REQUIRE(vip == 0x0A450002);
  REQUIRE(token == 99);
  REQUIRE(decoded.size() == 2);
  REQUIRE(decoded[1].gamertag == "ClientB");
  REQUIRE(decoded[1].public_ip == 0xC0A80014);
  REQUIRE(decoded[1].public_port == 50000);
  REQUIRE(decoded[0].xuid == 0xE000123456789ABCull);
  REQUIRE(reader.remaining() == 0);

  // Truncated or foreign packets are rejected rather than misread.
  Reader truncated(bytes.data(), bytes.size() - 1);
  REQUIRE(truncated.Header() == Type::kWelcome);
  REQUIRE(truncated.U32(nonce));
  REQUIRE(truncated.U32(vip));
  REQUIRE(truncated.U64(token));
  REQUIRE_FALSE(truncated.Members(decoded));
  const uint8_t foreign[] = {1, 2, 3, 4, 1, 6};
  REQUIRE_FALSE(Reader(foreign, sizeof(foreign)).Header());

  REQUIRE(IsVirtual(0x0A450102));
  REQUIRE_FALSE(IsVirtual(0xC0A80001));
}

TEST_CASE("Guest ports below 1024 map to 10000+ on every host", "[network]") {
  using rex::system::xam::Network;
  REQUIRE(Network::HostPort(1000) == 11000);
  REQUIRE(Network::HostPort(0) == 0);
  REQUIRE(Network::HostPort(14000) == 14000);
  REQUIRE(Network::GuestPort(11000) == 1000);
  REQUIRE(Network::GuestPort(14000) == 14000);
  for (uint16_t port : {1, 80, 1000, 1023}) REQUIRE(Network::GuestPort(Network::HostPort(port)) == port);
}

TEST_CASE("Profile IDs and gamertags", "[identity]") {
  for (int i = 0; i < 64; ++i) {
    const uint64_t xuid = Gow3GenerateXuid();
    REQUIRE((xuid >> 60) == 0xE);
    REQUIRE((xuid & 0x00C0000000000000ull) == 0);
    REQUIRE(Gow3ParseXuid(fmt::format("{:016X}", xuid)) == xuid);
  }
  REQUIRE(Gow3GenerateXuid() != Gow3GenerateXuid());
  REQUIRE_FALSE(Gow3ParseXuid(""));
  REQUIRE_FALSE(Gow3ParseXuid("xyz"));
  REQUIRE_FALSE(Gow3ParseXuid("00C0000000000001"));  // Titles reject these bits.
  REQUIRE(Gow3ValidGamertag("Glorious Eggrol"));
  REQUIRE(Gow3ValidGamertag("Player1"));
  REQUIRE_FALSE(Gow3ValidGamertag(""));
  REQUIRE_FALSE(Gow3ValidGamertag("1Player"));
  REQUIRE_FALSE(Gow3ValidGamertag("Two  Spaces"));
  REQUIRE_FALSE(Gow3ValidGamertag("Trailing "));
  REQUIRE_FALSE(Gow3ValidGamertag("SixteenCharacter"));
}

TEST_CASE("First run moves legacy saves to a new profile ID and re-signs them", "[identity]") {
  const auto root = std::filesystem::temp_directory_path() /
                    fmt::format("gow3-identity-test-{}", std::random_device{}());
  std::filesystem::remove_all(root);
  const auto legacy = fmt::format("{:016X}", kGow3LegacyXuid);
  const auto storage = root / legacy / "4D5308AB/00000001/PlayerStorage.dat/PlayerStorage.dat";
  WriteFile(storage, Signed(kGow3LegacyXuid, std::vector<uint8_t>(600, 0x5A)));
  // Profile data spans two title settings; the signature covers both.
  const auto profile_bytes = Signed(kGow3LegacyXuid, std::vector<uint8_t>(985, 0x33));
  WriteFile(root / "4D5308AB/profile/User/63E83FFF",
            std::vector<uint8_t>(profile_bytes.begin(), profile_bytes.begin() + 1000));
  WriteFile(root / "4D5308AB/profile/User/63E83FFE",
            std::vector<uint8_t>(profile_bytes.begin() + 1000, profile_bytes.end()));
  WriteFile(root / "4D5308AB/profile/User/63E83FFD", {});

  REXCVAR_SET(user_xuid, std::string());
  REXCVAR_SET(user_gamertag, std::string("Tester"));
  Gow3PrepareIdentity(root);
  const auto xuid = Gow3ParseXuid(REXCVAR_GET(user_xuid));
  REQUIRE(xuid);
  REQUIRE(*xuid != kGow3LegacyXuid);
  REQUIRE(std::filesystem::exists(root / "identity.toml"));

  const auto dir = fmt::format("{:016X}", *xuid);
  REQUIRE(SignedBy(*xuid, ReadFile(root / dir / "4D5308AB/00000001/PlayerStorage.dat/PlayerStorage.dat")));
  REQUIRE(SignedBy(kGow3LegacyXuid, ReadFile(storage)));  // The original stays as the backup.
  auto profile = ReadFile(root / "4D5308AB/profile" / dir / "63E83FFF");
  const auto tail = ReadFile(root / "4D5308AB/profile" / dir / "63E83FFE");
  profile.insert(profile.end(), tail.begin(), tail.end());
  REQUIRE(SignedBy(*xuid, profile));

  // A second start keeps the same identity and does not copy again.
  REXCVAR_SET(user_xuid, std::string());
  Gow3PrepareIdentity(root);
  REQUIRE(REXCVAR_GET(user_xuid) == dir);
  std::filesystem::remove_all(root);
  REXCVAR_SET(user_xuid, std::string());
}
