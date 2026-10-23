# squarehole360

Xbox 360 games, statically recompiled to native x86-64 and drawn by a native
Vulkan renderer: a square peg for a round hole.

The first target is **Gears of War 3**, which is playable on Linux. It has
been played through Act 1, including the boss fight. The framework is being prepared for **Gears of
War 2** and **Gears of War: Judgment** next.

> **No game content is included.** You need your own legally obtained disc
> image. Everything derived from it is generated locally during the build:
> assets, the recompiled game code, and the XDK functions the renderer copies.

## How it works

- **CPU.** The game's PowerPC executable is recompiled ahead of time to C++ and
  then native x86-64 with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).
  There is no JIT and no interpreter. The SDK also provides the Xbox 360 runtime
  around the game: memory, threads, kernel and XAM services, audio and input.
- **GPU.** A new renderer intercepts the game's Direct3D calls instead of
  emulating the Xenos GPU's command stream. It renders with real Vulkan render
  targets and fixed-function blending and depth, and models EDRAM only where the
  game depends on it. Shaders are translated from Xenos microcode to SPIR-V
  and cached; pipelines build in the background.
- **Threading.** The game's render thread only captures each draw. Planning,
  command recording and driver submission run on their own threads.
- **Presentation.** Native Wayland with HDR10 and VRR; X11 fallback.

[docs/renderer-vs-xenia.md](docs/renderer-vs-xenia.md) compares this approach
with Xenia. [docs/direct-renderer-plan.md](docs/direct-renderer-plan.md)
covers the renderer's stages and what remains emulated.

## Performance

Gears of War 3 at its native 720p, upscaled to 4K, on an RX 9070 XT
(frame-rate cap off):

| Scene | Frame time |
|---|---|
| Act 1 Tickers fight, about 1,600 draws and queries per frame | about 7.3 ms (about 137 fps) |
| Heaviest camera-pan frames, 2,500 or more operations | about 12 ms |
| Typical frames, under 500 operations | about 2.6 ms |

The GPU's command processor, not its shaders, is now the limit.
[STATUS.md](STATUS.md) has every measurement and the options behind it.

## Requirements

- Linux to build. The game runs on Linux, and on Windows from a cross-compiled
  build (see below)
- Clang, CMake 3.25+, Ninja, Python 3.11+
- A modern Vulkan GPU and driver (developed and tested on an AMD RX 9070 XT
  with RADV)
- Optional: jemalloc and OpenSSL development packages (used automatically);
  PySide6 and tomli-w for the standalone settings editor
- Your own Gears of War 3 disc image. The supported executable is
  `default.xex` with SHA-256 `4e5d00b8…f8f790`; the importer checks it.

## Build and run

```sh
git clone https://github.com/GloriousEggroll/squarehole360.git
cd squarehole360
tools/setup-dependencies.sh            # pinned submodules (our forks), recursive

python3 tools/import-assets.py import "/path/to/Gears of War 3.iso"
tools/build-linux.sh                   # recompiles the game, then builds it
tools/run-linux.sh
```

`tools/build-linux.sh` runs `rexglue codegen` on your executable. It then
generates the XDK-derived inputs (`tools/porting/generate_getdata.py`) and
builds `build/bin/gow3`. Saves and caches live in `user/`, and logs in
`logs/`.

### Windows build

Code generation runs on Linux, so the Windows build is cross-compiled there
with clang-cl and an [xwin](https://github.com/Jake-Shadle/xwin) sysroot
(creating it accepts Microsoft's license):

```sh
xwin --accept-license --arch x86_64 splat --use-winsysroot-style \
  --preserve-ms-arch-notation --output ~/.xwin
tools/build-linux.sh                   # generates the recompiled sources
tools/build-windows.sh                 # builds build-windows/bin/gow3.exe
```

Copy the checkout (or at least `build-windows/`, `config/`, `game/` and
`tools/`) to Windows and run `tools\run-windows.bat`; the settings editor is
`tools\settings-windows.bat`. It needs the Microsoft Visual C++ 2015-2022
runtime and a Vulkan 1.3 driver. The same build also runs under Wine and
Proton.

Settings live in `config/gow3.toml`. You can change them in the standalone
editor (`tools/settings.sh`) or in game (L3+R3, or F4). The
[Gears of War 3 guide](docs/gears3.md) covers controls, display, HDR, the
frame-rate cap, the renderer choice (native or Xenia) and troubleshooting.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Game glue, settings, Direct3D bridge |
| `src/renderer/native/` | Native renderer: capture planner, scene executor, resolve memory, GPU work queue |
| `src/renderer/profiles/` | Per-game executable profiles (D3D entry points, device offsets) |
| `config/` | Game settings and recompiler hook definitions |
| `vendor/rexglue-sdk/` | Submodule: [our ReXGlue SDK fork](https://github.com/GloriousEggroll/rexglue-sdk/tree/squarehole360) (v0.8.0 + port changes) |
| `vendor/plume`, `vendor/xenos-recomp`, `vendor/fidelityfx-sdk` | Submodules: our forks, `squarehole360` branch |
| `tools/assets/`, `tools/import-assets.py` | Disc import and verification |
| `tools/porting/` | XEX image dump, XDK signature scanner, GetData generator |
| `tools/rendering/` | Renderer tests, replay tools, the [benchmark harness](tools/rendering/bench/README.md) |
| `docs/` | Design notes, porting guide, Gears 3 guide |

## Porting other games

The renderer's dependence on one executable is collected in a compile-time
profile (`src/renderer/profiles/gears3.h`). `tools/porting/xdk_signatures.py`
locates the XDK Direct3D functions and hook sites in another executable. It
also reports how the D3D device structure moved between XDK versions.

First scans:
- **Judgment:** 34 of 39 functions and 30 of 31 hook sites located automatically.
- **Gears 2:** built against an older XDK; needs manual work.

See [docs/porting.md](docs/porting.md).

## Testing

- Planner parity on captured frames: `gow3-native-live-plan-test` in the
  `tools/rendering/native` test project.
- Memory, constant-upload, cache and GPU fixture tests:
  `tools/rendering/native/CMakeLists.txt` and the `gow3-*-test` targets.
- Live automated runs: `tools/rendering/verify-native-boot.py` and
  `tools/rendering/bench/`.

## License

BSD 3-Clause; see [LICENSE](LICENSE). Bundled and submodule components keep
their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Gears of War, Xbox and Xbox 360 are trademarks of Microsoft Corporation. This
project is not affiliated with or endorsed by Microsoft, The Coalition or Epic
Games.
