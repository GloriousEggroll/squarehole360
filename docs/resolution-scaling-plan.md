# Internal resolution scaling (native renderer)

Goal: render above 1080p without the guest's 12-bit EDRAM tile bases getting
in the way. The guest keeps a layout that fits EDRAM (1280x720), and the
native renderer draws every render target at an integer scale behind its
back, as Xenia's draw resolution scaling does.

Settings (`gow3_render_height`, Display > Internal resolution):

| Choice | Guest layout | Host scale | Rendered |
|---|---|---|---|
| 720p  | 1280x720  | 1x | 1280x720  |
| 1080p (default) | 1920x1080 | 1x | 1920x1080 |
| 1440p | 1280x720  | 2x | 2560x1440 |

1080p stays a guest-side size (gameplay_hooks.cpp), as today. Only 1440p uses
the host scale, so 720p and 1080p keep their current, tested paths.

## Scope

Only the default hardware render target path
(`gow3_native_hardware_render_targets = true`): each guest surface is a
Vulkan image and EDRAM is a storage buffer used when surfaces alias. The FSI
path and its packed fast paths (hard-coded 1280x720 layouts, 2048-tile
shaders) stay 1x; a scale above 1 with FSI is rejected at startup.

## Design

- **Scale.** `NativeRenderScale()` (1 or 2), set once before the renderer
  starts, next to the EDRAM model (edram_extent.h). Guest-visible state
  (EDRAM addresses, surface sizes, resolve rectangles, fetch constants) stays
  in guest pixels everywhere; only host images, viewports, scissors and
  shader pixel positions are scaled.
- **Surfaces.** Host images at width x s by height x s. Viewport and scissor
  are multiplied when applied; the half-pixel offset is in clip units and
  carries over. Checks against literal 1280x720 compare guest sizes and stay.
- **EDRAM transfers.** Storing a surface into the EDRAM buffer averages
  (colour) or picks the nearest sample (depth/stencil) of each s x s block;
  restoring replicates. resolve_memory.comp gets the scale. These only run
  when surfaces alias, so the loss of detail is limited to those cases.
- **Clears.** Rectangles scaled.
- **Resolves.** The source rectangle is scaled; the destination host image is
  kept at s x the guest size and used for sampling (DirectResolveTexture).
  The guest-memory mirror (ResolveMemory) stays authoritative at 1x: the
  store downsamples. Partial resolves, format aliases, memexport and CPU
  overlays keep using the 1x mirror, which loses only resolution.
- **Sampling scaled textures.** A per-binding scale bit in the shared
  constants. For those bindings the shader helpers divide image dimensions
  by s (unnormalized coordinates, fetch offsets, bilinear weights, LOD), as
  Xenia's fetch translator does. Changes generated shader code: needs a
  shader cache version bump.
- **Pixel position.** VPOS (`iPos.xy - 0.5`) is divided by s.
- **Occlusion queries.** Sample counts divided by s x s.
- **Temporal inputs, ray-traced AO/shadows, presentation.** Sizes from host
  images; comparisons against guest sizes scaled.
- **Game hooks.** At 1440p the guest layout stays 1280x720 (no screen size
  override); the EDRAM model stays 2048 tiles.

## Status (2026-11-03)

Working in harness runs of the Act 2 opening (menu, cutscene, gameplay),
with and without ray tracing and FSR 3.1:

- Surfaces of at least 640x352 guest pixels are drawn at 2x; smaller ones
  stay 1x (`SurfaceScaleFor`). Their contents are computed per guest texel,
  so enlarged they only repeat texels and filtering between repeats becomes
  a staircase (this showed as speckles in dark areas through the colour
  grading table).
- EDRAM stores read the host image one sample per guest pixel (store input
  scale in `resolve_memory.comp` format bits 24-26); restores decode at
  guest size and enlarge (colour: nearest blit; depth/stencil: the restore
  draw reads at the scale, since D32_SFLOAT_S8_UINT has no BLIT_DST on RADV).
- Resolves read the scaled source and keep a scaled image for sampling; the
  guest-memory copy stores one sample per guest pixel. Format aliases decoded
  from guest memory are enlarged (nearest) for scaled draws.
- Shaders: bit 30 of a 2D descriptor index marks a scaled image;
  `getTexture2DDimensions` reports guest texels. VPOS is divided by the scale
  (shared constant 544). Occlusion query counts are divided by scale squared.
- Ray-traced AO traces one texel per 4x4 host pixels at 2x (the same ray
  count as unscaled); shadows trace one point per 2x2 host pixels (the
  guest pixel count; 3.3 -> 1.25 ms in the Act 2 opening), filtered by depth
  when merged.

2026-11-04 fixes (the 1440p grain):

- The front buffer is presented at host size (2560x1440). It used to be
  decoded from guest memory at 1280x720, so FSR 3 rejected every frame (scene
  inputs 2560x1440) and fell back to spatial FSR 1 on a jittered 720p image:
  the grain. The SDK's swap sizing treats a native source that is an integer
  multiple of the guest swap as scaled instead of cropping it.
- Sampled resolves bind the resolve's own scaled image
  (`direct_resolve_textures`, forced on at scale > 1; format 54 included).
  Decoding them from guest memory enlarged every one from guest pixels.
- Full-resolution EDRAM copy (`ResolveMemory::EnableScaledCopy`): a second
  buffer with scale x scale samples per word, written by every surface
  store; restores and fixed-point resolve sources of scaled surfaces read it
  when every byte is current there (any other EDRAM write marks bytes stale).
- Partial resolves over an image a draw still samples copy that image.

Harness, start of the user's save, RT off: 11.5 -> 8-9 ms per frame.

Not done: occlusion queries are divided by the global scale even for draws
into unscaled targets; the FSI path stays 1x.
