# Changelog

## v0.1.1 (2026-11-09)

### Fixes

- **AMD Radeon on Windows now starts.** The game showed a black window and
  closed with "Native renderer requires 1x FSI storage" on every AMD GPU
  using AMD's Windows driver, which lacks fragment shader interlock. The
  default renderer mode (hardware render targets) never needed it, and no
  longer asks for it (#1).
- **New campaigns with Networking off.** With Networking turned off, starting
  a new campaign dropped you into a black, empty level where your character
  fell until "Objective Failed". The game now behaves like a console with no
  network cable: it still works offline, and nothing leaves your PC.
- The Networking setting now takes effect when the game restarts, instead of
  breaking a running game when switched.

### For testers

- `vulkan_fragment_shader_interlock = false` in `config/gow3.toml` makes any
  GPU behave like one without interlock (as AMD's Windows driver does).

## v0.1.0 (2026-11-09)

First release.

- Windows (`gears3launcher.exe`) and Linux packages, with a setup wizard for
  your own disc image, Title Update 6 and DLC.
- Dedicated lobby server included: `gears3-lobby` (`gears3-lobby.exe` on
  Windows).
- `gow3_texture_pool_mb` setting for the texture streaming pool (0 keeps the
  game's 145 MB).
