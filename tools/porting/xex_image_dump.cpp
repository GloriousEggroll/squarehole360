// Load an Xbox 360 XEX through the ReXGlue runtime (decryption, decompression,
// relocation) and write its loaded image for offline analysis:
//   OUT.bin   bytes of [base, base+image_size), zero where no section is mapped
//   OUT.json  {"base","image_size","entry","sections":[{name,address,size,executable}]}
// Usage: gow3-xex-image-dump GAME_ROOT XEX_RELATIVE_PATH OUT_PREFIX
#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  using rex::X_STATUS;
  if (argc != 4) {
    std::cerr << "usage: gow3-xex-image-dump GAME_ROOT XEX_RELATIVE_PATH OUT_PREFIX\n";
    return 2;
  }
  const std::filesystem::path root = std::filesystem::canonical(argv[1]);
  std::string relative = argv[2];
  std::replace(relative.begin(), relative.end(), '/', '\\');

  rex::Runtime runtime(root.string());
  if (runtime.Setup(rex::RuntimeConfig{.kernel_init = rex::kernel::InitializeKernel, .tool_mode = true}) !=
      X_STATUS_SUCCESS) {
    std::cerr << "runtime setup failed\n";
    return 1;
  }
  if (runtime.LoadXexImage("game:\\" + relative) != X_STATUS_SUCCESS) {
    std::cerr << "failed to load " << relative << "\n";
    return 1;
  }
  auto user = runtime.kernel_state()->GetExecutableModule();
  auto* xex = user ? user->xex_module() : nullptr;
  if (!xex) {
    std::cerr << "no executable module\n";
    return 1;
  }
  const uint32_t base = xex->base_address(), size = xex->image_size();
  std::vector<uint8_t> image(size);
  nlohmann::json sections = nlohmann::json::array();
  for (const auto& section : xex->binary_sections()) {
    sections.push_back({{"name", section.name}, {"address", section.virtual_address},
                        {"size", section.virtual_size}, {"executable", section.executable}});
    if (!section.host_data || section.virtual_address < base) continue;
    const uint64_t offset = section.virtual_address - base;
    if (offset >= size) continue;
    const size_t count = std::min<uint64_t>(section.virtual_size, size - offset);
    std::memcpy(image.data() + offset, section.host_data, count);
  }
  const std::string prefix = argv[3];
  std::ofstream(prefix + ".bin", std::ios::binary)
      .write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
  std::ofstream(prefix + ".json") << nlohmann::json{{"base", base}, {"image_size", size},
                                                    {"entry", xex->entry_point()},
                                                    {"sections", sections}}.dump(1)
                                  << "\n";
  std::cout << "base=0x" << std::hex << base << " size=0x" << size << " sections=" << std::dec
            << sections.size() << "\n";
  return 0;
}
