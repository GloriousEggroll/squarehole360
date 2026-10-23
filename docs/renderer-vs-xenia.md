# How the Gears of War 3 native renderer differs from Xenia

## In one sentence

Xenia reads the low-level command stream the game sends to the Xbox 360 GPU
and emulates that GPU in Vulkan. Our renderer captures the game's Direct3D
calls instead and renders the scene directly with native Vulkan render
targets, closer to how DXVK translates Windows D3D games than to how an
emulator works.

## How Xenia renders

1. The game's D3D library writes PM4 command packets into a ring buffer.
   These are the raw instructions the Xenos GPU understands.
2. Xenia's command processor parses those packets, rebuilds the GPU's
   register state, and works out what each draw means.
3. It translates Xenos shader microcode to SPIR-V or DXIL at runtime.
4. It emulates the 360's 10 MB EDRAM, the special memory where the GPU
   renders. Its most accurate mode (FSI/ROV) does depth, stencil and blending
   inside pixel shaders, writing into a big buffer laid out like EDRAM.
5. It copies results out to "main memory" the way the real GPU does, then
   untiles and hashes textures from emulated memory.

This approach is accurate and general, because it works for any game. But it
pays for re-deriving everything from low-level commands and for doing
hardware jobs in shaders.

## How our renderer works

1. **The game is statically recompiled** (rexglue-sdk), so it runs as native
   x86-64 code, not under a JIT.
2. **We hook the game's Direct3D functions**: draw calls, SetRenderTarget,
   Present and occlusion queries. At each draw we capture the D3D device
   state, the shaders, and the vertex, index and texture data the draw
   actually uses.
3. **A planner** turns those captured calls into a typed scene plan: draws,
   clears, resolves and queries, with their dependencies.
4. **A native executor** renders that plan with Vulkan:
   - Shaders are translated ahead of time and cached. Pipelines are
     prewarmed or built in the background, so new effects don't freeze the
     frame.
   - **Hardware render targets** (the default): every render target is a
     real GPU image, and depth, stencil and blending use the GPU's
     fixed-function hardware. EDRAM is only modeled where the game depends on
     it, such as reusing the same memory as a different format, or copying a
     render target out to a texture.
   - Occlusion queries, texture tracking and resolves are handled natively
     with caching and dependency tracking. The renderer doesn't re-check guest
     memory on every draw.
5. **Presentation is native:** Wayland HDR and VRR, with no emulated video
   output.

## Side by side

| | Xenia | Ours |
|---|---|---|
| Where it hooks in | GPU command packets (PM4) | Game's Direct3D calls |
| CPU code | Translated while the game runs (JIT) | Recompiled ahead of time |
| Shaders | Translated at runtime | Translated ahead of time, cached, async |
| Render targets | Emulated EDRAM (shader-based in accurate mode) | Real Vulkan images with hardware blend and depth |
| Scope | Any 360 game | Built specifically for Gears of War 3 |
| Measured cost | — | GPU cost per draw is about half of our own FSI mode (the Xenia-style accurate approach) |

## Things to state accurately

- **Not 100% Xenia-free.** The runtime (rexglue-sdk) comes from Xenia, as do
  the texture untiling (`texture_data.cpp`), the EDRAM address math in
  `resolve_memory.cpp`, and the FSI precision math.
- **The game still builds its GPU command buffers.** We just don't use them
  to draw. Removing that step is planned as stage 4 in
  [direct-renderer-plan.md](direct-renderer-plan.md).
- **The exact Xenia-style mode still exists.** It's FSI, enabled with
  `gow3_native_hardware_render_targets = false`. The default hardware mode
  trades a little precision for speed: 7e3 colour targets aren't quantized on
  every write, and depth isn't rounded to float24 per pixel.
