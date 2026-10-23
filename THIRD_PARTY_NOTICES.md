# Third-party notices

squarehole360's own code is under the BSD 3-Clause license in `LICENSE`.
It builds on the following projects, each under its own license.

| Component | Location | License |
|---|---|---|
| ReXGlue SDK, our fork `GloriousEggroll/rexglue-sdk` branch `squarehole360` (submodule), including code derived from the Xenia project | `vendor/rexglue-sdk` | BSD 3-Clause (`vendor/rexglue-sdk/LICENSE`) |
| Xenia-derived texture untiling | `src/renderer/native/texture_data.cpp` | BSD 3-Clause (`src/renderer/native/texture_data.LICENSE`) |
| Plume (submodule + `patches/plume.patch`) | `vendor/plume` | MIT |
| XenosRecomp (submodule + `patches/xenos-recomp.patch`) | `vendor/xenos-recomp` | MIT |
| AMD FidelityFX SDK (submodule + `patches/fidelityfx-sdk.patch`) | `vendor/fidelityfx-sdk` | MIT (see `LICENSE.txt` in the submodule) |
| ReXGlue third-party libraries (FFmpeg, SDL3, glslang, SPIRV-Tools, Vulkan headers, VMA, volk, fmt, spdlog, xxHash, imgui, tracy, and others; nested submodules of the SDK; glslang and SPIRV-Tools patched from `patches/`) | `vendor/rexglue-sdk/thirdparty` | Their own licenses, in each submodule or directory |

Each submodule is pinned to an exact commit. `tools/setup-dependencies.sh`
fetches them recursively and applies the patches in `patches/`.

## Not included

- **No game content.** No disc images, executables, assets, saves, or code
  recompiled from a game executable are distributed. Users import their own
  legally obtained disc (`tools/import-assets.py`). The build regenerates
  executable-derived code locally:
  - the recompiled game (`generated/default/`, via `rexglue codegen`)
  - the copied XDK GetData function (`generated/native-getdata/`, via
    `tools/porting/generate_getdata.py`)
- **No proprietary manuals** in this repository. (The SDK fork keeps upstream
  ReXGlue's files as they are.)

## Trademarks

Gears of War, Xbox and Xbox 360 are trademarks of Microsoft Corporation.
This project is not affiliated with or endorsed by Microsoft, The Coalition,
Epic Games or AMD.
