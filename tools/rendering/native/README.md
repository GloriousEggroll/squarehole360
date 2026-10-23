# Gears renderer development

The game defaults to the native renderer with explicit compatibility fallback.
The config and both settings interfaces select `gow3_renderer = "native"`, shown
as **Native (with compatibility fallback)**. Changing the selector requires a restart. Use
`gow3_renderer = "reference"` for compatibility rendering. Native ownership
requires the Vulkan host, 1× internal resolution, fragment-interlock storage and
enabled occlusion queries; startup checks retain compatibility rendering when
these requirements are unavailable.

The native path executes matched draw/clear/resolve work, publishes native query
results and supplies completed images to the existing window/HDR presenter.
It retains the SDK's command ordering, guest-memory waits and fence handling.
Unsupported work detected before its writes triggers a fenced state transfer
and one-way transition to compatibility rendering for the rest of that run.
GPU failures or errors after an incomplete write cannot safely replay the
operation and are treated as fatal. This is not a complete source decompilation
or a renderer covering every possible game state.

Latest typed-draw/diagnostics check (2026-10-02): with identical native and Mesa
cache seeds, stationary windows improved from 36.695 to 30.1 ms (about 33 FPS),
and movement from 37.08 to 29.65 ms. Two selected movement frames exceeded 50 ms,
versus 81 before; none exceeded 100 ms. The candidate completed 10,414 native
frames and all 1,299,595 query publications, closed cleanly and had no fallback/fatal error.
Neither run had a slow shader/pipeline creation. Fixed-time movement still
produces different camera paths; these results do not establish campaign-wide
performance or 60 FPS. `movement-perf/latest-comparison.json` records selection
and limits. Typed records own canonical resources and avoid JSON on the draw
path; complete exports remain exact, paired typed/JSON GPU samples and lifetimes
pass, and all 172 existing replay images remain byte-identical through the full
typed live path. Routine diagnostic counters and frame statistics now serialize
only on observation. This cleanup measured 30.1/29.65 ms versus 30.69/28.64 ms for
typed draws alone, so it does not establish a separate movement FPS gain.

Earlier cold-seed movement check: moving texture conversion off CPU
reads from mapped upload memory improved selected movement windows from 80.68 ms
to 35.86 ms in the first candidate. The latest flat-prefix candidate measured
37.285 ms during movement (about 27 FPS), and 39.46 ms before movement. It completed
7,209 native frames and all 805,862 query publications, closing through the UI
without fallback or fatal errors. A first-use hitch still reached 1.40 seconds.
60 FPS and stable frame times remain unmet. These use medians of complete
120-frame window medians at 1280×720; the same fixed-time inputs can produce
different camera paths when frame rates change. They do not establish
whole-campaign performance. Evidence: `reports/rendering/native-renderer/movement-perf/`.

For a full typed-path correctness check, build `gow3-native-live-capture-replay`.
It accepts a version-six API capture, the original inspected scene directory,
compiled shader directory and a new output directory. It reconstructs the real
live plan and executes its typed draws; `--json-adapter` executes an explicit
JSON export of that same plan. `--exact-live-oracle=PATH` verifies the complete
unnormalized plan and resource table against an independent JSONL export before
GPU execution. Both modes keep captured provenance and original GPU history.
The weapon capture passed 3,330 draws and all 172 output image comparisons in
both modes and against the earlier offline renderer. Commands and evidence are
in `reports/rendering/native-renderer/typed-live-draw-phase-a/`.

An earlier executable with both normal native and Mesa caches measured 40.28 ms
during movement and 167.52 ms maximum (one selected frame above 100 ms). It
completed 8,387 native frames and all 981,160 queries with no fallback/fatal error.
Pipeline creation for 358 entries totaled 118.72 ms, versus 12,972.03 ms when
only the native cache was populated. This is cache reuse, not a code-speedup
claim. Diagnostic runs use a fresh driver cache unless `--mesa-cache-source`
explicitly seeds it; `--preserve-mesa-cache` retains its result for comparisons.
Normal game launches already use the driver's persistent cache. Keep the two
cache layers distinct when interpreting first-use stalls.

Pipeline/shader objects and immutable CPU index buffers have bounded persistent
caches. Normal gameplay omits informational per-operation JSON histories; use
`gow3_native_detailed_reports = true` for those diagnostics. Extra dormant texture
residency is enabled; `gow3_native_retain_unused_vertices` remains false because
the tested extra vertex cache had weak reuse. Small immutable command prefixes
use flat resource tables, preserving exact owners and diagnostic export order
with fewer allocations. This last allocation change has not shown an additional
FPS gain. Source translation and driver pipeline creation have separate miss
timers; existing memory watches still do not justify skipping exact texture checks.

A same-binary comparison measured 47.435 ms with one submission versus 42.35 ms
with two in flight; host fence waits fell from 15.405 to 9.557 ms per frame.
Both used the same checkpoint, input sequence and shader-cache seed. This supports
`gow3_native_async_gpu_submissions = true` as the default. The current build also
retains old texture generations until their exact fences, avoids redundant
state hashing/copying and cache-use sets, and serializes diagnostic counters only
on observation/frame completion. All 172 firing replay images remain unchanged.
Evidence: `reports/rendering/native-renderer/lazy-diagnostics/validation.json`
and `cache-epochs/submission-comparison.json`.

Optional `gow3_native_gpu_timestamps` diagnostics remain off by default. They
separate host submit/wait/retirement times from inclusive GPU batch intervals,
which may overlap and must not be added as exclusive busy time. They add no fence
waits. Full texture-byte coherence comparisons remain required; existing memory
write-watch epochs do not cover all raw physical writers.

Earlier live check: campaign run 09, with accelerated SHA-256 and
shared immutable operation metadata, reached the saved gameplay checkpoint,
accepted movement and closed normally with exit status 0. It completed 2,778
native frames, 989,135 owned packets and 173,180 native query publications
without fallback. It used the same 703 shader-cache entries as runs 06–08;
the file manifest checksum is identical. The live comparison below selects
stationary windows using recorded inputs and scene screenshots.

Earlier hardware startup run 05 completed 713 native frames, 15,402 raster/copy
packets and 24 query publications without fallback. Deliberate query handbacks
verified native-positive/reference-zero and native-zero/reference-positive
results, including the actual guest report and cleared wait sentinel. These
checks do not establish whole-campaign coverage. CPU resource handling,
remaining synchronous transfers and first-use shader work still need tuning.

Before draw batching, campaign run 05's warm checkpoint samples were 74–100 ms
per frame after eliminating repeated surface-footprint sorting and reusing
descriptors, uploads and texture bindings. That run was roughly 10–13 FPS;
these measurements are a prior baseline, not a result for the batched build.
The native default does not imply 60 FPS readiness.
First-use shader compilation still causes long pauses. Existing validated local
shader cache entries were seeded into this installation's normal cache; every
entry remains version/source/mode/checksum validated at load.

Capture 11 also executes both complete offscreen frames with verified initial
GPU state: 1,523 draws, 36 clears and 62 resolves. The repaired reference replay
and native output now agree closely after resolve channel-swap and mixed-mip
ownership fixes; the image metrics and reference repair provenance are recorded
below. Shader, layout and pixel comparisons remain distinct from live coverage.

Implemented and tested components:

- Version-6 D3D boundary capture includes device state, shaders, geometry,
  CPU texture snapshots, clear/resolve parameters and Present resource headers.
  GPU snapshots are separate and carry the same explicit API-frame marker.
  Imported history accepts only the initial GPU download portion of a trace;
  later CPU snapshots cannot seed prior history. SHA-256 and frame checks apply.
- The portable texture decoder covers tiled/linear addressing, packed mip tails,
  2D/3D/cube layouts and four endian modes. GPU storage retains partial writes
  across different base addresses, formats and pitches. Exact byte-range alias
  classification avoids confusing adjacent CPU assets and padding with resolves.
- A 10 MiB EDRAM model tracks overlapping tile interpretations, wraparound,
  depth half-tiles and stencil bytes. Fixed16 and 7e3 conversions have targeted
  rounding/NaN tests. Fixed-point source values are clamped and quantized before
  resolve scaling. This does not establish exact per-fragment blend/depth behavior.
- `SceneExecutor` accepts in-memory operation/payload/shader providers and can
  borrow a device/queue. Its file-backed CLI uses the same execution path.
  Previous-frame resources persist. Native FSI draws record into a bounded pool
  of up to 64 immutable draw slots and share a submission fence. Dependent
  operations and command-processor completion boundaries drain the batch.
  The fixed-function diagnostic path retains one draw per fence.
- Native shader compilation uses XenosRecomp and DXC in process, with a cache
  identity covering compiler sources, the ABI and DXC library. All 189 capture-11
  shaders match the standalone SPIR-V byte for byte; malformed inputs, cache
  corruption and concurrent requests have dedicated checks.
- The borrowed Vulkan device adapter serializes submission through the host queue
  mutex and retains host ownership. Tests verify image/buffer transfers, multiple
  semaphore waits and owner-device survival. The game's existing gamma/HDR and
  platform presenter can accept a same-device native image with a lifetime lease.
  Full ownership is selected with `--gow3_renderer=native`. Diagnostic shadow
  modes explicitly set `--gow3_renderer=reference`, then add
  `--gow3_native_live=true` (reference presentation) or
  `--gow3_native_present=true` (native preview). Both diagnostic flags default to
  false. Reference drawing remains enabled in those shadow modes.
- Native occlusion queries count actual visible/hidden fragments in GPU tests.
  Frame-tagged comparison handles missing/stale results and repeated report
  addresses in shadow mode. Full ownership publishes actual native samples at
  generation-matched command-processor query ends, before guest waits consume
  them. Mid-query fallback preserves the native subtotal and adds a completed
  reference residual segment; zero samples remain zero.
- The production fragment-interlock helper passes 26 GPU cases and 14,080 exact
  packed-word checks, including ordered fixed16/7e3 blending, depth24, stencil,
  polygon offsets, constant color/alpha blending and rejection from occlusion queries. Translated shader
  variants compile, and both captured gameplay frames execute with this path.
  A built-in depth-only shader handles null guest pixel shaders. Exact image
  equivalence and broad live scene coverage remain under validation.
  After the frame completion fence, the executor retires unused uploads,
  views, surfaces and pipelines while retaining physical GPU history.
- A recognized startup shader-memory-export program is replaced by a native
  buffer copy after complete shader/state/range validation. The real 8,192-byte
  input passes an independent scalar endian interpretation and GPU copy check.
  Other memory-export programs still require explicit support.
- Physical write notifications invalidate native ownership on guest CPU writes
  or frees. Readable updates are snapshotted at the next API boundary; callbacks
  perform no GPU work. Binding ownership is recorded in draw order. SDK raw
  physical readbacks bypass these callbacks, so they are not mistaken for CPU
  asset uploads. Native memory-copy operations publish GPU and CPU-visible
  destination bytes at their matched packet boundaries. State handback excludes
  bytes superseded by a newer guest CPU write, using captured page epochs.
- Mixed texture uploads retain CPU snapshots and overlay exactly defined GPU
  bytes, including separately owned mip regions. A GPU-only binding with missing
  required bytes rejects the draw. A capture-based fault injection removes
  one texel before event 323: the original history passes 297 draws, while the
  altered history stops before draw 297 with an explicit partial-input error.
  Evidence: `captures/campaign-11/partial-history-regression/result.json` under
  the native-renderer report directory. Separate ownership tests cover partial
  CPU writes, mip-page reuse, padding and sub-word endian footprints.

Current focused checks: 49 shader GPU groups, 523,264 storage component/byte
comparisons, ten invalid storage/transfer rejections, 22 capture parser tests,
13 scene-plan tests, six history-import tests and 18 asset-import tests. Earlier
CPU texture/depth checks covered 5,760 layouts and 35,171,904 comparisons.
Tests use synthetic fixtures where possible; real-game cases require the user's
own captures. Shader compilation is not proof of rendering equivalence.

The consumer code uses portable C++/Vulkan. Plume source is unchanged. The main
build shares the SDK's Vulkan loader and allocator implementations through
consumer header shims. Windows compilation/runtime still need validation.

## Reproduce

From the project root:

```bash
python3 tools/import-assets.py import "/path/to/gow3.iso"
cmake -S vendor/xenos-recomp -B build-xenos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DXENOS_RECOMP_GOW3=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
cmake --build build-xenos --target XenosRecomp -j 2
python3 tools/import-assets.py prepare --compile-shaders
cmake -S tools/rendering/native -B build-native-renderer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-native-renderer -j 2
build-native-renderer/gow3-native-shader-test \
  game/.gow3-assets/native/shaders/compiled/0a94610849f6cdebfbfa41fb7c840852e2562bd1f39e1dedddb2275e4cfa0b63.spv
python3 tests/renderer_capture_test.py
python3 tools/rendering/make-shader-fixtures.py \
  reports/rendering/native-renderer/semantic-fixtures
python3 tools/rendering/compile-gears-shaders.py \
  reports/rendering/native-renderer/semantic-fixtures/manifest.json \
  reports/rendering/native-renderer/compiled-fixtures --jobs 2
python3 tools/rendering/verify-native-shaders.py
build-native-renderer/gow3-native-pass-test
python3 tools/rendering/verify-fsi-precision.py
```

Known shaders extracted from the user's assets can be compiled into the native
runtime cache ahead of launch, without starting the game or using the GPU:

```bash
cmake --build build-native-renderer --target gow3-native-shader-prewarm -j 2
build-native-renderer/gow3-native-shader-prewarm \
  --cache user/cache/native-shaders \
  game/.gow3-assets/native/shaders/source/manifest.json
```

Pass additional capture manifests to include shaders encountered during play.
The tool deduplicates their source hashes, validates stages and input bytes,
and compiles raster variants, pixel interlock variants and the built-in
depth-only shader. It defaults to the machine's available hardware threads,
bounded by the number of unique sources; `--jobs N` overrides that limit.
Progress goes to stderr and a compact JSON summary goes to stdout. The full
result is saved to `<cache>/<compiler-version>/prewarm-report.json`, including
explicit failures and verified persisted-cache hits. Any failed variant
produces a nonzero exit.
The compiler version selects a new cache directory automatically when shader
translation changes. `--spirv-output DIR` additionally writes the `.spv`,
`.fsi.spv` and `depth-only.fsi.spv` files used by offline replay. Input assets
are preserved and output paths are checked before compilation.

This prepares only the supplied shader containers. It does not discover every
shader in the game or prebuild Vulkan graphics pipelines. The CPU regression
runner is `python3 tools/rendering/native/shader_prewarm_test.py
build-native-renderer/gow3-native-shader-prewarm
game/.gow3-assets/native/shaders/source/manifest.json`. The local 625-source
manifest produced all 1,071 variants in 14.59 seconds using 16 workers, with
zero failures; see `reports/rendering/native-renderer/shader-prewarm-report.json`.
This is shader compilation time, not a gameplay performance measurement.

The FSI check requires `VK_EXT_fragment_shader_interlock` and creates a device
with ordered pixel interlock enabled. It validates GLSL and DXC implementations
against exact packed EDRAM results, then exercises the production constant and
descriptor ABI plus late occlusion rejection. The default shader variant keeps
its original ABI and does not require interlock. The FSI variant uses descriptor
set 4, binding 0 and appends 128 bytes after the existing shared constants.
`gow3-native-scene-replay --native-fsi` loads `<hash>.fsi.spv` pixel variants.
MSAA, gamma render targets, extended-range half-float behavior, and complete
hardware early-depth ordering remain outside the current validation coverage.

For selected CPU texture inputs, enable both startup diagnostics
`gow3_native_texture_probe_directory` (a new output directory) and
`gow3_native_texture_probe_hashes` (up to 32 comma-separated lowercase base
payload SHA-256 hashes). Offline replay accepts the equivalent
`--texture-probe-dir=DIR --texture-probe-hashes=HASH[,HASH...]` options.
The probe saves exact base/mip bytes after GPU-history overlay and before
decoding, together with fetch state, frame/event, version, source IDs and output
hashes. It adds no GPU readback, deduplicates unchanged inputs, and stops only
the diagnostic after 128 records, 256 MiB, or an I/O failure. These files contain
the user's game assets and should remain local.

Compare committed probes on the CPU with:

```bash
cmake --build build-native-renderer --target gow3-native-texture-probe-compare gow3-native-texture-probe-test -j 2
build-native-renderer/gow3-native-texture-probe-compare \
  reports/probe-comparison.json reports/probe-before reports/probe-after
build-native-renderer/gow3-native-texture-probe-test
python3 tools/rendering/native/texture_probe_compare_test.py \
  build-native-renderer/gow3-native-texture-probe-compare
```

The report path must be new. The comparator verifies payload sizes and hashes,
removes tiling and byte swapping from consumed format blocks, and distinguishes allocation
padding changes from changed blocks at each mip/layer. Exact groups retain fetch
state except allocation addresses. A separate, explicitly relaxed comparison
uses each asset's latest record and matches levels by format and dimensions,
allowing investigation across streaming sampler changes. Different frames and
sampling states do not establish rendering equivalence.

Use `--adopt-existing` for the first import of an existing `game/` extraction.
The importer records provenance and generates these shaders from each user's
own ISO. Do not ship extracted/translated shaders or captures with the runtime.
See [asset preparation](../../assets/README.md) for layout and limitations.

Use a new capture and trigger filename for each diagnostic. Existing captures
and triggers are rejected. With `--gow3_renderer_capture_trigger=<path>`, load a
checkpoint first and then create that file to begin at the next frame boundary.
The trigger is not deleted automatically. Capture flags do not modify the
normal settings file. Reference GPU-history capture must explicitly select
the reference renderer, as this example does:

```bash
./tools/run-linux.sh \
  --gow3_renderer=reference \
  --gow3_renderer_capture=reports/rendering/native-renderer/captures/example.g3d \
  --gow3_renderer_capture_trigger=reports/rendering/native-renderer/captures/example.trigger \
  --gow3_renderer_history=reports/rendering/native-renderer/captures/example-history \
  --gow3_renderer_capture_frames=2
# In another terminal, once gameplay is visible:
touch reports/rendering/native-renderer/captures/example.trigger
python3 tools/rendering/inspect-gears-capture.py \
  reports/rendering/native-renderer/captures/example.g3d \
  reports/rendering/native-renderer/captures/example
```

Inspection writes a compilation manifest and uniquely named payloads. To
reproduce the captured character preview:

```bash
python3 tools/rendering/export-mesh-replay.py \
  reports/rendering/native-renderer/captures/campaign-06 \
  reports/rendering/native-renderer/captures/campaign-06/mesh-1030.g3m --event 1030
build-native-renderer/gow3-native-mesh-replay \
  reports/rendering/native-renderer/captures/campaign-06/mesh-1030.g3m \
  reports/rendering/native-renderer/compiled-campaign-06/c4f02e79785bbc0b0cad1e661a86bf38755f929ebbc1b6b2ec20479ce26947af.spv \
  game/.gow3-assets/native/shaders/compiled/0a94610849f6cdebfbfa41fb7c840852e2562bd1f39e1dedddb2275e4cfa0b63.spv \
  reports/rendering/native-renderer/captures/campaign-06/mesh-1030.ppm
```

To replay the captured material with raster state:

```bash
python3 tools/rendering/export-mesh-replay.py \
  reports/rendering/native-renderer/captures/campaign-07 \
  reports/rendering/native-renderer/captures/campaign-07/material-depth-1297.g3m \
  --event 1297 --material
build-native-renderer/gow3-native-mesh-replay \
  reports/rendering/native-renderer/captures/campaign-07/material-depth-1297.g3m \
  reports/rendering/native-renderer/compiled-campaign-07/c4f02e79785bbc0b0cad1e661a86bf38755f929ebbc1b6b2ec20479ce26947af.spv \
  reports/rendering/native-renderer/compiled-campaign-07/2852b1b3aa1faae6e25f03ed8af0697e96c7079879d7d51a328f47e8d0495eb8.spv \
  reports/rendering/native-renderer/captures/campaign-07/material-depth-1297.ppm
python3 tools/rendering/build-scene-plan.py \
  reports/rendering/native-renderer/captures/campaign-09 \
  reports/rendering/native-renderer/captures/campaign-09/scene.json
python3 tests/scene_plan_test.py
cmake --build build --target gow3-texture-layout-test
build/bin/gow3-texture-layout-test
build-native-renderer/gow3-native-scene-replay \
  reports/rendering/native-renderer/captures/campaign-09 \
  reports/rendering/native-renderer/compiled-campaign-09 \
  reports/rendering/native-renderer/captures/campaign-09/native-scene-prefix 2264
```

The final optional scene-replay argument is the last event to execute. Omit it
to attempt the whole frame; capture 09 then reports the missing history at event
2267 and exits nonzero. `replay.json` records the stop, completed operations,
image statistics and limitations. Resolve previews are PPM; depth previews are
normalized for visibility. Normal gameplay remains on the working backend.

The capture magic remains `G3D3D001`; the version word is now 6. A block length
with bit 31 set refers to an earlier inline block's data offset, never a forward
reference. Blocks 14–29 hold physical vertex streams 95–80, block 30 the index
resource header and block 31 the selected indexed range. Blocks 32–95 hold
base/mip CPU texture data for slots 0–31. Blocks 96–97 hold finalized UP vertex
and index allocations. Blocks 98–102 hold source rectangle, clear color,
resolve destination resource, destination XY and floating/stack arguments.
Block 103 contains the Present frontbuffer resource header and fetch constants.
Phase 1 is after a guest call; phase 2 is after the inline UP commit. Successful
allocation alone never makes an UP buffer replayable.

`G3MESH01`–`G3MESH03` previews remain readable. New material exports use
`G3MESH04`, extending the register window through `0x2a5f` to include front/back
polygon offsets. Older raster previews with offset enabled must be re-exported.
Unsupported state is rejected.

For current frame-equivalence diagnostics, the history option records adjacent
GPU boundaries N and N+1. Import N with `import-gpu-history.py` as initial state,
then import N+1 with `--reference-frame N`. The second import writes a separate
`gpu-reference-N.json` and never replaces the initial history. Scene replay can
then compare its frame N with the reference's completed frame N in the same
pre-presentation image space. An image from boundary N is the preceding frame
and must not be treated as the matching reference.

`replay-reference.py trace.xtr new-output-directory` exports the trace viewer's
guest image in a private compositor. This is a Linux-only diagnostic wrapper;
the viewer export uses portable C++. Trace playback fidelity must itself be
verified before using this export as an image-correctness oracle. Its guest
image includes presenter gamma conversion, unlike the raw physical snapshots.

For a raw comparison, add `--export-state` to the reference replay command.
The viewer writes a new `gpu-state/state.xtr` after GPU completion, containing
the replayed physical memory and EDRAM, plus explicit diagnostic provenance.
The wrapper records the trace, snapshot, and viewer executable hashes. This is
reconstructed replay state, not an original captured frame. Never use it to
replace `gpu-history.json` or claim original-capture equivalence.

`compare-replayed-state.py capture native-output reference-output report.json
event...` compares selected native `--debug-raw-images` resolves with those
physical bytes, before fetch swizzles and presentation gamma. It verifies the
frame and snapshot hash, rejects missing bytes and later physical overwrites,
and supports the color formats used by the current scene diagnostics.

To isolate a consumer from earlier producer differences, use
`export-replayed-state.py reference-output new-state-directory`, then run scene
replay with `--diagnostic-state=new-state-directory/state.json --first-event=N`.
The replay report marks this continuation as invalid for full-scene equivalence.
Only select inputs whose lifetimes cover that point; the state represents the
replay endpoint, so overwritten transient resources cannot reconstruct an
earlier pass. This workflow isolated the final stacked color-LUT sample from
lighting and DOF producers and exposed incorrect layer interpolation in the
reference SPIR-V translator.

CPU texture uploads combine CPU bytes with any newer GPU-owned sampled blocks,
including a CPU base image with GPU-owned packed mip levels. The overlay excludes
allocation padding, preserves sub-word CPU invalidations, and does not publish
CPU snapshots into physical GPU storage. Cache versions cover all sampled base
and mip pages. `gow3-native-texture-overlay-test` verifies these ownership rules
on Vulkan, including BC3 mip tails and endian-swapped one-byte textures.

This corrected the capture-11 hard particles and lighting mismatch: the particle
base atlas was correct, but its CPU mip snapshot contained stale data while the
GPU history held the current mip chain. The complete two-frame replay is in
`captures/campaign-11/native-mixed-mip-history`. Against the explicitly diagnostic
repaired reference, final raw RGB RMS fell from 0.028240 to 0.0009476; 99.9871% of
the 1280×720 pixels differ by at most one 8-bit step in every RGB channel. The
scene-color resolve RMS is 0.0003298 and the 8,192-pixel color LUT matches exactly.
The reference is a replay with recorded repair provenance, not an untouched
original-frame snapshot. Small edge and motion-buffer differences remain; these
measurements do not establish hardware-exact rendering for all scenes.

## Remaining integration gates

1. Expand exact-frame image comparisons beyond the repaired capture-11 pair,
   including depth-of-field/motion blur and more lighting/material states.
2. Expand native campaign coverage beyond the saved-checkpoint test. Exercise
   a query handback with nonzero native and
   reference contributions in the same query. Extend shader memory exports
   beyond the known copy intrinsic as encountered. All query, fence and readback
   publication must continue to preserve packet order.
3. Extend precision coverage to multisampling, gamma targets, extended half
   formats and early-depth ordering. Per-fragment fixed/7e3 blending, 24-bit
   depth/stencil, masks and constant blending have focused GPU validation;
   broader game-state coverage remains necessary.
4. Validate resource lifetimes, bounded frame queues, native presentation,
   Wayland/HDR, overlay, resize and fallback in the actual game. Bounded draw
   batching has exact replay, focused GPU checks and saved-checkpoint live
   validation. Broader display/fallback scenarios, pipeline prewarming and
   further performance changes still need their own validation.
5. Integrate FSR 3.1 after scene color, verified motion vectors, depth, jitter and
   history reset are available. Normal gameplay currently uses FSR 1.

## Early submission and reference independence

The shadow preview detaches immutable operation prefixes at API query ends and
executes them at the matching CP `EVENT_WRITE_ZPD` end. Remaining frame operations
execute from `VulkanCommandProcessor::IssueSwap`, after the reference submission.
Reference drawing and guest query publication remain enabled in shadow mode.
The ownership path now captures exact emitted spans and can replace verified
memory-copy packets, publishing both GPU and CPU-visible bytes before later
fences. Private ownership run 02 replaced three startup copies (20,480 bytes),
then continued coherently on reference rendering. Full native raster/query
authority has separate integration checks; do not infer it from shadow images.

Both modes must preserve packet order, not just API call order:

* `CommandProcessor::CallInThread` drains callbacks at the outer worker loop.
  `ExecutePacketType3_WAIT_REG_MEM` does not drain that queue while polling.
  Queuing native work there and waiting on it can deadlock behind a guest wait
  that needs the native result. Native completion must be connected to ordered
  packet callbacks on the command-processor thread.
* Query issue function `0x82AFA458` calls emitter `0x82AFF740` with record 1 for
  begin and record 0 for end. Its end path can first emit `MEM_WRITE` packets
  setting ZPass words to -275, followed by `WAIT_REG_MEM` for those sentinel
  values. A native result written from the CPU emitter hook would be overwritten
  by the later packet execution. Publish only at the corresponding CP
  `EVENT_WRITE_ZPD` end, after the preceding sentinel packets have executed.
  Preserve the guest GetData implementation at `0x82AFA708` and its report ABI.
* DrawPrimitive `0x82AEFF00` and DrawIndexedPrimitive `0x82AF0318` split large
  counts into chunks of at most 65,535 indices, adjusted for topology. Their
  per-chunk command-pointer commits are `0x82AF02E8` and `0x82AF0760`.
  Between chunks they may call flush function `0x82AF58B8`, which can wait for a
  prior GPU fence. A CP callback must not block for the current after-call
  snapshot: the CPU may need CP progress before it can finish that call.
  Capture immutable chunk metadata at the commit sites, before a later flush
  can expose the commands. Existing inline UP commit hooks show the required
  instruction-validated mechanism; they do not cover regular split draws.
  At the primitive commit, r31 is the device, r25 the topology, r23 the current
  start vertex, r29 the emitted count and r11 the command end. The corresponding
  indexed values are r31, r18, r21, r27 and r11. These instruction-validated
  commit hooks now supply the packet ownership mapping; allocator/reset stores
  are excluded.
* Memory-copy helper `0x82968948` brackets its point draw with `0x82AE9AB0` and
  `0x82AE9BD8`. The latter records resource fences and emits completion/coherence
  packets. Native copied bytes must be available to guest reads before those
  fences become visible. Execute the matching draw prefix, await its native
  fence, and publish its output at CP packet order, before the subsequent
  `EVENT_WRITE_SHD`/coherence completion. The full-image preview at swap is too
  late for this contract.

The incremental shadow implementation retains each prefix's payload/shader maps
independently of the next CPU frame. A CP ZPD callback matches `{report address,
use generation}` and consumes only the corresponding prefix. It submits
preceding reference commands with `EndSubmission(false)` before native queue
work and resumes reference commands with `BeginSubmission(true)` afterward.
The callback verifies the native result's frame, address, generation and end
event before returning to packet processing. Native queue fences cover prior
same-queue work. Guest publication remains reference-owned. Prefix enqueue never
waits for the GPU, since the guest may not yet have submitted its command buffer;
backpressure applies at Present. The planner bounds unique immutable bytes per
frame and the queue additionally bounds prefix metadata. Synthetic tests cover
ordered ranges, repeated reports across frames and snapshot ownership after CPU
overwrites and planner destruction; live packet-order validation is separate.

The 70-second private `native-boot-08` run exercised this path with the production
graphics baseline and reference async compilation disabled. Its log records
generations 1–3 completing 39/3/3 operations in API frame 407 at the query
callbacks, before that frame's 527-operation Present completes. Four logged
non-reused query comparisons matched exactly, with no visibility mismatches or
fallback. See `reports/rendering/native-renderer/native-boot-08/result.json` and
`game.log`. This validates the observed startup path, not full campaign coverage
or independent guest publication.

The native raster path uses a generation-tagged mapping from emitted command
spans to native operations. Capture the command
address, extent, identity/hash and API sequence before publication; validate the
matched packet and state when CP reaches it. Audit clear/resolve internal draws
and driver flush draws separately. A single API call may emit several packets,
so skipping the next draw for each API event is insufficient. Retain register
writes, conditionals, memory writes, waits, interrupts and fence bookkeeping.
Preflight support before committing any replacement; an unknown packet or state
must not be silently skipped.

After a reference draw has been suppressed, the reference EDRAM and physical
GPU memory are stale. Safe runtime fallback then needs explicit native-to-
reference state handback. Simply clearing the presentation callback would expose
stale reference state. The ownership integration now has draw-packet generation
mapping, exact GPU byte-range transfer and fenced active-query subtotal export. The
transfer primitive passes 32,768 byte comparisons; a separate executor test
verifies an actual full 10 MiB EDRAM import and partial physical ownership.
Native raster run 03 verified state handback after a real native clear and clean
UI shutdown. Hardware run 04-display verified visible movie rendering and the
reference title after handback. Hardware run 05 completed 713 native movie/title
frames, 15,402 raster/copy packets and 24 native query publications without
fallback, then closed cleanly. Deliberate handback tests now cover a completed
native draw followed by an empty reference segment, and a handback before the
first native query draw followed by 230,400 reference samples. Both publish
230,400 actual guest samples and clear the guest wait sentinel. No single tested
query yet has both nonzero native and nonzero reference contributions. Native
campaign run 04 subsequently passed visible gameplay, movement and clean close;
broader scene coverage remains necessary despite enabling the native default.

The query-handback checks can be repeated against saved reports:

```sh
python3 tools/rendering/verify-native-query-handback.py \
  reports/rendering/native-renderer/native-query-handback-01
python3 tools/rendering/verify-native-query-handback.py \
  reports/rendering/native-renderer/native-query-handback-before-02 \
  --allow-zero-native --require-reference-samples
```

Live snapshot planning preserves SHA-256 payload identities while avoiding
repeated hashing of identical data: a fast fingerprint is followed by exact byte
comparison against immutable retained bytes. The index lasts one frame and
never assumes a guest address is immutable. CPU bytes for GPU-owned texture
bindings are not interned. The capture-11 planner still matches all 1,623
operation layouts and same-provenance payloads, while its two-frame CPU check
fell from 9.25 to 1.51 seconds. This isolated measurement is not gameplay FPS.

Query validation must also account for reference work intentionally omitted by
asynchronous compilation. Reference results record the number of async-skipped
draws in each query. Such comparisons preserve native, raw reference and guest
counts, but are marked unverified. Correctness runs disable reference async
shader translation and pipeline compilation; no forced-positive query values
are introduced by the native path.

The executor retains immutable texture views and samplers with their decoded
image. All six fetch words and the current anisotropy settings identify a binding;
GPU image replacement retires its old bindings before destroying the image.
The cache is bounded to 16 bindings per image and 4,096 overall. Overflow uses
draw-local bindings until the existing submission fence completes, so no
descriptor can refer to an evicted view. Capture-11 validation created 351
bindings and reused 3,975, with 296 resident at the end and no overflow. All 67
raw RGBA32F images and 67 PPM previews are byte-identical to the preceding
mixed-mip-ownership replay. See
`captures/campaign-11/native-texture-binding-cache/binding-cache-comparison.json`
and `replay.json` under the native-renderer reports directory. This reduces
resource creation without changing submission order; gameplay frame-time
improvement requires a separate live measurement.

The original single-draw implementation reused five descriptor sets and the
index, constant and rectangle upload buffers after each completed draw. The
batched implementation keeps those resources separate in each pending slot and
reuses a slot only after its shared batch fence completes. Every active texture
binding is rewritten before submission; inactive array entries are covered by the
enabled `PARTIALLY_BOUND` feature, whose [Vulkan validity rule](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorBindingFlagBits.html)
does not require dynamically unused descriptors to retain valid resources.
Each upload allocation grows to a power of two up to the existing 16 MiB
limit, while each write retains the flush and submission ordering. The
`native-draw-resource-reuse` capture-11 replay created only five descriptor sets
and five upload buffers across 1,523 draws, with 3,043 buffer reuses. All 134
image files remain byte-identical to `native-texture-binding-cache`; the saved
`draw-resource-comparison.json` records each comparison. The latest completed
frame's `draw_timings` object separates setup, its texture-processing subset,
submission/fence wait, and completion bookkeeping without accumulating an
unbounded timing history.

## Bounded draw batching

Native FSI rendering defaults to `gow3_native_draw_batch_size = 64`. The internal
diagnostic accepts 1–64 and requires a restart; it is not exposed in the normal
settings menu. Compare the same executable with:

```sh
./tools/run-linux.sh --gow3_native_draw_batch_size=1
./tools/run-linux.sh --gow3_native_draw_batch_size=64
```

The file-backed replay uses `--draw-batch=1` or `--draw-batch=64`; its default
remains one. Each pending slot retains its command list, descriptors, upload
buffers and temporary texture bindings until completion. A batch drains at its
configured draw limit (at most 64), the upload-byte limit, a resource
replacement/readback dependency, or a non-draw operation. Resolves, clears and presentation remain ordered;
first-use shader compilation is still synchronous.

Command ownership distinguishes `NativePending` from `NativeCompleted`. Both
suppress only the exact accepted packet span; `NativeCompleted` additionally
requires completed GPU work. A committed split span stays owned through all of
its matching packets, even if later capture work fails. Before guest-visible
memory writes, enabled scratch-register fence writes, waits, query publication,
primary-buffer completion, presentation, fallback or shutdown, the command
processor drains accepted work. State-only register/shader packets can continue
recording without a fence. Query metadata can be inspected without waiting,
but sample counts and guest reports are available only after completion.
Submission or completion failure retains pending ownership and is fatal: it
never authorizes reference replay. Unsupported work rejected before acceptance
can still use the existing fenced, one-way state handback.

The same executable replayed capture 11 with batch limits 1 and 64. All 67 raw
images and 67 previews were byte-identical. Across both frames, 1,523 draw
submissions became 197. The warm second frame measured:

| Draw limit | Setup (ms) | Submission/fence wait (ms) | Completion bookkeeping (ms) |
| --- | ---: | ---: | ---: |
| 1 | 28.13 | 34.71 | 0.701 |
| 64 | 28.22 | 12.38 | 0.559 |

These are **offline, CPU-inclusive elapsed stage timings**, not GPU timestamps
or a gameplay FPS measurement. They do not include the complete live guest,
capture, window and display workload. The separate live measurements below
include that workload. Runtime logs report bounded 120-frame intervals
with median, p95, p99, maximum and counts above 50/100 ms. They also report
`draw_batches`, `texture_updates` and draw-stage timing counters.

Focused checks passed: 64 pending query command buffers retired exactly 16,384
samples; the pass suite checked 1,719,296 components/bytes; state export covered
the full 10 MiB EDRAM seed. The completion regression checks opcode/scratch-fence
boundaries, pending split-span suppression and failed-fence ownership retention.
These checks establish the tested ordering and output preservation, not broader
game coverage or target frame rate.

Offline evidence is recorded in
[the batching validation report](../../../reports/rendering/native-renderer/native-draw-batching-validation.json)
and capture 11's `native-draw-batch-64/replay-parity.json`. A separate batched
query handback published native 230,400 plus reference 0 as guest 230,400 and
cleared its sentinel. That harness timed out without a clean UI close, so it
proves query publication only, not a successful full run; see
[the scoped handback proof](../../../reports/rendering/native-renderer/native-batched-query-handback-01/query-publication-proof.json).

## Live performance comparison

Runs 06–09 used the same saved checkpoint, input sequence, 1× rendering settings
and 703 shader-cache files (identical manifest checksum). All four completed
without fallback and closed through the UI with exit 0. The comparison selects
whole 120-frame windows after the first confirmed stationary scene screenshot
and before the recorded five-second W input. A 0.5-second boundary guard avoids
marginal input/log timestamp alignment. Stationary screenshots show the same
room and camera framing, with different NPC/dust/animation phases.

| Run and change | Draw batch limit | Selected stationary frames | Range of 120-frame window medians |
| --- | ---: | ---: | ---: |
| 06: batching baseline | 64 | 360 | 99.89–101.85 ms |
| 07: per-draw fencing | 1 | 240 | 114.65–114.68 ms |
| 08: overlay readback changes | 64 | 120 | 102.41 ms |
| 09: accelerated hashing and shared operations | 64 | 360 | 89.87–92.65 ms |

Run 08's profiler-overlap windows are excluded, leaving only one comparable
stationary window. Run 09's median of window medians was 90.36 ms versus
101.58 ms in run 06, an observed interval reduction of about 11%. These are
sequential live runs with simulation and scheduling variation, not identical
render-command A/B replays. The reported ranges are window statistics;
individual frame intervals are unavailable for a pooled median or average FPS.
Run 09's selected window p95 values were 95.67–102.98 ms, and the largest
selected frame interval was 144.11 ms.

After movement, run 09's selected window medians were 60.06–60.57 ms. Screenshots
show different final camera positions across runs, with fewer operations in
this view, so that result is descriptive and is not used to claim a strict
code-change speedup. **The 100 FPS target is not achieved**: stationary gameplay
is roughly 11 FPS, with roughly 90 ms frames against a 10 ms budget. All tests had
a 30 FPS cap; capped movies/menus are excluded, and these runs cannot certify
higher-rate capability. Full-campaign coverage remains unverified.

[performance-comparison.json](../../../reports/rendering/native-renderer/performance-comparison.json)
records each included/excluded window, input timestamps, lifecycle outcomes,
cache identities and source checksums. The work documented here ends at run 09;
further pipeline-key and geometry changes need separate validation.

### Performance after the texture-coherence fix

The subsequent `deferred-query/live-synchronous-01` and `live-deferred-01`
runs isolate query deferral with the same binary and cache seed. Complete warm
window medians had medians of 64.62 ms and 59.38 ms respectively
(about 8% lower). Native draw submissions fell from 237 to 79 per frame.
The deferred run closed cleanly after 4,031 native frames and 327,584 real query
publications, with every ticket retired and none pending. Deferral uses exact
generation/address tickets and bounded GPU slots; guest waits, publication,
idle, fallback and shutdown still force completion. CPU and GPU fixtures cover
empty-query ordering, actual sample counts, reuse, pressure and failure retention.
The default live setting is `gow3_native_deferred_queries=true`; the harness
accepts `--synchronous-native-queries` for comparison. Evidence is in
`reports/rendering/native-renderer/deferred-query/query-benchmark-comparison.json`.
These sequential runs include simulation/scheduling variation and remain below
60 FPS. The table below records the earlier optimization rounds.

The October 1 `native-perf-coherence-01` and `native-perf-round1-01` through
`native-perf-round3-01` runs use the same stationary checkpoint and input
sequence, unlimited frame cap, draw batch limit 64, and an identical 1,117-file
native shader-cache seed. Full API capture and texture probes are disabled.
Only whole 120-frame windows inside log elapsed 140.5–179.5 seconds are selected;
the separate CPU profile at 100–130 seconds is excluded. Screenshots confirm the
same room and camera, with different animation phases. The first three runs
each contribute 360 selected frames; round 3 contributes 480. Individual
intervals are not retained, so these are window
statistics rather than a pooled median or average FPS.

| Run | Changes | Window median range | Median of window medians |
| --- | --- | --- | --- |
| Coherence baseline | Corrected weapon textures | 85.10–88.71 ms | 87.24 ms |
| Round 1 | Deferred GPU-only texture snapshots, lazy compatibility submission reopening, direct fixed16 resolve loading | 79.74–85.19 ms | 83.99 ms |
| Round 2 | Direct index decoding, mapped constant packing, completed CPU frame disposal worker | 64.95–74.43 ms | 66.07 ms |
| Round 3 | Recorded per-operation payload dependencies in place of recursive JSON collection | 63.91–64.67 ms | 64.18 ms |

The final run shows about 26% lower frame intervals than the coherence baseline
in this selection. Its selected window p95 values range from 67.93–69.25 ms;
individual spikes remain, including a 121.03 ms interval. It completed 3,885
native frames and 1,857,307 native raster
packets without fallback or fatal errors and closed through the UI with exit 0.
All 3,885 completed CPU frames were accepted and drained by the disposal worker;
only one frame was outstanding at a time in this run. Its hard limits are two
outstanding frames and 128 MiB of retained payload bytes, including the frame
being destroyed. JSON/map metadata adds memory beyond that payload count.
Admission is nonblocking; full/contended admission retains the prior inline
release behavior. No graphics resources or guest-memory pointers enter the
worker.

The selected compatibility submission median fell from 1,032 to 3 per frame.
Required reference commands, sparse binds, control effects, query publication,
fences and fallback remain ordered. CPU snapshots are deferred only for
textures proved fully GPU-owned after current CPU invalidations are processed;
CPU/mixed textures retain exact immutable snapshots. Diagnostic captures remain
complete. Index decoding preserves every supported 16/32-bit endian mode,
UP padding and topology expansion. Constant packing preserves raw float bits
and both shared ABIs, and writes only a previously fenced upload slot.

Validation includes 324 index equivalence cases (2.4 million indices), 120
invalid-input cases, 512 constant-packing ABI cases, disposal bounds/lifetime
tests, completion lifecycle tests, deferred-snapshot capture parity, and the
Vulkan state-export/fixed16 resolve fixtures. Both recorded firing frames
retain all 172 output images byte-for-byte after the rendering changes.
The dependency collector additionally matches a test-only copy of the legacy
recursive collector on both captures and synthetic delayed/empty prefixes,
cross-namespace digest aliases, map rehash, CPU/GPU copies and producer
destruction. This preserves exact dependency maps and immutable owners; the
game build does not include the legacy collector.

The improvement is insufficient for 60 or 100 FPS at this checkpoint. Sampled
draw submission waits still take roughly 20 ms per frame, alongside resolve,
texture and CPU planning costs. Sampled command-thread `memcmp` work is mostly
short JSON property and digest/cache-key comparisons; it is not evidence that
CPU texture-coherence checks can be removed. Typed draw metadata is a further
optimization candidate, not implemented here. Texture timing is a subset of draw setup and
must not be added to it again. Deferred query publication is documented in
`reports/rendering/native-renderer/deferred-query-design.json` but is **not
implemented**; actual query results and their current synchronization remain
unchanged. Sequential live measurements include simulation and scheduling
variation and do not establish full-campaign performance.

Detailed windows, cache identities, lifecycle results and counters:
[native-performance-progress.json](../../../reports/rendering/native-renderer/native-performance-progress.json).

Upstream sources and licenses: `vendor/xenos-recomp` (MIT),
`vendor/plume` (MIT), and the read-only Unleashed reference identified in
`reports/rendering/unleashed-renderer-assessment.json`. No Sonic assets or
game-specific renderer hooks have been copied into the Gears build.

Texture addressing helpers follow the BSD-licensed Xenia/ReXGlue utilities;
see `src/renderer/native/texture_data.LICENSE`. Plume itself is unchanged.
