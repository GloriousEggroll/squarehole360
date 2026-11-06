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
folder with the extracted disc files, imports them, asks for Title Update 6,
then offers to add DLC from folders or `.zip`, `.tar.gz` and `.tar.xz`
archives. The same steps work from the command line:

```sh
python3 tools/import-assets.py import "/path/to/gow3.iso"   # or a game folder
python3 tools/import-assets.py update ~/Downloads            # Title Update 6 (file, folder or archive)
python3 tools/import-assets.py dlc ~/Downloads               # DLC folders/archives
python3 tools/setup-wizard.py --dlc                          # add DLC later
python3 tools/setup-wizard.py --shaders                      # rescan and precompile shaders by hand
```

At start, before the game runs, the native renderer compiles every pixel
shader in the game files that isn't compiled yet, behind a progress bar
("FINDING SHADERS", then "PREPARING SHADERS"). The first visit to an area then
doesn't stall while its shaders compile. It decompresses the cooked packages,
extracts the LZX-compressed shader units from each package's shader cache, and
compiles them with the game's own compiler into `user/cache/native-shaders`.

With the DLC that's about 142,000 shaders: roughly 20 minutes on a 16-thread
CPU and 7.4 GB of cache, on the first start only. Later starts check the
package files' names, sizes and times against the last complete run, which
takes a fraction of a second; new DLC or a changed compiler triggers a rescan
that only compiles what's missing. Closing the window during the first run is
safe: what's compiled is kept and the next start continues. Set
`gow3_native_shader_precompile = false` to skip it. Vertex shaders are not
covered: the game links them against each mesh's vertex layout when it loads
them, which rewrites their instructions, so they still compile in game.

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

### Title Update 6

The game code is recompiled from the disc's `default.xex` with Title Update 6
applied, the last update for the game; RAAM's Shadow needs it (its campaign
act comes from the update's script). Title updates are made per disc version:
the one for the supported disc (media ID 58122AD6) is
`TU_16L625B_000001C000000.00000000001GB`, and the wizard finds it by that name
in folders and archives, checks it and unpacks it to `user/update/`. The game
mounts that folder as `update:` and applies its `default.xexp` when it loads
the executable, the same patch `rexglue codegen` applies
(`patch_file_path` in `gow3_manifest.toml`). The game does not start without
it. `tools/porting/xex_address_map.py` maps disc addresses to the updated
executable by pairing functions through `.pdata`.

### Downloadable content

The setup wizard and `import-assets.py dlc` find your own DLC packages (files
named like `4D5308AB/00000002/85F354E7...`) in folders and archives, check them,
and copy them to `user/dlc/`; packages can also be put there by hand. The next
start installs each new package once into `user/0000000000000000/4D5308AB/00000002/`,
where the game finds it like content on a console hard drive; the packages in
`user/dlc/` can be deleted afterwards. The map packs, the Horde Command Pack and
RAAM's Shadow are supported. Booster's "Swamp" map is listed as Bullet Marsh.

### Language

**Language** in the settings (`gow3_language`) picks the game's text and
speech language; it applies at the next start. The game chooses it from the
console language and, for Czech, Hungarian and Spanish (Spain or Latin
America), the console country, so the setting sets both. Each disc carries only
some languages (the supported one has English, French, Chinese, Czech,
Hungarian, Korean, Polish, Portuguese and Russian; others are on other
regions' discs). A language missing from `game/` (no
`GearGame/CookedXbox360/Coalesced_<code>.bin`) falls back to English, with a
warning in the log.

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
Mouse and keyboard controls (`mnk_mode`) are on by default; switch them off in
the settings GUI's Input tab or with `--mnk_mode=false`. A controller keeps
working alongside them.
Keys act as controller buttons. The mouse aims with the engine's own mouse look:
after the engine passes the controllers' axes to its viewport client
(`0x8296DCB8`), the game also passes mouse motion as the `MouseX` and `MouseY`
keys, which Gears binds to its PlayerInput mouse axes, as on PC builds of the
engine. `mnk_mouse_mode = "stick"` makes the mouse move the right stick instead,
with the game's stick aiming feel, for example for a gyro mapped to the mouse.
`mnk_sensitivity` scales either. While the window has focus the pointer is
locked: Wayland pointer constraints with unaccelerated relative motion, or a
grab with re-centering on X11.

Each player slot has its own controller settings (`controller1_*` to
`controller4_*`; settings app tabs Controller 1 to 4). Player 1 stays the main
profile's controller: the game remembers it (`user/player1-controller.txt`)
and gives it player 1 at startup even if another pad is listed first;
controllers connected while playing take the next free slot.

Controllers with a gyro (DualSense, DualShock 4, Switch Pro and others SDL
reads motion sensors from) can aim with it; it is off by default
(`controllerN_gyro_mode = "off"`). `"mouse"` passes the rotation through the
same mouse look, at one camera degree per controller degree times
`controllerN_gyro_sensitivity` (the game turns 60/65536 of a turn per mouse
count); `"joystick"` adds it to the right stick instead, starting at
`controllerN_gyro_stick_deadzone` (the game's 0.3 look dead zone). Like Steam
Input, it is then always on, unless `controllerN_gyro_activation` is `"aiming"`
(while the left trigger is held) or `"button"` (while an input mapped to
`gyro` is held). `controllerN_gyro_turn_axis` picks yaw (the default), roll or
player space. The gyro calibrates itself whenever the controller rests. Older
configs' `gyro_*` settings become player 1's.

`controllerN_buttons` remaps inputs as comma-separated `source=target` pairs,
for example `rpaddle1=gyro,lpaddle1=a,touchpad=back`; unlisted inputs keep their
defaults. Sources are the physical inputs (face buttons by position: `a` is the
bottom one), including the DualSense Edge and Elite back paddles, the touchpad
click and both triggers; targets are any game button or trigger, `none` or
`gyro`. The settings app edits it with a dropdown per input. Each slot can also
swap the sticks, invert the right stick vertically, and add stick dead zones
for a drifting stick.

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
sharpening and the latency options can update while playing.
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
The Xenia renderer is for development and testing only: release packages
(built with `-DGOW3_XENIA_RENDERER=OFF`) and the settings menus offer only the
native renderer. In a dev build from `tools/build-linux.sh`, select Xenia with:

```sh
./tools/run-linux.sh --gow3_renderer=xenia
```

AMD Anti-Lag stays off with the Xenia renderer: Mesa's anti-lag layer crashes in
`vkQueuePresentKHR` there.

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
Dithering is available in both renderers. **Anisotropic filtering**
(`anisotropic_override`, Image tab) defaults to the game's own per-texture
filtering (`-1`); 2x, 4x, 8x and 16x (`2` to `5`, the Xenos encoding) apply
to every mipmapped, bilinear-filtered texture, leaving point-sampled and
unmipped ones (lookup tables, render targets) as they are. It applies while
playing; other saved values fall back to the game's filtering.
**Motion blur** (`gow3_motion_blur`, Image tab, native renderer, live) is off
by default. Gears 3's blur is part of its post-process passes: a tile pass
(pixel shader `8e22319c…`, velocity scale in c12.xy) and a gather pass
(`f5e7c3c5…`, c11), which read the velocity buffer resolved after the
velocity pass. Off zeroes those scales for the draws that sample that buffer,
so the passes still run (bloom, tone, everything else) without blurring.
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
menus scale to fit; no assets change. Heights round to a multiple of 8; the
width follows the aspect ratio below. Above 720p the native renderer models 8192 EDRAM tiles instead of
the console's 2048, since 1080p buffers no longer fit in 10 MB. 1080p is the
largest size the game itself can lay out: it stores EDRAM positions in 12 bits.

`gow3_render_height = 1440` keeps the game's 720p layout and has the native
renderer draw every screen-sized target at twice the size behind its back
(2560x1440 with 16:9): render targets, the copies the game samples back
(resolves), viewports and pixel positions are scaled, while everything the
game itself sees stays at 720p. Targets smaller than half the frame (colour
grading tables, the bloom chain) stay at their own size. Shadow maps are
scaled too. See [resolution-scaling-plan.md](resolution-scaling-plan.md).
Changing the resolution needs a restart; the Xenia renderer always renders
at 720.

### Aspect ratio and field of view

**Aspect ratio** (`gow3_aspect_ratio`, default `auto`) sets the shape the game
renders: `auto` takes the window's when the game starts; 16:9, 16:10, 21:9 and
32:9 are fixed. The game itself only renders 16:9: its player camera always
constrains the view to that aspect and letterboxes anything else. A hook on
`ULocalPlayer::CalcSceneView`'s projection (`FPerspectiveMatrix` with the
camera at r9) writes the render aspect into the camera instead, so the view
fills the screen. Wider than 16:9 keeps the 16:9 vertical field of view and
shows more at the sides (Hor+); 16:10 keeps the horizontal one and shows more
above and below. Cinematic cameras with their own aspect keep it. Menus and
the HUD are laid out for a 16:9 stage, so on wider screens their Scaleform
viewport is narrowed to the middle 16:9 area. Wide sizes keep at most
1920x1080's pixels (21:9 renders 2200x928, 32:9 2704x760) because of the EDRAM
limit above; the upscaler scales to the window. The presenter shows the frame
at its own aspect (`present_frontbuffer_aspect`), since the console video mode
stays 1280x720. The game's Bink player fits prerecorded movies to the screen
at their own aspect, with black bars at the sides. Needs the native renderer
and a restart.

**Field of view** (`gow3_fov`, default 65, the game's) is in degrees,
horizontal at 16:9, and applies while playing. It scales every camera's field
of view, so aiming and zooming keep their proportions; cutscenes widen too.

**HUD size** (`gow3_hud_size`, Display tab, applies while playing) sizes the
in-game HUD compared with the console's. The game draws parts of its HUD
(weapon panel, ammo, prompts, subtitles) at fixed pixel sizes, so they look
smaller at 1080p than at 720p. Automatic (0) keeps that behaviour for every
internal resolution: 100% at 720p, 67% at 1080p, 50% at 1440p, which would
otherwise keep the 720p layout's size. A percentage (25-200) fixes the size
instead. `src/gow3_hud_scale.cpp` lets the HUD lay out on a virtual screen:
after `UCanvas::Update` (0x827483C0) it sets the canvas size and clip size to
the virtual size and scales the canvas transform to match. Scaleform menus
are not affected.

### Ray-traced ambient occlusion (Native)

**Ray-traced ambient occlusion** (`gow3_rt_ao`, off by default; Ray tracing
tab) adds soft shadows where surfaces meet and objects touch, traced against
the frame's real geometry with the GPU's ray tracing. It needs a GPU and
driver with Vulkan ray queries and transform feedback (AMD RX 6000 or NVIDIA
RTX and newer; not macOS) and costs about 2.5 ms per frame at 1080p on an
RX 9070 XT. Strength, reach and rays per pixel apply while playing.

How it works (`src/renderer/native/ray_traced_ao.cpp`): each scene draw that
writes depth without alpha test is drawn a second time with a vertex-only
pipeline whose transform feedback stores its clip-space triangles; the game's
own draw is unchanged. Before the HUD (or the final resolve when there is no
HUD) the triangles become camera-relative positions through the frame's camera
(VS c0..c3, c5), one bottom-level and one top-level acceleration structure are
built, and rays from the game's depth resolve are traced at half resolution,
accumulated over frames with the camera motion and upsampled by depth. The
result multiplies the tonemapped colour, less on bright pixels (direct light,
glow and light shafts), and goes back into the colour target the HUD is drawn
on. Alpha-tested surfaces (foliage, fences) and translucency cast no
occlusion; fog in front of a surface is darkened with it. `gow3_rt_ao_debug`
shows the occlusion alone.

### Ray-traced shadows (Native)

**Ray-traced shadows** (`gow3_rt_shadows`, off by default; Ray tracing tab)
trace the shadows of the game's dynamic shadow-casting light (the dominant
light: the sun outdoors, often a lamp indoors) against the frame's geometry.
**Shadow softness** (`gow3_rt_shadow_softness`, 0 to 1, default 0.25) widens
the light; 0 gives hard shadows. Both apply while playing. Same GPU
requirements as the ambient occlusion above. On an RX 9070 XT at 1080p they
cost about 3.5 ms per frame soft and 1.5 to 2 ms hard; with ambient occlusion
also on, the two together cost about 4 ms, since they share the frame's
acceleration structures.

Gears 3 bakes most of its lighting, sun shadows included, into lightmaps;
only the dominant light is shaded in real time, and only on materials that
accept dynamic shadows (characters, most walls and floors near them). Its
shadow maps are per object: each caster's depth is drawn from the light,
projected into the red channel of a frame-sized light attenuation buffer
(1 lit, 0 shadowed), and the base pass multiplies the light by that buffer.
The renderer hooks in there (`src/renderer/native/ray_traced_ao.cpp`,
`tools/rendering/native/rt_shadow.comp`):

- A projection draw writes only red into a frame-sized target, with the
  shadow buffer's size in PS c7.xy; its ScreenToShadow matrix (PS c8..c11,
  input `(ndc.xy * w, w, 1)`) combined with the camera gives world to shadow
  space, whose centre of projection is the light (perspective, a local light)
  or whose depth axis points away from it (orthographic, a directional one).
- Before that buffer is resolved for the base pass, the geometry captured so
  far (the depth prepass) becomes the frame's acceleration structures, and
  every pixel of the depth resolve is traced towards the light. The buffer
  keeps the darker of the game's value and the traced one, so the RT can only
  add shadow: a caster that was not captured (alpha-tested bars, foliage,
  geometry outside the view) still has the game's shadow map.
- Soft shadows trace two rays towards a disc the size of the light, two more
  where those disagree, and blur the result by depth before merging it.
  Hits within 15% of a local light's distance are ignored (lamps sit in
  fixtures the game does not draw into its shadow maps).
- Ray-traced AO, when on, reuses the same acceleration structures later in
  the frame.

The light comes only from the frame's projections, so frames where the game
draws no dynamic shadow get none traced either. Surfaces that are lit only
by lightmaps do not change. `gow3_rt_shadows_debug = 1` uses the traced
visibility alone, `2` shadows everything (shows which surfaces take the
dynamic light).

### Temporal upscaling and latency (Native)

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
library cannot run them; the log says which upscaler is active. FSR 4 exists
only for Direct3D 12 and is not available to this Vulkan renderer. There is no
frame generation.

FSR 3.1 at 720p to 4K (a 3× ratio, AMD's Ultra Performance) leans on about
60 frames of history. Surfaces that animate without motion vectors, such as
grass in the wind, can smear, and a still image is softer than with FSR 1.
`present_fsr3_ghosting_reduction` (0 to 1, default 1) shortens that history: it
writes a minimum reactive value everywhere (up to 0.6) and lowers FSR's
accumulation per frame (from 0.333 to 0.1). In an offline test against a known
pattern, 1 cut the error on swaying content by about a quarter, with more
shimmer from frame to frame. FSR 1 has no history and so no ghosting at all.

**Low latency** (`gow3_low_latency = true`) keeps the game at most one frame
ahead of the renderer and applies the frame-rate limit on the game thread just
before the next frame reads input, instead of holding finished frames back. It
cost no measurable throughput in testing. **AMD Anti-Lag**
(`present_anti_lag = true`) additionally uses the driver's `VK_AMD_anti_lag`:
built into AMD's Windows driver, and on Linux provided by Mesa's anti-lag layer,
which the game enables at startup (so turning it on takes effect after a
restart there).

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

VRR (FreeSync/G-Sync) needs no game setting: when the display and compositor
have it enabled, it applies automatically. The game leaves desktop VRR policy
untouched.

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

In the native renderer a draw whose shader is still compiling is skipped (the
object appears a moment late). `gow3_native_async_shader_budget_ms` (default
5000) bounds that: past it, the draw waits for the shader instead. A shorter
budget turns more pop-in into stalls, not fewer.

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
