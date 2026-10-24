#pragma once
#include <filesystem>

namespace rex::system {
class KernelState;
}

// Downloadable content: Xbox content packages (map packs, RAAM's Shadow) put in
// USER/dlc/ are unpacked once into USER/0000000000000000/<title>/00000002/,
// where the game finds them like content installed on a console hard drive.
// Run after the XEX is loaded (the title ID is known) and before it starts.
void Gow3InstallDownloadedContent(rex::system::KernelState* kernel_state,
                                  const std::filesystem::path& user_root);
