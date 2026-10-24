#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The local player's identity. Multiplayer needs every player to have a
// different XUID, so each installation generates one on first run and keeps it
// in USER/identity.toml. The gamertag is a normal setting (user_gamertag).

// Built-in XUID of earlier versions; saves made with it are migrated.
inline constexpr uint64_t kGow3LegacyXuid = 0xB13EBABEBABEBABEull;
inline constexpr uint32_t kGow3TitleId = 0x4D5308AB;

// Offline-profile XUID (top nibble 0xE) with 48 random bits; never sets the
// 0x00C0000000000000 bits that titles reject.
uint64_t Gow3GenerateXuid();
std::optional<uint64_t> Gow3ParseXuid(std::string_view text);
// 1-15 characters, letters, digits and single spaces, starting with a letter.
bool Gow3ValidGamertag(std::string_view gamertag);

// Run after the config is loaded and before the runtime starts. Unless the
// user_xuid setting is set explicitly, reads or creates USER/identity.toml,
// copies saves and profile settings of the legacy XUID to a new one, and sets
// user_xuid for the profile.
void Gow3PrepareIdentity(const std::filesystem::path& user_root);
