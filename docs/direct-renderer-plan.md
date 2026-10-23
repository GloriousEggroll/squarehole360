# Direct renderer plan

Goal: remove the remaining emulated layers between Gears of War 3's Direct3D
calls and Vulkan, the way Unleashed Recompiled does. Today the game still builds
Xbox PM4 command buffers. The SDK command processor (CP, the "GPU Commands"
thread) reads every packet, and the native renderer matches packets to captured
API calls before it executes them. Every draw also goes through EDRAM emulation
via fragment shader interlock (FSI).

Measured cost (2026-10-16, combat):
- The guest D3D thread spends about 3 ms per frame spinning in GetData for the
  previous frame's occlusion results. Each result waits for the CP and then the
  GPU.
- The GPU spends about 10 µs per draw (about 1,080 draws per heavy frame,
  10.8 ms). Even tiny draws cost 4–8 µs.
- Batch size, fence count and query polling have been tuned already. Batch
  16, two fences and a 1024-retry poll are the best settings found.

## Stage 1: no command processor (direct execution)

The guest's D3D runtime has a built-in mode for running without a GPU. It sets
it after detecting a hang, through global `0x8342AED4` and `device+0x2B3D`
bit `0x2`. In this mode:

- `sub_82AF5008`, the ring submit, writes neither the ring nor
  `CP_RB_WPTR`.
- `sub_82AF5498`, the fence marker emitter, stores the completed serial and
  command-buffer position directly into the completion block (`*(device+0x2B10)`
  +0 and +4) when no PIX hook is installed (`device+0x5534 == 0`). Fences
  therefore complete immediately on the CPU.
- `sub_82AF58B8`, KickOff, and `sub_82AF6B10`, Present, have matching
  branches.

Plan:

1. Set `device+0x2B3D |= 2` once the device exists. This happens in direct mode
   only, behind a restart-only renderer option.
2. Drive the existing native executor straight from the planner, in API order,
   on the CP thread through `CallInThread`. That keeps Vulkan queue,
   query-publication and presenter thread affinity unchanged. The CP thread is
   otherwise idle because no packets arrive.
   - Query prefixes: run them as soon as they are planned, so results publish as
     early as possible.
   - Frames: run the rest of the frame, then present. Replace the XE_SWAP path
     with a direct swap.
3. GetData: the serial gate no longer waits, and the guest's MEM_WRITE
   sentinels are no longer executed. The GetData hook must block until this
   query's real result is published, then run the original body.
4. Synthesize source-1 graphics interrupts (PM4 INTERRUPT packets emitted by
   `sub_82AF5AB0` and others) at the matching API point, if the guest turns out
   to depend on them.
5. Memexport (Gears' GPU memcpy kernel) keeps writing guest memory when
   executed. Reference mode works with `readback_memexport=false`, so the CPU is
   not expected to read it early.
6. Keep frame backpressure on the guest thread. Fences now complete instantly,
   so this is what stops the game from racing ahead.

Expected effect: no PM4 decoding, no packet matching, no CP latency in the query
round trip, and no CP thread competing for CPU.

Validation: captured-scene planner tests, live boot and checkpoint runs, then
user play tests. Compare frame-interval logs and `query_wait_ms_per_frame`
against the current renderer.

### Stage 1 status (2026-10-16): working; default for the native renderer (`gow3_direct_execution`)

- `VdIsHSIOTrainingSucceeded` returns 0 in direct mode, so the guest D3D boots
  GPU-less.
- The CP thread runs posted work through `PostDirectWork`, with an idle and
  per-item hook that submits partial batches and publishes retired queries.
- `VdSwap` presents through `PostDirectSwap`.
- `GetData` answers type-9 queries from the direct result table
  (`Gow3DirectQuery*` in gears_bridge.cpp).
- The planner hands work over every `gow3_native_direct_batch_operations`
  operations (default 32), not only at query ends. Without that, query results
  lagged a whole frame.
- Packets the guest submits before it becomes GPU-less are retired as
  completed, or published as visible.
- CPU-sourced memexport copies run at the API point.
- Automated room scene (`tools/rendering/verify-native-boot.py --config
  native_direct_execution=true`, live-on-03 input sequence): median 12.5 ms,
  versus 14.8 ms for the PM4 path. Query wait dropped from 6.6 to 1.2 ms per
  frame. GPU is about 88% busy. Screenshots match.
- User combat run: heavy frames median 16.7 ms, query wait 0.25 ms. Combat is
  now bound by producer CPU work.
- Producer-thread profile in combat, excluding query spin: renderer capture,
  hashing and allocation take about 78%; game code about 16%; the guest D3D
  library about 5%. Stage 3 is therefore the next lever, not stage 4.
- Follow-up fixes:
  - Batches are also cut before they exceed 128 dependencies. Wider prefixes
    fall back to canonical storage and digest every lazy device state with
    SHA-256, which had cost about 20% of producer time.
  - Direct GetData spins without taking the result lock unless something new
    was published.
- Remaining producer work, excluding waits: about 70% renderer planning and
  capture, spread over comparisons, permission checks, hashing and hash tables.
  About 25% is game code.

## Stage 2: EDRAM to real render targets

Render most draws through the GPU's own depth test and blending into ordinary
Vulkan images. Keep FSI only for formats or blend modes the hardware cannot
reproduce exactly. Packed-to-image conversion happens only at resolves and
format reinterpretation. This targets the 4–15 µs per-draw GPU floor.

Why earlier attempts failed: every hardware-attachment path so far was hybrid.
Retained opaque, the FP24 writer, the opaque phase and early depth each moved a
subset of draws while the rest stayed on packed FSI storage. The data was
restored and exported between the two representations several times per frame.
All variants were bit-exact, and none gained (STATUS.md 130–140, 1955–2072). So
stage 2 must make host images the authoritative render-target storage for
whole passes, not for selected draws.

Combat census (saved fight capture, about 1,147 draws per frame, joined with
run-11 GPU timing):
- No MSAA and no alpha-to-coverage. Blending is standard: additive, alpha and
  multiply, and alpha is always preserved.
- 96% of draws use D24FS8 (float24) depth. 57% write k_2_10_10_10_FLOAT (7e3).
- Three raw-bit reinterpretations per frame: a 7e3 target resolved or drawn as
  2_10_10_10. Other EDRAM reuse is clear-delimited.
- Movable share by fidelity level:
  - bit-exact: about 2.4%;
  - D32F standing in for float24, with 7e3 opaque output rounded into RGBA16F:
    about 77% of draws and 74% of GPU time;
  - also blending 7e3 in RGBA16F with fixed-function blend: about 99.9%.

Design:
1. Host render-target images keyed by EDRAM base, format and extent: RGBA16F
   for 7e3 and 16F, RGBA8 for 8888, D32F_S8 for both depth formats.
2. Within a pass, draws share one render pass. Real depth, stencil and blend
   state replace the FSI epilogue, and the pixel shader quantizes 7e3 output.
3. Resolves copy or convert from host images. Depth resolves round to float24.
   Raw reinterpretations go through a host transfer shader.
4. Packed EDRAM is touched only at reinterpretation, memexport and
   capture/diagnostics.
5. Settings: hardware-render-target mode with an "exact" fallback that keeps FSI.

### Stage 2 status: default since 2026-10-21 (`gow3_native_hardware_render_targets`)

Done so far, all only in hardware mode (`native_fsi` off):
- Async unified-queue submission and draw batching are allowed.
- `PrepareCommittedStateForDraw` no longer commits working images before every
  draw, which used to drain the GPU queue about 370 times per frame.
  `Surface()` still commits an image when another interpretation touches its
  tiles. Counters: `surface_conversions` (about 0.1 commits and 3 restores per
  frame in the room scene).
- One cached framebuffer per (color, depth) pair instead of one per draw.
- Render-pass continuation (default since 2026-10-22,
  `gow3_native_hardware_render_pass_continuation`): grouped draws into the
  same framebuffer keep one render pass open while both attachments stay in
  their attachment layouts. A first attempt (2026-10-18) hung the RX 9070 XT
  gfx ring twice. The likely cause was the stale framebuffer cache keyed by
  recycled image pointers, fixed 2026-10-20 (not proven: the kernel logs were
  gone). The re-enabled version adds the layout guard and passed a full
  seeded pan under Vulkan validation with sync validation and break-on-error
  (1,038,862 continued / 313,525 started passes, no errors). Seeded pan on the
  RX 9070 XT, two runs each: frames under 500 operations 3.05/3.04 →
  2.74/2.73 ms, 500–1000 6.30/5.98 → 5.10/4.95 ms, 1500–2500 13.56/14.06 →
  12.97/12.94 ms, 2500+ 19.66/19.75 → 18.94/19.02 ms. If a GPU hang returns,
  set the option to `false` first.
- Async planner, submission worker and four in-flight batches are the
  default (2026-10-22). Once render-thread capture was cheaper, the frame was
  bound by the GPU Commands thread and the occlusion-query round trip. That
  thread spent about 13% of its wall time in the kernel submit ioctl and 10%
  waiting on fences with only two batches in flight. (Cycle profiles with
  `cycles:u` miss both; `tools/rendering/bench/wall_stacks.sh` samples wall
  time.) All three together took the Tickers fight from 10.2 to 8.5 ms. Each
  alone was within noise.
- Room scene renders correctly.
- Clean A/B (RX 9070 XT, 2026-10-18): default 13.30 ms median, p95 14.7;
  hardware mode 13.39 ms, p95 24.3. No gain yet.

Note: the SDK used to pick the first Vulkan device, which became the iGPU after
a reboot. It now prefers discrete GPUs (vulkan_provider.cpp, fsi_device.cpp). Benchmark: `tools/rendering/bench/auto.sh OUT_DIR --config
gow3_native_hardware_render_targets=true`, compared with no `--config`.

Update (2026-10-20): hardware mode is now faster than FSI. Seeded pan
benchmark (user caches, RX 9070 XT): frames with 2500+ operations 17.5 /
18.3 ms against 19.2 / 19.3 ms for FSI; frames under 500 operations 2.8 ms
against 3.9–4.9 ms. Per-draw GPU cost is about half of FSI. The earlier loss
was latency, not GPU work, fixed in three steps:
- `Surface()` memoizes tile masks, packed surface versions and typed surface
  keys (it was half of the GPU Commands thread).
- Operation boundaries (query controls, clears, resolves, memexport, CPU
  writes) no longer commit host images to packed EDRAM in hardware mode;
  every packed reader commits overlapping images itself. This removed about
  600 GPU drains per heavy frame.
- Alias commits/restores inside draws run in a transfer scope, so they queue
  in order instead of submitting synchronously and draining the GPU.
Core and synchronization validation are clean for the renderer.

Update (2026-10-21): hardware mode is the default. Async shaders and async
pipelines now work in it (pipeline key built from decoded state: about 450
pipelines instead of 2614), retired images drop their framebuffers (fixed the
Alt+F4 hang), and color alias restores decode directly into the target. Exact
FSI stays available with `gow3_native_hardware_render_targets = false`.

Still to do: 7e3 output quantization in the pixel shader (needs shader
generation changes), per-fragment float24 depth rounding, and raw
reinterpretation transfers.

## Stage 3: native resource model

Identify textures, vertex buffers and index buffers by their guest D3D objects
(CreateTexture `0x82AEF008`, surfaces `0x82AEF128`, SetTexture `0x82AEF370`,
SetStreamSource `0x82AECAF0`, SetIndices `0x82AECC08`, Lock/Unlock `0x82AE9140`).
Track dirtiness from Lock/Unlock instead of hashing and snapshotting guest memory
on every draw. This removes most of the per-draw capture CPU cost.

## Stage 4: skip PM4 generation

Once nothing consumes PM4, replace the guest draw functions (`0x82AEFF00`,
`0x82AF0318`, `0x82AEF500`, `0x82AEF9C0`) and the draw-time flush helpers
(`0x82AF7CA8`, `0x82AF7578`, `0x82AF7910`, `0x82AF7B50`) with native
implementations. They read the device state directly and clear its dirty masks,
so the game stops building command buffers at all.

## Reference: guest D3D facts

- D3D library is at about `0x82AE8000–0x82B04800`. CreateDevice is `0x82AF9CD8`
  (0x5F80-byte device). The device pointer is r3 of every API call.
- Device layout: dirty masks at 0x00–0x20; PM4 cursor and limit at 0x30–0x38;
  fetch constants at 0x480; ALU constants at 0x780 (VS) and 0x1780 (PS); the
  register shadow from 0x2880 to 0x2A5F; completion block pointer at 0x2B10;
  next fence at 0x2B1C; render targets at 0x3190–0x31A0; shaders at
  0x328C/0x3290.
- Guest waits all poll the completion block through the spin helper
  `sub_82AF98D8`. Ring-space waits (`sub_82AF4E28`) only happen in the non-direct
  branch.
- Interrupt callback: `sub_82AF5D00`. Source 0 is vblank, delivered by the SDK
  VSync thread. Source 1 is PM4 INTERRUPT.

## FSI draw pass groups (2026-10-18)

Measurements in the default mode showed the frame was GPU-bound through occlusion-query sync:
- the GPU was about 85% busy;
- each draw cost about 7 µs of fixed overhead, because almost every FSI draw got its own render pass plus an EDRAM barrier.

`gow3_native_fsi_draw_pass_groups` (src/renderer/native/fsi_draw_pass_groups.h) keeps consecutive FSI draws in one ordered-interlock subpass, without barriers, as long as every EDRAM word maps to one pixel only:
- same pitch;
- each color and depth plane either identical to a pass plane (same base, kind and width) or disjoint from all of them, counted in tiles modulo 2048.

Room scene on the RX 9070 XT:

| | Median | p95 | Query wait |
|---|---|---|---|
| Before | 12.58 ms | 13.61 ms | 5.0 ms per frame |
| After | 9.92 ms | 10.99 ms | 2.3 ms per frame |

- About 75% of eligible draws continue a pass.
- The image matches the normal render.
- The producer thread is at 99.5% again and the GPU at 82%, so CPU capture work (stage 3) is the next lever.

Validation:
- Unit test for the plane rule.
- Integrated-GPU replay: OFF vs ON differences are within the iGPU's own run-to-run noise.
- Core Vulkan validation is clean.
- Sync validation cannot see these EDRAM accesses, because they go through buffer device addresses.

Follow-up: admitting occlusion-query draws to these groups (`gow3_native_fsi_draw_pass_queries`) was exact offline, but slower live: 9.87 → 10.28 ms. Holding groups open across query ends delays results that the game thread is waiting for. The option stays off.

## Blocking GetData retries (2026-10-18)

The guest's occlusion loop (sub_82961960) called the GetData hook at full speed while waiting, which kept a core at 100% and competed with the executor thread. Now a repeated GetData for a pending query waits on a condition variable until the next publication, bounded by `gow3_native_direct_getdata_wait_us`. The default is 2000; 0 restores spinning.

Room scene:

| Wait bound | Median |
|---|---|
| 0 (spin) | 10.02 ms |
| 500 µs | 9.15 / 9.33 ms |
| 2000 µs | 8.84 / 9.36 ms |
| 5000 µs | 8.83 ms |

GPU busy rose from 81% to 87%, and query latency fell from about 3.3 to 2.5 ms.
