# Gears of War 3: user guide

Detailed setup, settings and renderer notes for the Gears of War 3 build.
The project overview is in the [README](../README.md); current measured
results are in [STATUS.md](../STATUS.md).

This is an **incomplete, experimental port**, not a full source decompilation.
The native Linux executable reaches the title screen and loads and renders
the opening prison cinematic and gameplay from a fresh offline campaign.
The former empty-world defect and black patches on Anya's face in the
opening cinematic are fixed. Full gameplay correctness has not been established.

The build uses our ReXGlue SDK fork (`vendor/rexglue-sdk`, a submodule) and
regenerates C++ from the executable extracted from your ISO. It uses native
x86-64 CPU code,
ReXGlue's Xbox services and command ordering, and the new native Vulkan renderer
by default. The Xenia-derived renderer remains available for compatibility.

## Run and build

Each user supplies their own copy of the game; game content and converted
renderer data are not release inputs. When the game files are missing, the
launcher opens a setup wizard (`tools/setup-wizard.py`, a window with PySide6
or prompts in a terminal). It asks for your Xbox 360 disc image (ISO) or a
folder with the extracted disc files, imports them, then offers to add DLC from
folders or `.zip`, `.tar.gz` and `.tar.xz` archives. The same steps work from
the command line:

```sh
python3 tools/import-assets.py import "/path/to/gow3.iso"   # or a game folder
python3 tools/import-assets.py dlc ~/Downloads               # DLC folders/archives
python3 tools/setup-wizard.py --dlc                          # add DLC later
```

Each game has a definition in `tools/assets/games/` (`gow3.json` here): its
executable, every base-game file with its size and SHA-256, and its known DLC
packages. Folders and archives may contain other files; only the listed ones
are used, and each is checked before it is copied.

The portable Python tool extracts the disc into `game/`, verifies checksums,
and prepares a local asset catalog and native shader inputs in
`game/.gow3-assets/`. It checks the exact executable version this build supports,
shows progress, and publishes a fresh installation only after verification.
It does not download any game content. Python 3.11+ is required; extraction
does not require Wine, Xenia, a mount command, or the old `xdvdfs` executable.
Windows uses the same tool (`py -3 tools/import-assets.py ...`); Windows execution
has not yet been tested.

For an existing extraction, the following verifies every file against the ISO
before adopting it. Original game files are preserved:

```sh
python3 tools/import-assets.py import "/path/to/gow3.iso" --adopt-existing
python3 tools/import-assets.py verify
```

The ISO is only needed for import/adoption, not subsequent launches. A different
destination can be selected with `--output "/path/to/game"`; launch that
installation with `./tools/run-linux.sh --game_data_root="/path/to/game"`.
The normal launcher checks the imported inventory, sizes and executable hash;
`verify` checks every game and prepared file's full SHA-256.
See [the asset tool documentation](../tools/assets/README.md) for native shader
compilation, recovery, and the boundary between extracted and converted assets.

### Downloadable content

The setup wizard and `import-assets.py dlc` find your own DLC packages (files
named like `4D5308AB/00000002/85F354E7...`) in folders and archives, check them,
and copy them to `user/dlc/`; packages can also be put there by hand. The next
start installs each new package once into `user/0000000000000000/4D5308AB/00000002/`,
where the game finds it like content on a console hard drive; the packages in
`user/dlc/` can be deleted afterwards. The map packs, the Horde Command Pack and
RAAM's Shadow are supported. Booster's "Swamp" map is listed as Bullet Marsh.

From this directory:

```sh
./tools/run-linux.sh
./tools/settings.sh
./tools/build-linux.sh
./tools/validate.sh
```

Run uses `build/bin/gow3`, mounts `game/`, and stores saves and caches in
`user/`. This is a Release build. Normal launches do not load RenderDoc or
enable its capture overlay; graphics capture requires an explicit RenderDoc launch. Xbox Live is unavailable. Co-op, Horde, Beast and
private Versus work through System Link, on a LAN or over the internet through
a lobby server; see [Multiplayer](multiplayer.md).  Start a **New Campaign** if an older checkpoint loads
the empty world; those saves were preserved, not replaced.
Normal builds also omit per-function guest tracing checks. For a diagnostic
build that needs function/state tracing, configure CMake with
`-DGOW3_ENABLE_GUEST_TRACE=ON` and rebuild `gow3`; switch it back to `OFF`
and rebuild afterward. This option is off by default and is separate from
ordinary logs and MangoHud.
Linux builds automatically link the installed jemalloc allocator when available;
normal launches need no `LD_PRELOAD` setting. Configure with
`-DGOW3_HOST_ALLOCATOR=SYSTEM` to use the system allocator, or `JEMALLOC` to require
jemalloc. `AUTO` is the default and keeps the system allocator on other platforms.
Native snapshot hashing uses OpenSSL's accelerated SHA-256 when its development
package is available at configure time. Otherwise it uses the bundled portable
implementation with identical cache keys. Set `-DGOW3_NATIVE_ACCELERATED_HASH=OFF`
to build without that optional dependency. Accelerated dynamic builds require
the matching OpenSSL Crypto runtime library on the destination machine.
Mouse and keyboard controls (`mnk_mode`) are switched on in the settings GUI's
Input tab or with `--mnk_mode=true`; a controller keeps working alongside them.
Keys act as controller buttons. The mouse aims with the engine's own mouse look:
after the engine passes the controllers' axes to its viewport client
(`0x8296DCB8`), the game also passes mouse motion as the `MouseX` and `MouseY`
keys, which Gears binds to its PlayerInput mouse axes, as on PC builds of the
engine. `mnk_mouse_mode = "stick"` makes the mouse move the right stick instead,
with the game's stick aiming feel, for example for a gyro mapped to the mouse.
`mnk_sensitivity` scales either. While the window has focus the pointer is
locked: Wayland pointer constraints with unaccelerated relative motion, or a
grab with re-centering on X11.

The Controls tab binds each action to one or more keys (`keybind_*`, a
comma-separated list; `WheelUp` and `WheelDown` press briefly per wheel step).
Defaults: WASD to move, the left and right mouse buttons to aim and fire, Space
for cover, E to use, F to melee, R to reload, Q for points of interest, Shift
for the tactical view, the middle button or V to zoom, 1 to 4 or the wheel for
weapons, Tab for the scoreboard and Escape to pause. Input settings apply while
the game runs.

Button prompts name the bound keys while the keyboard and mouse was used more
recently than a controller (and at start when `mnk_mode` is on), and switch
back and forth as soon as the other device is used. Menus and
message boxes get their button glyphs from the localized
`GearGameUI.ControllerFont` strings, so `Localize` (`0x822FC8D0`) returns the
key, such as `[SPACE]`, for those entries instead. Menus resolve this text when
they set it, through `UGFxObject::SetString` (`0x8292D488`), `SetText`
(`0x8292D5C0`), `TranslateString` (`0x82942C38`) or ActionScript calls
(`0x82941BB0`, `0x82B6B5A8`). Text and calls that name controller buttons are
remembered with their markup and, when the device changes, set or made again on
the game thread from the viewport's input processing, for objects that still
exist. The HUD's action prompts are
tiles of the `WarfareHUD_ActionIcons`, `HUD_ActionIcons2`, `Gears_WeaponIcons`
and `Gears3_WeaponIcons` atlases drawn through `UCanvas::DrawTile`
(`0x82736CE0`); when a tile contains a button glyph (A, B, X, Y, the sticks and
their clicks, LT, RT, RB) the game draws the tile, then a key cap over the glyph with the canvas's
default texture and the key name with `UCanvas::WrappedPrint` (`0x82737010`).

Stereo output now uses the runtime's normalized downmix to avoid the observed
clipping when six game channels were mixed by the audio server. The latest
interactive campaign checkpoint is now in the normal `user/` save root:

```sh
./tools/run-linux.sh
```

Audio diagnostics are disabled by default. Add `--audio_diagnostics=true` to
record buffer starvation and sample levels if crackling persists after restart.

Settings are loaded from `config/gow3.toml` before graphics initialization.
Run `./tools/settings.sh` to edit them separately from the game (Python 3.11+,
PySide6 and tomli-w are required). **Window mode** offers **Windowed** and
**Borderless fullscreen**. Returning to Windowed restores the configured client
size (1280×720 by default). Click **Apply**: window mode and size, the frame-rate
ceiling, dithering, the upscaler (FSR 1, FSR 3.1, DLSS, XeSS, CAS, bilinear),
sharpening, frame generation and the latency options can update while playing.
Xenia also supports live FSR pass count and FXAA. The editor waits for the running game to
acknowledge the saved file; resource/compatibility options show **Game restart
needed** at the bottom. Reverting those options clears the notice. The editor
preserves unknown settings, keeps a `.toml.bak`, and refuses to overwrite external
edits made since loading. Explicit command-line options override the config.
Use `--gow3_config=/path/to/settings.toml` to select another file.

**Renderer** has two choices, **Native** (the default) and **Xenia**, in both
settings editors. Each shows its own supported options alongside shared display,
HDR and input controls. Changing the renderer requires a game restart.
Native owns all scene drawing, copies, visibility queries and frame sources;
it never switches to Xenia after an error. Unsupported native work stops the run
with an error. Native currently uses 1× internal resolution, hardware render targets
(the exact fragment-interlock mode remains selectable with
`gow3_native_hardware_render_targets = false`) and exact occlusion queries.
These prerequisites are
selected automatically without overwriting saved Xenia preferences.
To explicitly select Xenia:

```sh
./tools/run-linux.sh --gow3_renderer=xenia
```

Native campaign rendering, movement, real query publication and clean shutdown
have passed a saved-checkpoint test. Whole-game coverage and normal frame-rate
performance are still being improved. Native batches up to 64 draws while
retaining their resources until the batch fence completes. Queries, guest-memory
fences, dependent work and presentation drain pending draws before
publishing results. Native shader preparation starts in the background when the
game creates shader objects. Known native pipeline combinations are recorded in
the local cache and prepared behind the startup progress bar on later runs. A
draw still waits if its exact shader or pipeline is not ready; this is not a
complete cache for the whole game.

This installation also retains 409 observed native pipeline recipes from its
own gameplay captures. Startup prepares those combinations before the game
begins. New scenes can still introduce shaders or pipelines that were not seen
in those captures.

For a diagnostic comparison, `--gow3_native_draw_batch_size=1` restores a fence
per native draw. The default is 16; the accepted range is 1–64 and changes require
a restart. This internal diagnostic is not a settings-menu option. An offline
comparison preserved all 134 captured images and reduced draw submissions from
1,523 to 197. These replay timings do not establish gameplay FPS.

The default **maximum is 60 FPS**, with VSync off. This is a ceiling, not a
minimum or a forced simulation rate. In Display, choose Custom maximum,
Monitor refresh rate, or Unlimited. Unlimited removes the host ceiling;
engine limits and GPU load can still constrain performance. The unlock of the
original 30 FPS game cap is enabled independently. To select modes manually:

```toml
gow3_frame_limit_mode = "custom" # "custom", "display", or "unlimited"
framerate_limit = 60.0         # used by "custom"
vsync = false
```

Both renderers offer FSR 1, CAS and bilinear output scaling and sharpening.
Native also offers temporal upscalers, which use depth, motion vectors and
sub-pixel jitter from the scene (see below). Xenia adds 1×/2×/3× internal
rendering, FXAA and MSAA support.
Dithering is available in both renderers. Both always use the game's original
per-texture filtering; legacy `anisotropic_override` values normalize to `-1`.
FSR 1 scales to the actual window or fullscreen surface; for 720p to 4K use
native internal resolution and a 3840×2160 desktop surface. Fractional desktop
scaling uses Wayland's preferred fractional scale and viewport protocol, so
fullscreen can use the physical 4K output. Xwayland fallback may expose a
smaller surface that the compositor scales.
The runtime logs the actual swapchain dimensions.

### Render resolution

`gow3_render_height` (default 1080) sets the resolution the game itself renders
at: 1920x1080 by default, or `gow3_render_height = 720` for the console's
original 1280x720. The engine sizes its scene buffers from it and the Scaleform
menus scale to fit; no assets change. Heights round to a multiple of 8 with a
16:9 width. Above 720p the native renderer models 8192 EDRAM tiles instead of
the console's 2048, since 1080p buffers no longer fit in 10 MB. 1080p is the
practical maximum: the game stores EDRAM positions in 12 bits. Shadow map
resolutions are unchanged. Changing it needs a restart; the Xenia renderer
always renders at 720.

### Temporal upscaling, frame generation and latency (Native)

With **Upscaling** set to a temporal mode, the native renderer jitters the
camera by a sub-pixel amount each frame and hands the upscaler the scene depth,
motion vectors and a mask of the HUD. Motion comes from the camera, and for
moving characters and objects from the game's own motion-blur velocity pass
(the player, attached to the camera, is treated as still on screen). The HUD mask is taken from the frame just before
the game's first HUD draw, so HUD and subtitles are never smeared by history.
Menus, movies and loading screens have no 3D scene; they use FSR 1.

| `present_effect` | Upscaler | Platforms |
|---|---|---|
| `fsr3` | AMD FSR 3.1 | All GPUs, Linux and Windows |
| `dlss` | NVIDIA DLSS Super Resolution (NGX) | NVIDIA RTX, Linux and Windows |
| `xess` | Intel XeSS Super Resolution | Windows (also Proton/Wine); any recent GPU |

DLSS and XeSS need the vendor SDKs fetched at build time
(`tools/fetch-vendor-sdks.sh`) and fall back to FSR 3.1 when the GPU, driver or
library cannot run them; the log says which upscaler is active. FSR 4 and XeSS
frame generation exist only for Direct3D 12 and are not available to this
Vulkan renderer.

FSR 3.1 at 720p to 4K (a 3× ratio, AMD's Ultra Performance) leans on about
60 frames of history. Surfaces that animate without motion vectors, such as
grass in the wind, can smear, and a still image is softer than with FSR 1.
`present_fsr3_ghosting_reduction` (0 to 1, default 1) shortens that history: it
writes a minimum reactive value everywhere (up to 0.6) and lowers FSR's
accumulation per frame (from 0.333 to 0.1). In an offline test against a known
pattern, 1 cut the error on swaying content by about a quarter, with more
shimmer from frame to frame. FSR 1 has no history and so no ghosting at all.

**Frame generation** (`present_frame_generation = true`, with any temporal
upscaler) uses FSR 3.1 frame interpolation: between two game frames it shows a
generated frame, then the real frame half a game frame after the generated
one has finished on the GPU. Motion looks smoother;
input still follows the real frame rate and the real frame is shown slightly
later, so it suits a base rate of 60 FPS or more. The HUD of the real frame is
drawn over generated frames. On an RX 9070 XT at 4K the base rate in the Act 1
pan drops from about 135 to about 83 FPS, presenting about 166 frames per second.

**Low latency** (`gow3_low_latency = true`) keeps the game at most one frame
ahead of the renderer and applies the frame-rate limit on the game thread just
before the next frame reads input, instead of holding finished frames back. It
cost no measurable throughput in testing. **AMD Anti-Lag**
(`present_anti_lag = true`) additionally uses the driver's `VK_AMD_anti_lag`:
built into AMD's Windows driver, and on Linux provided by Mesa's anti-lag layer,
which the game enables at startup (so turning it on takes effect after a
restart there). Both options can be combined with frame generation.

Native Wayland presentation is the default. The launcher first checks Vulkan
Wayland support; if unavailable it uses X11/Xwayland with HDR disabled. Set
`GDK_BACKEND=x11 ./tools/run-linux.sh` for an explicit SDR compatibility run.

The HDR tab enables native Vulkan HDR10 output (10-bit BT.2020/PQ), without
Gamescope. Both the settings app and the renderer inspect the compositor's
**active** output color description, and require Vulkan HDR10 surface support.
Missing/failed detection disables HDR controls and the renderer falls back to
SDR even if the config requests HDR. The renderer rechecks when the surface
moves between outputs or the compositor reports an output color change;
the standalone editor refreshes its independent probe every ten seconds.
Paper white defaults to 203 nits and peak brightness to the output's report
(0 means automatic); custom calibration and highlight strength are exposed.
This is HDR presentation with highlight expansion from the completed SDR game
image. It does not recover original pre-tonemap engine lighting or extra scene
detail. HDR output/calibration changes currently require a game restart.

Both settings interfaces show read-only **VRR (FreeSync)** status when the
Wayland compositor reports it. The game leaves desktop VRR policy untouched.
An enabled status reports the monitor's configuration, not proof of variable
scanout on every frame. VSync and the frame-rate ceiling remain separate options;
turning VSync off does not itself enable VRR.

The in-game settings menu uses the same option schema and config as the
standalone app. **L3+R3** (press both stick buttons) toggles it; **F4** is the
keyboard shortcut. Use the D-pad/left stick to select a category, then **A**
to enter its options. Up/down moves between enabled options; left/right changes
the focused value (hold **RB** for faster numeric adjustment). **B** returns
to the category list; B again closes. **X** applies and **Y** reloads saved
settings. Keyboard equivalents are arrows, Enter, Escape, Ctrl+Enter to apply,
F6 to reload, and Shift for faster adjustment. Each category remembers its
last option, and the selection scrolls into view automatically.
Gameplay controls are blocked while the menu is open. Release both stick buttons
before pressing the shortcut again; the Guide button does not toggle settings.
Shader loading uses a plain red bar and white text; the menu uses matching red, white and dark
colors. Simultaneous edits in the other editor require Reload before saving.

For compatibility rendering, the default config enables asynchronous shader translation and pipeline compilation,
using all detected logical CPU threads in one shared worker pool. Newly encountered
effects can briefly appear late while this work finishes. Draws that export data
to game memory complete their shader preparation synchronously to preserve those
writes. The Shaders tab exposes Background shader translation; disable it with
`--vulkan_async_shader_translation=false` for a comparison. Earlier asynchronous
pipeline changes reduced stutter; the additional translation change removed
GPU-thread translation work but the user reported similar overall stutter.
Some stalls occur without shader compilation.

Asynchronous visibility queries are also enabled for compatibility rendering. GPU query results are
published after their submission fences complete, with pending batches drained
before the command thread waits. This removes the previous CPU wait after
every individual visibility query. The Advanced tab exposes the option;
for a comparison run, use `--vulkan_async_occlusion_queries=false`.
Group visibility queries also combines queries into normal submission batches;
`--vulkan_batch_occlusion_queries=false` restores per-query submissions while
retaining asynchronous result collection. Guest memory waits and command-thread
idle submit and drain pending results, including batches not yet submitted.
The runtime still uses real query results and protects query storage against
reuse while GPU work is pending. This improves synchronization overhead;
heavy GPU scenes can still fall below 60 FPS.

The game config disables optional CPU readback of shader memory exports
(`readback_memexport = false`), matching the working Xenia comparison. The prior
run's rare 0.57–1.65 second freezes coincided with 1,638–3,595 synchronous
readbacks in a single frame. GPU shader exports still execute normally. The
Advanced tab exposes Copy shader output to CPU memory and Buffered shader
readbacks for compatibility testing; enabling CPU copies can bring those stalls
back. Longer gameplay validation of the disabled setting is ongoing.

Startup shows a progress bar while translating and compiling shaders discovered
in previous play sessions. The compatibility cache consolidates 2,472 shaders and 3,130
pipeline descriptions from our runs, with source caches retained. Native
rendering has its own versioned shader cache and observed pipeline recipes. It is not a
complete whole-game cache. The initial compilation with an empty Mesa cache
took about 14 seconds on this machine; subsequent driver-cache hits are faster.

The version-checked frame-rate unlock is enabled by default. Gameplay has been
observed around 40–60 FPS, including about 53 FPS in one fire-heavy ship scene;
this does not guarantee locked 60 FPS. Prerecorded movies retain their original
frame rate. The movie pacing option reduces the idle render polling interval
from 25 ms to 1 ms; it does not change the video's playback clock or shared game
constants. To restore the original game cap, restart with:

```sh
./tools/run-linux.sh --gow3_60fps=false
```

The frame-rate unlock hook sites follow the [Xenia Canary Unlock FPS patch by illusion and
boma](https://github.com/xenia-canary/game-patches/blob/main/patches/4D5308AB%20-%20Gears%20of%20War%203.patch.toml).
They are regenerated from `config/gameplay-hooks.toml`, and the original
instruction bytes are checked before activation. `game/default.xex` is unchanged.

Build prerequisites on the current machine are Clang 22, CMake, Ninja,
Python 3, GTK 3 development files, and the vendored SDK dependencies.
The build uses eight jobs by default (`GOW3_BUILD_JOBS` overrides this).
`validate.sh` returns failure if any checks fail; existing SDK failures are
documented in `STATUS.md`.

## Artifacts and limits

- `generated/default/`: instruction-level C++ translation, used by the build.
- `analysis/decompiled/functions/`: unverified Ghidra C pseudocode, **not**
  the input to the Linux build or a recovery of the original project.
- `analysis/decompiled/functions.tsv`: per-function export status.
- `reports/progress.json`: mechanically checked counts and executable identity.
- `reports/input-inventory.json` and `reports/iso.sha256`: input provenance.
- `logs/`: generation, compilation, tests, and runtime diagnostics.
- `reports/linux-first-boot.png`: observed native title screen.
- `reports/rendering/native-offline-start.png`: fresh campaign prison cinematic.
- `reports/rendering/native-offline-gameplay.rdc`: native prison gameplay capture.
- `reports/rendering/findings.json`: rendering diagnosis and validation scope.
- `reports/rendering/vertex-residency-fix.patch`: fix for stale animated vertex data.

Ghidra exported 41,369 functions recognized in a copied analysis database,
after verifying its executable SHA-256. Of those, 7,477 contain warnings.
Some functions contain truncated control flow from unsupported instructions.
The database does not establish complete discovery or correct semantics.
Types, class layouts, names, build organization, and correctness still need
extensive reconstruction and validation.

`tools/decompile.sh` repeats the export using the saved local analysis
database. It defaults to the installed Ghidra and the adjacent JDK 21;
`GHIDRA_HEADLESS` and `GOW3_JDK` override those paths. Creating a fresh database
from the ISO, and full analysis with Xenon instruction support, are not yet
automated by this wrapper.

See `STATUS.md` for the checkpoint and unresolved work. Game assets, generated
game code, and captures are local artifacts excluded from version control.
