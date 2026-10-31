# Third-party notices

squarehole360's own code is under the BSD 3-Clause license in `LICENSE`.
It builds on the following projects, each under its own license.

| Component | Location | License |
|---|---|---|
| ReXGlue SDK, our fork `GloriousEggroll/rexglue-sdk` branch `squarehole360` (submodule), including code derived from the Xenia project | `vendor/rexglue-sdk` | BSD 3-Clause (`vendor/rexglue-sdk/LICENSE`) |
| Xenia-derived texture untiling | `src/renderer/native/texture_data.cpp` | BSD 3-Clause (`src/renderer/native/texture_data.LICENSE`) |
| Plume (fork `GloriousEggroll/plume`, branch `squarehole360`) | `vendor/plume` | MIT |
| XenosRecomp (fork `GloriousEggroll/XenosRecomp`, branch `squarehole360`) | `vendor/xenos-recomp` | MIT |
| AMD FidelityFX SDK (fork `GloriousEggroll/FidelityFX-SDK` of ReXGlue's fork, branch `squarehole360`) | `vendor/fidelityfx-sdk` | MIT (see `LICENSE.txt` in the submodule) |
| ReXGlue third-party libraries (FFmpeg, SDL3, glslang, SPIRV-Tools, Vulkan headers, VMA, volk, fmt, spdlog, xxHash, imgui, tracy, and others; nested submodules of the SDK; glslang and SPIRV-Tools from our forks' `squarehole360` branches) | `vendor/rexglue-sdk/thirdparty` | Their own licenses, in each submodule or directory |

Each submodule is pinned to an exact commit. Where we change a dependency,
the submodule points at our fork's `squarehole360` branch: the upstream
commit we build against plus our changes. `tools/setup-dependencies.sh`
fetches everything recursively.

## Bundled in the release packages

The packages carry runtime pieces so that players install nothing else. None
is in this repository: `tools/fetch-runtime-deps.sh` downloads them from
their official sources, pinned by version and SHA-256, and the package
scripts copy them unmodified.

| Component | Packages | License |
|---|---|---|
| CPython 3.13 (Windows embeddable package; python-build-standalone on macOS) | Windows, macOS (`python/`) | PSF License |
| PySide6-Essentials and shiboken6 (Qt for Python) | Windows, macOS (`python/`) | LGPLv3 |
| tomli-w | Windows, macOS (`python/`) | MIT |
| Microsoft Visual C++ 2015-2022 runtime DLLs (redistributable files) | Windows | Microsoft Visual C++ Redistributable license |
| MoltenVK and the Khronos Vulkan loader | macOS (`build/lib`) | Apache 2.0 |
| OpenSSL libcrypto | macOS (`build/lib`) | Apache 2.0 |

The Windows build compiles mimalloc 3.5.3 (MIT, Microsoft) from its pinned
release archive at configure time and ships it as `mimalloc.dll`.

The Linux package carries libstdc++, libgcc_s (GPL with the GCC runtime
library exception), jemalloc (BSD 2-Clause) and OpenSSL libcrypto (Apache
2.0).

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
