#pragma once
#include <filesystem>
#include <string>

namespace rex::system {
class KernelState;
}

// Downloadable content: Xbox content packages (map packs, RAAM's Shadow) put in
// USER/dlc/ are unpacked once into USER/0000000000000000/<title>/00000002/,
// where the game finds them like content installed on a console hard drive.
// Run after the XEX is loaded (the title ID is known) and before it starts.
void Gow3InstallDownloadedContent(rex::system::KernelState* kernel_state,
                                  const std::filesystem::path& user_root);

// The game code is recompiled from default.xex with Title Update 6 applied.
// The setup wizard unpacks the user's update into USER/update/, mounted as
// update:, where the loader finds its executable patch (default.xexp).
// Returns an error message when the update is missing or the loaded
// executable is not the updated version; empty when the update is in place.
std::string Gow3TitleUpdateProblem(const std::filesystem::path& update_root);
std::string Gow3TitleUpdateProblem(rex::system::KernelState* kernel_state);

// Title Update 6 raised the campaign checkpoint version from 3 to 4 and reports
// older checkpoints as corrupted; the rest of the format is unchanged. Each
// version 3 checkpoint under USER/<XUID>/<title>/00000001/GearsCheckpoint/ is
// copied once to USER/<XUID>/<title>/checkpoints-before-tu6/ and then marked
// version 4 in place. Run before the game starts.
void Gow3UpgradeCheckpoints(rex::system::KernelState* kernel_state, const std::filesystem::path& user_root);

// The game picks its text and speech language from the console language and,
// for Czech, Hungarian and the two Spanish versions, the console country
// (XGetLanguage/XGetLocale through XConfig). Sets those from gow3_language.
// Discs differ in which languages they carry; one missing from the game
// folder (no Coalesced_<code>.bin) falls back to English. Run before the
// game starts.
void Gow3ApplyLanguage(const std::filesystem::path& game_root);
