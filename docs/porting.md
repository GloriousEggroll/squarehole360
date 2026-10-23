# Porting the framework to another Xbox 360 game

The runtime and most of the native renderer are game-independent:
- static recompilation, memory, threads, kernel/XAM, audio, input
- Xenos shader translation, texture formats, EDRAM modelling
- the hardware render-target executor and its threading

The executable-specific parts are collected in a few places.

## Where the game-specific data lives

| What | Where | Found in a new game by |
|---|---|---|
| D3D entry points the renderer intercepts | `src/renderer/profiles/<game>.h` (`d3d::`) | `tools/porting/xdk_signatures.py` |
| D3D device offsets the capture reads | same header (`device::`) | the scan's displacement map |
| Game (UE3) code addresses | same header (`game::`) | by hand |
| Recompiler hooks and function boundaries | `config/*.toml` | sites from the scan; boundaries by hand |
| Instruction-word witnesses for hook sites | `src/renderer/native/*_sites.h`, `query_poll_yield.h`, `shader_discovery.h`, `getdata_inline_selection.h` | the scan's `sites` report |
| Overridden D3D functions | `sub_XXXXXXXX` overrides in `src/renderer/gears_bridge.cpp` | rename to the scanned addresses |
| Generated GetData copy | `src/renderer/native/getdata_*.inc` | regenerate from the new binary |
| Shader-hash fast paths (about 750 hashes) | various `src/renderer/native/*catalog*`, `*_compiler.h` | not needed; they fall back to the general path |

`src/renderer/game_profile.h` selects the profile at compile time (`GOW3_GAME_PROFILE`, default Gears 3). Moving the Gears 3 constants into the profile changed no generated code. The only differences in the disassembly were `__LINE__` constants shifted by the added lines.

## Signature workflow

```sh
# 1. Dump the loaded image (decrypted, decompressed) of each executable.
build/bin/gow3-xex-image-dump game default.xex /tmp/gears3
build/bin/gow3-xex-image-dump /path/to/other/game default.xex /tmp/other
# 2. Signatures from the reference profile (functions + mid-function hook sites).
python3 tools/porting/xdk_signatures.py extract /tmp/gears3 tools/porting/profiles/gears3.json /tmp/sigs.json
# 3. Locate them in the other game.
python3 tools/porting/xdk_signatures.py scan /tmp/sigs.json /tmp/other --diff 12 --out /tmp/other-scan.json
```

**Masking.** Signatures mask call targets and absolute-address immediates.

**Matching.** Matching proceeds in three steps:
- Exact masked matches first.
- For the rest, alignment-tolerant similarity, with the search limited by the XDK library's fixed link order.
- Hook sites are located inside their matched function by context and instruction word.

**Displacement map.** The scan reports every device-offset displacement that differs between the two builds. That gives the `device::` values for the new profile.

## First results (2026-10-23)

Gears 3 scanned against itself resolves all 39 functions and 31 sites.

| Target | Functions (exact / similar / missing) | Hook sites located | Device offsets |
|---|---|---|---|
| Gears of War: Judgment (2013) | 16 / 18 / 5 | 30 of 31 (20 exact) | +0x80 for every field from 0x2F68 up (50 votes) |
| Gears of War 2 (2008) | 3 / 13 / 23 | 10 of 31 (5 exact) | Ranges shifted by −128 to −304 |

**Judgment** is close to Gears 3. Five functions need hand checking: Swap, the ring submit, DrawFlushB, interrupt emit and pixel-shader init.

**Gears 2** was built against an older XDK. Its Direct3D library differs enough that most functions need manual identification. The similarity candidates and decoded diffs are the starting point.

The register shadow (fetch constants at 0x480, render state 0x2880–0x2A5F) showed no movement in either game. That is consistent with it mirroring Xenos hardware registers.

## Not covered by the scan

- The 19 inlined `EndVertices` commit sites in game code. Find them by scanning for `stw rX,0x30(rDevice)` after the BeginVertices call sites.
- The GetData blocking-retry caller and the shader-cache decoder hooks.
- The frame-rate and movie-timing patches.
- Recompiler function boundaries missing from `.pdata`.

Each new game will also exercise Xenos features Gears 3 did not use. Multisampled render targets were the first example; they now fall back to single-sample. Stages 3 and 4 of `docs/direct-renderer-plan.md` would move the renderer's boundary from inlined D3D internals to the D3D API itself. That would shrink the per-game list above.
