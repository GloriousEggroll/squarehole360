# Checkpoint — 2026-10-06

The requested full decompilation and correct native Linux port are **not
complete**. This workspace now has a buildable native Linux static
recompilation and a separate unverified pseudocode export.
Normal launch selects the native Vulkan renderer. Native and Xenia are separate
restart-only modes; Native errors cannot hand scene work to Xenia.
The saved campaign checkpoint has passed native rendering, movement,
real query publication and clean shutdown; broader coverage and performance
work remain.

## Verified results

- GPU bottleneck in the Tickers fight (2026-10-22, `amdgpu_top` block
  counters, `tools/rendering/bench/gpu_counters.py`). At full 2,361 MHz and
  about 93 W, the command processor is 67–72% busy, while colour (21%), depth
  (31%), texture (28%), interpolator (51%) and memory (10%) are lightly
  loaded. The frame is bound by per-draw command processing, not pixels.
  Tried, sync-validation clean, kept off:
  - **`gow3_native_skip_readonly_sample_barriers`.** It skips 93% of sampled
    barriers, but frame time did not change.
  - **Batched, cached and pruned draw descriptors**
    (`gow3_native_batch_draw_descriptors`, `_cache_draw_descriptors`,
    `_skip_unused_descriptor_sets`). The fight was about 2% faster, but light
    pan frames were 3–4% slower.
  - **Draw batch sizes 8 and 64.** Both were worse than 16.
- Multisampled render targets fall back to single-sample storage (2026-10-22,
  `gow3_native_msaa_single_sample_fallback = true`). Previously the native
  renderer stopped when the game first used one, after the cutscene that
  follows the Act 1 boss fight. That was a 640×360 4x surface (EDRAM base 720,
  format 3), used 1,268 times during the transition. It now renders without
  anti-aliasing. Its EDRAM footprint is modeled as single-sample, so a later
  reinterpretation of its extra sample tiles is not tracked. The play test
  continued past that point. Each new multisampled layout is printed to the
  console and listed under `msaa_fallback` in the shutdown report.
- Threading, lookup and upload defaults (2026-10-22).
  - **Threading.** Planning runs on its own thread, driver submission on a
    worker, and up to four GPU batches are in flight
    (`gow3_native_async_planner`, `gow3_native_submission_worker`,
    `gow3_native_four_inflight_batches`).
  - **Lookups.** Exact hashed lookasides sit in front of the pipeline,
    CPU-image, shader and vertex-buffer maps
    (`gow3_native_pipeline_lookup_lookaside`,
    `gow3_native_cpu_image_lookup_lookaside`,
    `gow3_native_shader_lookup_lookaside`).
  - **Uploads.** Only changed 64-byte constant blocks are uploaded, using a
    fused SIMD compare (`gow3_native_delta_constant_uploads`).
  - **Page versions.** Block page-version summaries and range texture
    versions are on (`gow3_native_block_page_versions`,
    `gow3_native_range_texture_versions`,
    `gow3_native_compact_cpu_texture_version_pages`).
  - **Executor.** Draw borrows payload owners instead of copying
    `shared_ptr`s. ResolveMemory adds a covered-hull fast path in `Define`, a
    surface-version memo that survives disjoint writes, and footprint
    lookasides. Occlusion queries are typed operations rather than JSON.
  - **Render-thread capture.** A cached SDK page-permission check, recycled
    device-state buffers, a sampled device-state fingerprint, and lock-free
    planner statistics.
  - **Results** on the RX 9070 XT:
    - Tickers fight save, 1,500–2,500 operations: 10.8 → 7.3 ms
      (6.36 → 4.4 µs per operation).
    - Seeded pan: 2500+ operations 19.6 → 11.8/12.4 ms; 1500–2500
      13.3 → 9.4/9.9 ms; 500–1000 5.3 → 4.4/4.7 ms; under 500
      2.8 → 2.6 ms.
    - No DEVICE_LOST, and no increase in slow frames.
  - **Tests.** Planner parity, memory, constant, LRU and page-version tests
    pass.
  - **Rollback.** Any listed option can be set `false`.
- Hardware render-pass continuation is the default (2026-10-22,
  `gow3_native_hardware_render_pass_continuation = true`). Grouped draws into
  the same framebuffer share one render pass while attachment layouts are
  unchanged. Clean under Vulkan sync validation on the seeded pan; on the
  RX 9070 XT every frame band is 4–17% faster (under 500 operations about
  2.7 ms, 2500+ about 19.0 ms). Set it to `false` if a GPU hang appears.

- Hardware render targets are the default (2026-10-21,
  `gow3_native_hardware_render_targets = true`). Draws render into real
  Vulkan color/depth images with hardware depth, stencil and blending; packed
  EDRAM is touched only for aliasing, resolves and export. Seeded camera pan on
  the RX 9070 XT: frames with 2500+ operations about 19.7 ms, frames under 500
  operations about 3.0 ms (FSI: about 19.3 ms and 3.9–4.9 ms). Async shaders
  and pipelines work in this mode, so cold-cache hitches match FSI. Known
  differences from exact FSI: 7e3 and 10:10:10:2 targets are stored in fp16
  without per-write 7e3 quantization, and depth has no per-fragment float24
  rounding (resolves still round). Set the option to `false` for exact FSI
  output.

- A second default-OFF candidate caches the combined GPU texture definedness
  and version result only while its physical-memory serial is unchanged.
  The main build, CPU cases, queued-write GPU fixtures at capacities64/128 and
  fresh same-binary gameplay replay all pass. Replay images, complete query
  results and final packed memory are identical; cache hits are positive.
  Gameplay FPS benefit remains unmeasured. Details:
  `user-room-20261006-01/gpu-alias-state-validation.json`.

- The default-OFF conservative readonly-depth candidate is built. It preserves
  shader arithmetic and adds only the Vulkan DepthLess execution mode to the
  admitted D32 visibility-query shader. CPU numerical/transform checks and real
  GPU/Scene fixtures at capacities 64 and 128 pass. Fresh same-binary weapon
  replays with game-default filtering match all 172 image artifacts, two complete
  query results, and final 10 MiB packed memory; the new variant handles 184
  draws without refusals. Recorded desktop gameplay still falls below60FPS: visible held-view
  interval medians are around21.6ms and crouched cover around25ms. No matched
  FPS gain or default enablement is claimed. Details: `user-room-20261006-01/depth-less-validation.json`.

- Normal-desktop next-room reproduction is archived under
  `performance-60fps/user-room-20261006-01`. The earlier approximately 56 FPS
  headless result was not representative of the user's 4K HDR gameplay.
  Video-aligned, fully visible baseline windows increase from 607 to 1188
  draws/frame and 49.6 to 88.8 submissions/frame; medians 14.46→21.97 ms.
  The later window has zero shader/pipeline cache misses. These are different
  views in one route, not an optimization A/B. SDK completion time is nearly
  flat; added draw preparation and inflight queue pressure track the slowdown.
  A new default-OFF presenter timestamp diagnostic measures the actual 4K HDR
  paint submission at approximately 0.178 ms in reproduced gameplay, with no
  invalid query samples and no additional waits/submissions/barriers.
  A bounded exact-key shader lookup index is built, default OFF. The sanitizer
  oracle passes 28,838 lookaside and 37,974 retained-entry checks. The live
  candidate avoids about 94% of tree probes, but its visible medians remain
  22.48–22.68 ms; differing camera/simulation prevents claiming an FPS gain.
  Settings/cache/save files are preserved; a temporary output-size experiment
  was inconclusive because the compositor enlarged the window, and its config
  was restored byte-for-byte. Stable 60 FPS remains unachieved. See
  `current-status.json`, both recorded gameplay videos, and the visible-only
  analysis files in that report directory.
- Guarded hardware early-depth rejection is enabled by default for one
  pinned expensive material. Visible fragments retain the original packed
  depth/blending epilogue; active queries, missing pipelines and unsupported
  shapes use the original Native shader. Same-binary Radeon replay preserves
  all 172 images, both wrapped query results and the full 10 MiB captured
  render-memory history. Actual GPU fixtures at 64/128 query capacities
  validate all 16,777,216 lower-bound depth codes and reduce a controlled
  hidden draw from 5,120 shader invocations to zero. Two valid opposite-order
  stationary pairs improve 22.81→22.465 ms and 22.625→21.75 ms
  (1.5–3.9% lower median frame time; enabled 44.5–46.0 FPS). Selected windows
  have no events over 50 ms, but action/movement stability and 60 FPS remain
  unproved. Only about 21 draws per frame select this narrow guard.
  Evidence: `performance-60fps/early-depth-material-01/current-root-validation.json`
  and `default-enable-decision.json`. The complete matched source/build
  archive and the separately rebuilt default-enabled archive are preserved.
  The post-promotion run with no early-depth/batch override used both defaults,
  retired all 418,060 queries and closed cleanly; see
  `default-enabled-health-proof.json`. This is a health check, not new FPS proof.
- The broader exact FSI output experiment remains OFF. All 173 captured
  pixel materials pass compiler/cache/restoration checks, and all 346 paired
  modules pass Vulkan 1.2 SPIR-V validation. The helper GPU fixture passes
  156 paired cases; full natural replay at capacities 64/128 preserves
  172 images, both queries and final captured-history 10 MiB render memory.
  Its first valid same-build live pair is slower: 24.11 ms OFF versus
  24.82 ms ON (+2.94%). No reverse/default promotion was attempted.
  Evidence: `exact-fsi-stores-root-completed-checks.json` and
  `exact-fsi-stores-first-comparison-strict-02.json` under performance-60fps.
- Exact per-retired-slot constant delta uploads are built and remain OFF.
  The first valid same-build live pair measures 23.60 ms OFF versus
  23.79 ms ON (+0.8%); no reverse/default promotion was attempted.
  The integrated CPU oracle passes
  14,590 checks including 8 failed-flush recovery cases. Natural 64/128
  replay preserves all images, queries and final render memory. At capacity
  64 it writes 4,078,080 of 27,279,360 float-constant bytes; all shared ABI
  writes and original full-range flushes remain. Existing Scene stress
  fixtures with delta uploads enabled pass ten paired modes at each
  capacity and owned pending shutdown. These are host-store counts, not
  GPU transfer-byte or FPS savings. Evidence:
  `constant-delta-root-completed-checks-source-complete-02.json` and
  `constant-delta-first-comparison-source-complete-02.json`. The archive
  completeness correction adds only an omitted already-frozen CPU test
  source, with all measured binaries, original sources and policy unchanged.
  Current measured room remains
  below 60 FPS; stable 60 FPS and full campaign correctness are unproved.

- A separate hardware FP24 depth-writing experiment is now built and stays
  OFF by default. Its CPU numeric/classifier/authority fixture passes
  33,566,954 checks across all 16,777,216 depth codes. Both 64/128-capacity
  actual Scene fixtures pass ten paired modes and owned pending shutdown.
  Five shader modules pass SPIR-V validation. Full natural replay selects
  996 draws across two pinned vertex shaders, reuses depth 984 times and
  preserves all 172 images, both queries and every byte of the 10 MiB render
  memory. Missing and partial startup pipelines preserve the same outputs.
  The first valid live pair measures 23.22 ms OFF versus 25.50 ms ON
  (+9.8%); it remains disabled. Evidence:
  `retained-fp24-depth-root-completed-checks.json`,
  `retained-fp24-depth-hardware-on-validation.json` and
  `retained-fp24-depth-first-comparison.json`.
- A main-executable compiler code-model experiment is optional; its default
  preserves the SDK flags. Bounded ELF inspection finds no static 2 GiB
  range or object-size blocker in the executable. The opt-in small build
  links and passes real gameplay/query publication and clean shutdown.
  SDK/native libraries, guest addressing, floating-point rules and link
  options remain unchanged. All 56,955 dispatch entries preserve full-width
  targets and the audited calls/aliases pass. Matched room pairs improve
  24.84→23.40 ms (~5.8%) and 24.39→24.15 ms (~1.0%). The current Linux
  executable uses this verified optional small model; fresh configurations
  retain the SDK default. The 60 FPS goal remains unmet. Evidence:
  `main-code-model-selected-build-decision.json`.
- The narrower D32 experiment that omits the unused packed-EDRAM barrier
  remains OFF. Actual 64/128-slot GPU fixtures pass 1,324/2,220 checks,
  including full memory, real queries, reversed depth and owned shutdown.
  Paired gameplay replay preserves all 172 images and both queries while
  selecting 184 draws. Its first controlled room pair measures 23.68 ms with
  the barrier retained versus 24.28 ms omitted; no performance gain is
  established. Evidence: `performance-60fps/d32-barrier-skip-root-validation.json`
  and `d32-barrier-skip-first-decision.json`. The main build includes the
  diagnostic switch but keeps it disabled.
- The retained opaque R32_UINT color/writable D32 attachment experiment
  remains OFF. Actual GPU replay selects 182 draws, preserves all 172 images,
  both queries and every byte of the 10 MiB captured render-memory history,
  and passes owned shutdown with three dirty draws pending. Startup admission
  now reserves bounded optional-module headroom without evicting the original
  shader cohort or changing hard limits. The valid room pair selects the
  hardware path in live gameplay but measures 22.7 ms disabled versus 23.7 ms
  enabled (+4.4%); no gain is established. Its narrow coverage pays roughly
  one full-surface restore/export pair per frame. These are captured-history
  memory checks, not a separate zero-history oracle. Evidence:
  `performance-60fps/retained-opaque-headroom-root-proof.json`,
  `retained-opaque-headroom-quiet-pair-01.json` and
  `retained-opaque-headroom-default-off-decision.json`.
- A separate three-material retained-attachment catalogue is implemented
  experimentally and remains OFF. Its CPU policy fixture passes 1,767 checks;
  actual startup compilation validates three pinned pixel shaders, unchanged
  canonical cache bytes and repeatable derived modules. All eight retained
  canonical/candidate SPIR-V modules pass validation. Initial GPU rendering
  selects 502 draws across all three materials and preserves 172 images, two
  queries and the full captured 10 MiB render-memory buffer. That initial
  executable exits with a failed coverage gate because no retained material
  switch occurs naturally in this capture. Corrected validation includes a
  separate actual source-switch fixture and seven refusal/state cases whose
  images, queries and full memory match. The fixture that flips the captured
  reversed depth range to a forward range exposes three differing color words;
  a guard now refuses that unproven shape to ordinary Native rendering.
  Its controlled diagnostic room pair measures 22.34 ms disabled versus
  25.47 ms enabled; no gain is established and no default changes. Evidence:
  `performance-60fps/retained-opaque-catalog-gpu-validation-parity.json` and
  `retained-opaque-catalog-diagnostic-pair-01.json`. Queued ordinary-draw
  exports are a separate default-OFF follow-up; actual 64/128-capacity
  fixtures pass query ordering, CPU/foreign-write, full-memory and owned
  pending-shutdown checks. Complete replay preserves all 172 images, both
  queries and the captured 10 MiB render memory. The first isolated queued
  export pair measures 25.07 ms disabled versus 24.685 ms enabled (~1.5%);
  this is a small experimental comparison, not a gain against the shipping
  renderer. Evidence: `retained-queued-export-guard-full-parity.json` and
  `retained-queued-export-quiet-pair-01.json`.
- Retained attachment/scratch reuse is implemented behind a default-OFF
  switch. Actual 64/128-capacity fixtures verify two pending generations,
  covering-fence ownership, physical reuse, preaccept refusal and owned
  shutdown; full replay preserves all 172 images, two queries and 10 MiB.
  The first live pool pair fails its mechanism checks: the one-entry
  attachment pool churns when two generations overlap. No performance gain
  or default promotion is established. The two-entry fix is now built and
  passes the 64/128-capacity and full-replay memory/query tests without
  changing the 32 MiB accounting limit; fresh performance testing is pending. Synchronization validation
  and the new accepted-failure matrix remain pending. Evidence:
  `retained-resource-pool-full-parity.json` and
  `retained-resource-pool-diagnostic-quiet-pair-01.json`.
- Shared retained depth for read-only queries remains default OFF. Actual
  nine-mode Scene fixtures pass at capacities 64/128 with pooling OFF/ON;
  separate pooled/queued overlapping-generation fixtures also pass both
  capacities. The CPU authority model passes 452 checks. The natural full
  capture has no compatible sharing selections and fails its positive
  coverage gate, while all 172 images, two queries and 10 MiB still match
  ordinary Native. No live performance benefit is established. Evidence:
  `retained-pool-shared-depth-actual-execution.json` and
  `retained-shared-depth-natural-refusal-parity.json`.
- Exact query GetData helper inlining has repeated a small improvement in
  valid controlled comparisons. The actual SDK selector/body differential
  passes 48,826 checks across 5,267 cases, and production codegen removes only
  the two register-helper dispatches. The first attempted pair was invalid
  because the compiler folded a startup weak-alias check to constant false.
  The portable runtime pointer guard now authenticates all 233 instruction
  words. Fresh enabled/disabled runs complete with real query publication
  and clean shutdown. The first valid pair measures 24.62 ms disabled versus
  23.90 ms enabled; the reverse pair measures 24.71 versus 23.41 ms, an
  observed 2.9%/5.3% reduction. No measured intervals exceed 50 ms. The
  controlled scene still runs around 42 FPS, below the 60 FPS goal. Default
  promotion is now enabled for authenticated Native configurations; all
  startup body/alias/diagnostic guards remain. Evidence:
  `getdata-inline-alias-valid-first-pair.json`,
  `getdata-inline-alias-reverse-pair.json` and
  `getdata-inline-repeated-pair-decision.json`.
- A Native D32 depth-attachment path now defaults ON for compatible runtime
  configurations and replaces the packed-memory
  fragment interlock for one authenticated read-only visibility-query shape.
  It admits the actual BACK/CCW raster and complete forward or reversed depth
  ranges, preserves the original float24 quantization/GE test, and converts
  only fully defined depth history. The numeric CPU oracle passes 50,671,670
  checks across all 16,777,216 codes. Actual 64/128-slot Scene fixtures pass
  full 10 MiB memory and query oracles, asymmetric reversed depth, and owned
  complete/partial shutdown. The gameplay replay selects 184 queried draws,
  reuses history 182 times with two conversions, and preserves all 172 images
  and both real query results. Validation caught and fixed owned-device
  destruction order and the previously rejected reversed viewport; earlier
  zero-selection captures are failed evidence. The first valid quiet pair
  measures 23.63 ms disabled versus 23.30 ms enabled (~42.3 versus ~42.9 FPS),
  with no shader/PSO creation or eviction in the measured intervals. This is
  one descriptive pair. The reverse-order pair measures 23.94 ms disabled
  versus 23.53 ms enabled, repeating a small observed 1.4/1.7% improvement.
  One frozen runtime eligibility policy declines incompatible submissions or
  conflicting shader experiments; standalone Scene defaults remain OFF.
  Default boot/gameplay and synchronous-compatibility boots all exit cleanly
  with exact query publication and strict Native operation. The default
  gameplay check selects 396,387 real queries, reuses history 392,251 times,
  and measures 23.14 ms in its selected intervals (an unpaired smoke result).
  These results are not full-campaign or clean-install cold-boot proof.
  **60 FPS remains unmet.** Evidence under `performance-60fps/`:
  `readonly-depth-reversed-viewport-root-validation.json`,
  `readonly-depth-d32-first-comparison.json`,
  `readonly-depth-d32-reverse-comparison.json`,
  `readonly-depth-d32-default-decision.json`, and
  `readonly-depth-runtime-default-build/build-identity.json`.
- The combined pinned FSI-state/forward-control-flow shader experiment remains
  disabled. Captured hardware replay selects 182 queried draws and preserves
  all 172 images and both query results. An explicit zero-seeded fixture also
  preserves every byte of the 10 MiB render memory. The controlled room pair
  measures 24.13 ms enabled versus 23.25 ms disabled; no gain is established.
  Lower driver-reported register and instruction counts did not translate to
  faster gameplay. This is one pair, not proof of a repeated regression.
  Evidence: `performance-60fps/fsi-state-forward-cfg-root-validation.json`,
  `performance-60fps/fsi-state-forward-cfg-first-comparison.json` and
  `performance-60fps/fsi-state-forward-cfg-first-decision.json`.
- Retired Native DrawSlot descriptor content reuse is implemented but remains
  disabled by default. The 25,387-check CPU oracle and actual 64/128-slot GPU
  fixtures preserve resource lifetimes, partial-write failure handling, real
  queries and full 10 MiB render-memory contents. Captured replay skips 935
  image and 935 sampler writes while keeping all 172 images and both queries
  exact. The normal-room pair measures 24.67 ms enabled versus 24.66 ms
  disabled; no performance gain is established. Evidence:
  `performance-60fps/draw-descriptor-content-root-validation.json` and
  `performance-60fps/draw-descriptor-content-first-comparison.json`.
- Stable SDK-certified snapshots are now enabled by default at 256 KiB,
  after 32 exact full-byte reuse hits. Invalid tokens fall back exactly and
  cool down before rearming; unsupported platforms retain byte comparisons.
  Actual SDK alias/permission/write/lifetime tests pass. The independently
  gated checkpoint comparison measures 23.19 ms enabled versus 24.80 ms
  disabled, with 2,062,446 safe reuse hits and all queries retired. An earlier
  opposite-order pair measured 23.76/24.54 ms but lacked the stable shutdown
  counters, so it remains exploratory. The 32 KiB probe measures 23.08 ms
  with more protection/cold work; its threshold advantage is unconfirmed.
  Normal timing diagnostics remain off. This puts the controlled room around
  43 FPS; 60 FPS and rare action freezes remain unresolved. Evidence:
  `performance-60fps/stable-telemetry-root-validation.json`,
  `performance-60fps/quiet-pair-analyzers-v2-stage/stable-on02-comparison.json`
  and `performance-60fps/stable-certified-default-enable-stage/`.
- The attachmentless FSI experiment remains disabled. GPU memory, queries,
  derivatives, resizing and failure ownership pass, and all 172 captured
  images plus both query results are exact. Gameplay uses the candidate for
  about 791 draws per frame without a dummy image, but the controlled pair
  measures 24.60 ms enabled versus 24.54 ms disabled. No gain is established.
  Evidence: `performance-60fps/attachmentless-root-validation.json` and
  `performance-60fps/quiet-pair-analyzers-stage/attachmentless-on01-decision.json`.
- A separately keyed read-only depth-query builtin is default-off. It removes
  the unchanged packed store and fragment interlock only for an exact audited
  shader/state with no writes, retaining all inter-draw barriers. Its 4,743
  source checks, SPIR-V validation and capacity 64/128 GPU query/full-memory
  tests pass. Captured replay selects 184 queried draws and preserves all
  172 images and two query results exactly. The hardware gameplay pair measures
  24.31 ms enabled versus 24.01 ms disabled, so it remains off. Evidence:
  `performance-60fps/readonly-query-root-validation.json` and
  `performance-60fps/readonly-query-first-comparison.json`.
- Exact FSI depth/color/blend state specialization is implemented as a separate
  default-off Native shader/cache variant. All 174 captured pixel shaders plus
  the builtin pass cold/warm/corrupt-cache and SPIR-V checks. Hardware replay
  positively selects all 233 admitted tuples for 3,330 queried draws and keeps
  all 172 images and both query results exact. An eight-tuple sidecar exercises
  2,232 accepted ordinary Native fallback draws with the same parity. Device
  identity is now recorded; initial software-driver results are kept separate
  from hardware proof. The first controlled room pair measures 22.90 ms enabled
  versus 23.345 ms disabled, with zero stationary cache creation/eviction and
  full accepted-draw specialization coverage. The reverse pair measures
  23.35/23.72 ms enabled/disabled, confirming the small improvement. Actual
  hardware resource-limit tests preserve full packed memory and query results
  when optional modules/pipelines cannot fit. A selected-path lookup refinement
  also passes these GPU tests and all captured image/query comparisons. Its
  first pair measures 23.75/24.58 ms enabled/disabled, but the reverse pair
  measures 24.38/24.37 ms, so its first gain did not repeat. The current variant
  remains default-off. These results remain short of 60 FPS.
  Evidence:
  `performance-60fps/fsi-state-root-validation.json` and
  `performance-60fps/fsi-state-first-comparison.json`,
  `performance-60fps/fsi-state-reverse-comparison.json`,
  `performance-60fps/fsi-state-handoff-budget-gpu-test.log` and
  `performance-60fps/fsi-state-handoff-hardware-parity.json`,
  `performance-60fps/fsi-state-handoff-four-run-decision.json`.
- Earlier completed-query publication passes the current GPU pending-tail,
  same-address reuse, readback and shutdown fixtures. With FSI specialization
  enabled, opposite-order pairs measure 22.92/23.75 and 22.63/23.53 ms with
  prefix publication enabled/disabled. With FSI specialization disabled, the
  fresh pair instead measures 24.33/23.265 ms, so the normal prefix default
  remains off. The joint two-option pairs measure 23.135/23.265 ms and
  23.75/23.73 ms enabled/disabled, so no repeated benefit is established.
  Timing fields include a separately attributed host reset
  of already-signaled fences; this is not an additional GPU wait. Evidence:
  `performance-60fps/fsi-state-prefix-four-run-decision.json`,
  `performance-60fps/normal-prefix-first-decision.json`,
  `performance-60fps/joint-fsi-prefix-four-run-decision.json` and
  `performance-60fps/fsi-state-prefix-current-gpu-fixture.log`.
- A fresh 15-second exact-build profile places 34.03% of guest-thread sampled
  cycles in query GetData and 17.23% in residual snapshot comparisons. Recovered
  calls identify the blocking retry loop, but sampled cycles alone do not
  reveal each returned status or the query's required wall latency. Separate
  switch records show the guest thread scheduled for 99.37% of the interval
  and the Native command thread for 66.21%. Query submission/publication is
  being audited before choosing a change. Evidence:
  `performance-60fps/fsi-state-fresh-profile-01/cpu-analysis-exact-symfs/current-critical-path-source-findings.json`
  and that directory's `switch-independent-review.json`.
- A 15-second profile of the earlier `d718cfe13` build retains exact
  archived ELF identities and separate per-thread decoding. Guest capture
  accounts for 47.69% of sampled user cycles; exact snapshot lookup appears
  on 35.67% of that thread's cycles. Native command processing accounts for
  24.76% overall, with draw preparation on 57.66% of its cycles. Separate
  switch records show high short-sleep counts and 89.95% scheduled CPU time
  for the producer, but do not identify each blocked callsite. Device-wide
  GPU load during the same profiled interval has an 84% median. These
  overlapping measurements include profiler overhead and establish no
  exclusive critical path or FPS gain. Evidence:
  `performance-60fps/fresh-profile-02/cpu-analysis-exact-symfs/` and
  `performance-60fps/fresh-profile-02-gpu-load-summary.json`.
- Exact snapshot comparisons now support a portable pool bounded by reported
  hardware threads minus two, with private wakeups and coarse work ranges for
  explicit requests above two. The 2,025-case fixture covers byte equality,
  partitioning, ownership, allocation failure and exception/shutdown drains.
  The controlled checkpoint pair owns the requested two/eight helpers and
  measures 24.82/24.97 ms respectively, so the default remains two. Startup
  counts do not measure active worker dispatch. Evidence:
  `performance-60fps/snapshot-pool-root-validation.json` and
  `performance-60fps/snapshot-pool-decision.json`.
- The exact CPU texture page-union experiment remains disabled. Independent
  CPU tests pass 76,889 checks and captured GPU replay preserves all 172
  images plus both query results. Actual gameplay avoids about 31.6% of
  page visits during rescans, but the same-build pair measures 24.82 ms
  enabled versus 24.02 ms disabled. Fewer visits do not establish faster
  execution. Evidence: `performance-60fps/page-version-root-validation.json`
  and `performance-60fps/page-version-comparison.json`.
- Native FSI now discards the contents of its unobserved dummy color
  attachment by default. Exact packed-memory, derivative, query, resize and
  failure checks pass at both submission capacities; the captured workload
  retains all 172 images and both queried frames byte-for-byte. Two
  opposite-order checkpoint pairs measure 24.83/25.37 ms and 24.35/24.80 ms
  enabled/disabled. This repeated modest improvement still leaves the room
  around 40–41 FPS. It does not establish smooth action or exclusive GPU
  traffic savings. Evidence: `performance-60fps/dummy-fsi-root-validation.json`
  and `performance-60fps/dummy-fsi-four-run-decision.json`.
- GPU texture-footprint construction preserves every original address,
  capacity and endian validation while combining adjacent ranges before
  sorting and releasing unused vector capacity. The independent CPU fixture
  passes 665,503 checks; its complete set of layouts retains 1,245,232 bytes
  instead of 144,616,920 bytes. Those are fixture totals, not measured live
  memory savings. Actual GPU tests and all 172 captured images plus two
  queried frames remain exact. This change is enabled; no isolated live FPS
  benefit is claimed. Evidence:
  `performance-60fps/footprint-compaction-root-validation.json`.
- Recovered call stacks from the existing 15-second CPU profile now identify
  the callers of the earlier snapshot-comparison hotspot. Correct decoding
  requires the exact archived ELF files and separate per-thread perf walks;
  decoding all threads together still loses frames. Guest graphics capture
  accounts for 47.78% of sampled user cycles, and its planner is on 66.89% of
  that thread's cycles. The GPU command thread accounts for 25.10% of sampled
  user cycles: Native draw execution is on 57.67% of its cycles, with driver
  self time at 19.89%. Categories overlap and sampled cycles are not wall
  frame time. These are historical-build findings, not a fresh performance
  measurement. A default-off grouping experiment now records consecutive
  fully prepared draws in one physical command buffer, retaining separate
  bounded logical owners. GPU ownership, history and failure tests pass at
  both ring capacities. Captured gameplay retains all 3,330 draws, 206
  transfers, 172 identical images and two identical query results while
  reducing physical command buffers from 3,536 to 772. An empty live
  committed-state flush initially closed every group; the corrected path
  preserves dirty stores and failure refusal and now saves about 58% of
  physical command buffers in gameplay. Opposite-order pairs measure
  25.04 ms enabled / 25.21 ms disabled, then 25.59 / 25.41 ms. There is
  no repeatable FPS gain, so grouping remains default-off. Evidence:
  `performance-60fps/multidraw-v2-four-run-decision.json`
  and `performance-60fps/multidraw-v2-root-validation.json`
  and `performance-60fps/fresh-profile-01/cpu-analysis-exact-symfs/`.
- Thin live metadata copies are default-off. Captured immutable plans and
  resource identities remain byte-identical with 5,314 copies omitted, and
  the enabled live run avoids 4.33 GB of unused metadata. The controlled
  pair measures 25.785 ms enabled versus 25.225 ms disabled, so this does
  not establish an FPS improvement. Evidence:
  `performance-60fps/thin-metadata-root-validation.json` and
  `performance-60fps/thin-metadata-decision.json`.
- A bounded, default-off forward pixel catalog preserves 53 audited shader
  programs while removing eligible guest control-flow loops. CPU tests pass
  2,635 checks and all 53 actual compiled modules pass SPIR-V validation.
  The queried gameplay replay executes 910 selected draws, exercises every
  catalog source and preserves two real query results plus all 172 images
  exactly. Two opposite-order checkpoint pairs measure 24.920/27.030 ms and
  25.270/24.940 ms for enabled/disabled. The first benefit does not repeat,
  so the catalog remains off. All four runs authenticate the same private
  profile/save bytes, binary, caches and input sequence, retire every query,
  stay strictly Native and close cleanly. Evidence:
  `performance-60fps/forward-pixel-catalog-decision.json` and
  `forward-pixel-catalog-final-query-parity.json`.
- Startup catalog admissions now reserve bounded soft-trim headroom without
  increasing the hard 1,024-module/64 MiB shader-cache limits. The 2,246-check
  CPU fixture covers captured cache weights, overflow and original hard
  pressure. Queried replay preserves all 172 images and both real queries.
  A fresh isolated enabled run retains all 798 startup modules with zero
  subsequent module/pipeline creations or evictions and zero catalog residency
  refusals, replacing the earlier 34 recreations and 91 evictions. Its selected
  stationary windows measure 25.135 ms; this is cache-retention verification,
  not a paired FPS improvement. The catalog remains off by default. Evidence:
  `performance-60fps/catalog-headroom-root-validation.json`.
- Native frontbuffer decode, swizzle and shader-read transition now have
  optional timestamps around their original synchronous command lists.
  They add no submissions or waits and remain disabled in normal play.
  Four checkpoint runs report no diagnostic failures: these GPU intervals
  total about 0.037 ms/frame, with approximately 0.20 ms/frame of existing
  CPU fence waits. This excludes SDK gamma, UI scaling, swapchain presentation,
  earlier FIFO drains and compositor work; it is not total GPU frame time.
  Exact queried replay image parity is preserved. Evidence:
  `performance-60fps/presentation-source-audit.json` and the four
  `forward-pixel-catalog-*-coverage.json` reports.
- An isolated driver executable inspection confirms the pinned forward
  pixel shader reduces registers and instructions on this GPU: 108 to 72
  VGPRs and 3,812 to 3,019 instructions. The forward vertex candidate produces
  identical final machine code to the original. All 36 exported driver
  representations have authenticated extents and hashes. The tool's owned
  Vulkan device enables a different feature set from the game's SDK device;
  these are actual tool pipelines, not proof of live-game ISA or an FPS gain.
  Compiler occupancy and latency values are estimates. Evidence:
  `performance-60fps/pipeline-executables-analysis.json`.
- A default-off four-way texture-layout metadata cache retains the original
  1,024-entry and 4 MiB bounds. Its 32,337-case CPU fixture and active GPU
  replay preserve every image and real query. The live pair reduces metadata
  misses from 130.98 to 9.26 per frame but measures 24.980 ms enabled versus
  24.805 ms disabled, so this remains off. It changes no texture data or
  ownership. Evidence: `performance-60fps/associative-layout-root-validation.json`
  and `associative-layout-paired-comparison.json`.
- An exact original-format GPU texture transfer removes the recurring 64 KiB
  CPU round trip without altering RGBA32F bits or sampler properties. CPU
  tests pass 32,834 checks; real 64/128-capacity GPU fixtures preserve old/new
  immutable images, source ordering, precise queries, all packed EDRAM and
  bounded failure completion. Live counters confirm one queued 64 KiB copy
  per measured frame, but the matched checkpoint measures 25.430 ms enabled
  versus 25.090 ms disabled. Fence waits stay about 6.6 ms/frame, so this
  remains off by default. The 172-image replay is unchanged but does not
  exercise this layout. Evidence: `performance-60fps/raw-float-root-validation.json`
  and `raw-float-comparison.json`.
- The pinned forward-only pixel shader can remove its guest PC loop while
  preserving material clauses and packed FSI output. CPU tests cover all 16
  uniform branch combinations; the actual queried gameplay replay selects
  182 candidate draws with identical query results and all 172 images exact.
  A matched stationary pair measures 25.320 ms enabled versus 26.005 ms
  disabled, selecting 21 draws per frame. This is a small single-pair change,
  so the candidate remains off by default. Evidence:
  `performance-60fps/forward-cfg-root-validation.json` and
  `forward-cfg-comparison.json`.
- Reusing vertex-buffer allocations only after the existing full frame fence
  preserves real BDA geometry, query results, full packed EDRAM and failure
  ownership in the 64/128-capacity GPU fixtures. It replaces about 41.9
  physical allocations per frame with reuse, but the matched checkpoint
  measures 27.025 ms enabled versus 25.425 ms disabled. It remains off by
  default. Evidence: `performance-60fps/retired-vertex-root-validation.json`
  and `retired-vertex-comparison.json`.
- Queuing immutable page-sized CPU updates removes real completion barriers,
  but is not an FPS fix. Paired GPU tests preserve exact history, query results,
  upload ownership, the 128 MiB bound, null invalidation and synchronous CPU
  reads/copies. The isolated zero-draw query test covers its actual upload
  frontier. All 13 updates per room frame queue successfully: explicit drain
  waits fall about 0.99 ms/frame and submissions fall from 72.25 to 62.48/frame.
  The matched checkpoint only improves from 25.54 to 25.23 ms, and the enabled
  run has two frames above 50 ms. This remains off by default. The 172-image
  replay also matches, but contains no CPU updates and is only supporting
  regression evidence. Evidence: `performance-60fps/cpu-queue-coverage-pair.json`
  and `queued-cpu-write-root-validation.json`.
- A separately identified opaque color shader can omit a previous packed-word
  load for an audited full-word overwrite without changing material work or
  depth/query ordering. Its real GPU fixture passes 138 paired schedules and
  the captured replay selects 182 candidate draws with all 172 images exact.
  Startup prepares two resident-only pipelines; gameplay creates no shaders
  or pipelines. The matched room trial selects 21 draws/frame but measures
  25.975 ms enabled versus 25.340 ms disabled, so it remains off by default.
  Evidence: `performance-60fps/opaque-color-comparison.json`,
  `opaque-color-fixture-validation.json` and `opaque-color-parity.json`.
- A fresh CPU profile attributes 27.72% of sampled user CPU cycles to exact
  resource snapshot comparisons. A default-off size gate now restricts the
  existing all-alias SDK certificate path to large snapshots; smaller inputs
  still compare every byte. Fake and actual Memory/MMIO alias fixtures pass,
  including 5,388 external-write checks. Same-build/cache checkpoint trials
  measured 25.045 ms with certification off, 24.520 ms with a 256 KiB minimum,
  and 24.750 ms with a 64 KiB minimum. The enabled trials also use the previously
  tested batch permission queries and owned fault dispatch. These small,
  unconfirmed differences do not establish an FPS fix, so certification remains
  off. Evidence: `performance-60fps/fresh-profile-01/cpu-analysis/`,
  `certified-support-review.json` and `large-snapshot-comparison.json`.
- Diagnostic submission and wait reasons now identify why Native work is
  submitted or fenced. A default-off four-fence experiment preserves the
  64/128-list and 128 MiB scratch bounds, staging limits and draw targets. Real
  GPU fixtures pass exact queries, all packed EDRAM bytes, wrapped fence slots,
  leases, staged-tail retirement and shutdown at both capacities. Its matched
  checkpoint measured 25.790 ms enabled versus 25.835 ms disabled; reducing
  two-fence pressure shifted waits to draw ownership and final drains without
  a meaningful FPS gain. Two fences remain the default. Evidence:
  `performance-60fps/four-fence-root-validation.json` and
  `four-fence-comparison.json`. The room still does not meet 60 FPS.
- An experimental atomic depth path for two audited null-pixel-shader vertex
  families passed 918,652 CPU checks, eight paired GPU contention cases,
  synchronous/asynchronous Scene query and cache-transition cases, and all
  172 captured-gameplay images. A same-binary/cache stationary pair measured
  26.390 ms disabled versus 25.435 ms enabled, with 162 selected draws per
  frame and nearly identical total draw counts. Startup preparation now warms
  generic and atomic pipelines separately; the updated GPU fixture passes
  startup reuse, bounded caches and unsupported-builtin refusal. The reverse-order
  confirmation measured 24.140 ms disabled versus 25.365 ms enabled: GPU
  inclusive intervals fell about 0.5 ms, but the enabled run had more submissions
  and more CPU fence waiting. The FPS benefit did not reproduce, so the candidate
  **remains disabled by default**. Both pairs retired all queries, stayed strictly
  Native and closed cleanly. In the updated enabled run, no shaders or pipelines
  were created after startup. This does not fix the room's sub-60 FPS performance.
  Evidence: `builtin-atomic-depth/root-validation.json`, `decision.json`,
  `performance.json` and `prewarm/performance.json` within that directory.
- A fixed-position native camera sweep reproduced the room slowdown without
  walking or firing: roughly 36/35 FPS in two room views, 50 FPS toward the
  bright slats and 76 FPS facing the floor. These traced windows contain
  867/897/527/187 draws per sampled frame respectively; all 268,242 queries
  retired and all Xenia scene counters stayed zero. The return yaw differs
  from the initial view, so it is not an identical-view repeat. The added
  workload includes many unqueried depth-writing draws and opaque color
  draws; the video does not isolate a single material or particle effect.
  Optional unbound right-stick keys plus the private harness's
  `--camera-input` make further fixed-position tests repeatable. Evidence:
  `camera-view-investigation/sweep-01/camera-summary.json`,
  `camera-view-investigation/video/camera-findings.json` and
  `repeatable-camera-input/validation.json`.
- The rebuilt normal defaults passed a fresh checkpoint run with both graphics
  optimizations active: 9,234 Native frames, all 386,927 accepted queries
  retired, zero Xenia scene operations and clean shutdown. The selected 960
  stationary frames measured 25.105 ms (about 40 FPS), with no frame over
  50 ms in that interval. This is a warm-cache verification, not a new paired
  performance attribution or a whole-route smoothness result; 60 FPS is still
  unmet. Evidence: `action-perf/candidate-41-default-verification/default-validation.json`.
- Full color clears can omit a stale attachment restore when every pixel is
  overwritten. This is now enabled in the normal build alongside the one-pass
  depth/stencil restore. Dirty overlapping aliases still commit, packed layout/version
  validation remains, and the original clear/store updates ownership. The GPU
  fixture passes 34 paired schedules and 20 paired malformed-input cases;
  all 172 captured-gameplay images remain identical. The live pair measured
  26.88 ms disabled versus 26.07 ms enabled, eliminating nine restores and
  18 transfer stages per frame. Inclusive GPU intervals remain about 11.6 ms,
  so the observed improvement is mainly consistent with reduced host graphics
  work. This single stationary pair does not establish whole-route performance
  or 60 FPS. Evidence: `full-color-clear-overwrite/root-validation.json` and
  `full-color-clear-overwrite/performance.json`.
- A native GPU attachment-restore optimization replaces nine depth/stencil
  restore draws with one when stencil export is actually enabled on the device.
  This is now enabled in the normal build. The original Native path remains
  for unsupported devices; this never invokes
  Xenia. The real GPU fixture passes 69,990 checks, eight paired cases and 119
  actual queries, including all stencil values and disabled-extension coverage.
  Combined captured-gameplay validation preserves all 172 raw/preview images.
  Two same-binary checkpoint pairs measured 26.315/25.600 ms and 26.900/26.235 ms
  for original/optimized restores. Inclusive GPU batch intervals decreased
  from about 12.3 to 11.7 ms; these exclude presentation and may overlap.
  Roughly 135 restore draws become 15 per frame. This is a small improvement,
  not sustained 60 FPS or whole-game coverage. A separate exact packed-zero
  clear passes 1,605 CPU checks and 28 paired GPU cases but remains off: its
  isolated comparison was effectively unchanged and combining both changes
  was slower. Evidence: `gpu-clear-restores/root-validation.json` and
  `gpu-clear-restores/performance.json`.
- A separate engine occlusion-command experiment remains unverified and is
  not enabled in normal play. Direct and deferred startup commands stayed on
  the loading screen. A read-only debugger snapshot of the deferred run found
  the main thread in memory-fault handling and the sampled toggle word still
  zero; it did not establish a render-fence deadlock or disabled occlusion.
  These runs are excluded from gameplay performance comparisons. Evidence:
  `engine-occlusion-setting/audit.json` and
  `action-perf/candidate-41-engine-occlusion-deferred/engine-command-validation.json`.
- Optional per-draw GPU timing is integrated and stays off during normal play.
  Its 2,196-check ownership fixture and paired 64/128-slot real GPU tests pass:
  exact query samples, full 10 MiB memory and submission/fence counts match.
  A live checkpoint completed 8,979 native frames and all 374,072 queries,
  with no Xenia scene operations. All 154,665 timestamp pairs were available;
  the selected room interval covers 47,455 draws in 60 sampled frames without
  summary overflow. Costs are distributed across draw groups; queried draws
  account for about 1.14 ms of inclusive list latency per sampled frame.
  These intervals include dependencies and may overlap; they are not exclusive
  shader time or total GPU frame time. The traced checkpoint remains about
  25.98 ms versus 25.66 ms in the control, so sustained 60 FPS remains unmet.
  Evidence: `draw-timestamps/root-validation.json` and
  `action-perf/candidate-37-draw-profile/gpu-draw-analysis.json`.
- Completed host writes now notify physical texture-memory invalidation before
  retiring the active writer under the existing memory lock, including partial
  and failed reads. The bounded raw/A/C/E/7F alias union does not change page
  permissions, allocate retirement metadata, or wait on the GPU. The integrated
  actual-Memory fixture passes 5,388 checks; existing snapshot/coherence and
  certified-memory regressions pass. Certification remains off by default,
  and no performance improvement is claimed. Evidence:
  `external-write-completion/root-validation.json`.
- A second depth-query experiment removed the generic builtin's redundant
  store and fragment interlock only for pinned read-only room queries. The
  1,361-check structural fixture, SPIR-V validation, 392 real GPU queried
  draws (including the actual room vertex shader), full packed-memory parity
  and all 172 replay images passed. The matched checkpoint remained 25.95 ms
  disabled versus 26.01 ms enabled; inclusive GPU intervals were also unchanged
  at about 12.19 ms per frame. The experiment was removed from production.
  Staged code and evidence remain in `builtin-readonly-queries/stage/`,
  `builtin-readonly-query/`, and
  `action-perf/builtin-readonly-query-comparison.json`.
- CPU texture-footprint metadata now evicts one least-recent entry at its
  existing 2,048-entry limit. Exact six-word keys, ranges, version checks and
  failure behavior remain; 5,265 CPU checks and all 172 replay images pass.
  New counters showed 995 entries and no evictions in the stationary checkpoint,
  so no performance improvement is attributed to this retention change there.
  Evidence: `cpu-texture-footprint-lru/` and candidate-34 checkpoint reports.
- The October 2 22:34 diagnostic recording includes the user's room combat
  before fullscreen. The run completed 7,820 native frames and published all
  790,949 accepted queries; all six Xenia scene counters stayed zero. Clean
  visible intervals measured about 31 FPS in combat and 47 FPS near the exit.
  Higher presentation rates after opening settings are not gameplay rates.
  CPU profiles and bounded GPU batch timestamps identify recurring graphics
  preparation, depth-query and submission costs in addition to isolated
  compilation spikes. A same-build checkpoint comparison reduced submissions
  from 72.6 to 50.4 per frame by increasing the draw batch from 16 to 32, but
  frame time worsened from 26.81 to 27.92 ms. The default remains 16.
  Evidence: `room-video-analysis/room-check-02-exclusive/video-analysis/`,
  its `cpu-analysis/`, and `action-perf/exclusive-batching-comparison.json`.
- The renderer selector now offers Native (default) and Xenia with separate
  supported settings. Shared display, HDR and input options remain available.
  Native prerequisites are normalized before backend creation; stale inactive
  values cannot change the renderer. Renderer changes only take effect after a
  restart. Fifteen Python settings tests, 42 actual loader assertions, 41 guide
  navigation assertions and the filtered controller/policy fixture pass.
  The actual PM4 failure fixture passes 97 checks. A hardware Native startup
  rendered 335 frames and closed with all six Xenia scene counters at zero.
  Injecting a terminal error after five deferred native queries stopped the
  process with those counters still zero. Shared command decoding, device,
  queue and host presentation infrastructure remain. Evidence:
  `strict-renderer-modes/`, `strict-renderer-authority/`, `renderer-settings/`.
  This change establishes exclusive renderer ownership; it does not establish
  a gameplay performance improvement or complete whole-game coverage.
- Texture memory-footprint construction now merges adjacent ranges as they are
  generated and releases excess temporary vector capacity before caching. All
  10,588 old-algorithm comparisons and 172 replay images remain identical.
  A 512-square RGBA fixture retains 8 bytes of range metadata instead of 2 MiB;
  gameplay frame-time improvement is not isolated yet. Evidence:
  `texture-range-compaction/root-validation.json`.
- An experiment omitting provably identical readonly depth stores passed the
  full GPU memory/query fixtures and all 172 replay images, but did not improve
  the matched checkpoint: 26.945 ms enabled versus 26.41 ms disabled. Both
  runs retired all queries and closed cleanly. The experiment was removed from
  production, restoring the original shader compiler namespace and retaining
  the staged implementation for future investigation. Evidence:
  `action-perf/readonly-depth-comparison.json` and `fsi-readonly-depth/`.
- Review of the October 2 19:55:38 recording found that native rendering handed
  back to the compatibility renderer about 34 seconds into the video, after a
  CPU snapshot rearm race. The later 100+ FPS sections therefore do not measure
  native performance. The emptied room also remains slow when viewed from
  outside, supporting further investigation of view-dependent rendering cost.
  Native snapshots now use the existing portable memory lock across checked
  alias protection, copy and publication. Real SDK pending-store/refault,
  partial protection recovery, external-writer bounds and certificate regression
  tests pass. Uncertain ownership stops safely; arbitrary unnotified raw host
  writes are outside this fix's guarantee. Evidence:
  `room-video-analysis/video-findings.json` and `coherence-snapshot-transaction/`.
  The subsequent normal-settings route stayed native for 20,476 frames; all
  1,378,320 accepted queries retired and reached guest memory, with no pending
  GPU work at shutdown. Busy windows remained around 36–41 ms, while a later
  view reached about 14.1 ms. This is a workload comparison, not a matched
  before/after speedup. The user confirmed lower peak FPS than the original
  recording. Sustained 60 FPS in the busy view remains unmet.
- Large command prefixes no longer permanently disable lazy draw-state
  identities. The wide slice uses canonical state while later slices retain
  lazy identities. The 1,344-check planner fixture and all 172 firing replay
  images pass. This repairs the permanent fallback observed in the user's
  normal run; a live performance gain has not yet been established. Evidence:
  `scoped-wide-prefix/root-validation.json`.
- An optional `gow3_native_gpu_batch_report` path enables bounded batch timing
  and composition reports using the existing GPU timestamps. Mixed intervals
  remain unattributed; direct synchronous uploads/readbacks and presentation
  are outside this report. CPU and real GPU fixtures verify identical physical
  bytes and query results with reporting enabled and disabled. The normal
  default remains off. Evidence: `gpu-batch-attribution/root-validation.json`.
- A default-off cache-coherency wait experiment now submits and polls owned
  GPU work for narrowly validated texture/vertex cache waits, preserving full
  completion for memory reads, serial markers, fallback and presentation.
  The integrated SDK passes 4,518 CPU checks, real GPU alias/query/readback
  fixtures, all 172 firing replay images, and forced reference handback.
  Matched instrumented checkpoint runs measured 25.12 ms enabled versus
  25.92 ms disabled, with no selected frame above 50 ms. These full waits
  dropped from four to one per gameplay frame, but about nine serial-marker
  drains remain. Both runs published all accepted queries and closed cleanly.
  This small stationary improvement does not establish 60 FPS or smooth action;
  the option remains off by default. Evidence:
  `cache-coherence-waits/root-validation.json` and
  `action-perf/cache-coherence-comparison.json`.
- Snapshot metadata pressure now evicts one old entry instead of clearing all
  4,096 entries. Both action trials reached that limit: the enabled run made
  147 individual evictions and no full clear. The later action phase's maximum
  was 35.37 ms versus 242.24 ms in whole 120-frame windows; conservatively
  including boundary windows gives 51.49 ms versus 242.24 ms. Camera paths can
  vary, and the old run did not timestamp the clear, so this does not establish
  exact causation. Both runs closed cleanly and retired every query. The
  separate first-use shader/pipeline pause remained about 195–200 ms.
  Individual metadata eviction is now the runtime default;
  the bounded optional eviction history and ownership fixture also pass.
  Evidence: `action-perf/metadata-lru-action-comparison.json` and
  `certified-snapshots/lru-root-validation.json`.
- Exact completed-query prefix retirement now passes 590,478 CPU ordering
  checks, actual GPU query/ownership fixtures and all 172 captured firing
  images. The same-binary checkpoint comparison measured 23.92 ms enabled
  versus 25.16 ms disabled (about 42 versus 40 FPS); the enabled run also had
  five selected frames above 50 ms, including one at 142.46 ms. It remains off
  by default, and neither smooth action nor 60 FPS is established. Guest query
  polling also waits for a separate submission serial, so earlier query
  publication alone cannot remove that wait. Evidence:
  `action-perf/query-prefix-comparison.json` and `query-prefix-retirement/`.
- Per-capture permission batching reduced the measured loading transition from
  about 4.1 seconds to 1.5 seconds, while the stationary checkpoint remains
  about 42 FPS (23.88 ms). All accepted queries retired and both runs closed
  cleanly. The batch path passes real Memory/MMIO lifecycle tests and genuine
  read-only fault rejection; certification remains diagnostic and off by
  default. A checked integer parser now replaces general formatted scanning
  of OS map entries; its 995 valid-format comparisons, 25 malformed cases and
  complete memory/fault fixture matrix pass. With that parser, the two latest
  loading transitions measured about 1.2 seconds. Evidence:
  `action-perf/query-prefix-comparison.json`, `action-perf/implicit-fsi-comparison.json` and
  `certified-snapshots/proc-parser-root-validation.json`.
- An optional direct dispatch for proven snapshot write faults passes five
  normal SDK checks, including rejection of genuine read-only writes. In the
  checkpoint it bypassed 281,746 OS permission scans and measured 23.75 ms
  (about 42 FPS), with no frame above 50 ms in the selected stationary window.
  That build retained a four-second loading spike; the corresponding first gameplay
  Scene work is about 0.59 seconds with certification both on and off, isolating
  roughly 3.4 seconds outside measured Scene execution. The later permission
  batching result is recorded above. No shader or pipeline
  creation occurred there. Certification and this option remain off by default.
  Evidence: `action-perf/candidate-19-owned-faults/` and
  `certified-snapshots/candidate19-loading-gap-audit.json`.
- An offline pixel-shader variant removes only two audited device-wide memory
  barriers while preserving ordered pixel interlock and coherent EDRAM access.
  All 1,117 modules pass SPIR-V validation; 464 FSI modules changed and all other
  modules remain identical. The GPU precision fixture passes 26 cases and 14,080
  exact word comparisons; the 3,330-draw replay matches all 172 reference images.
  Runtime integration also passes all 172 reference images, eight GPU Scene
  cases with 4,096 actual query samples, and the full known shader cache audit.
  In the same-binary comparison, this variant measured 24.21 ms versus 23.88 ms
  with explicit barriers, showing no FPS benefit; it remains off by default.
  Evidence: `action-perf/implicit-fsi-comparison.json` and
  `fsi-implicit-barriers/` under `reports/rendering/native-renderer/`.
- The corrected certified-memory hit path measured 24.15 ms in the stationary
  checkpoint versus 26.395 ms with certification disabled (about 41 versus
  38 FPS). Adding the three GPU barrier options and compact channels measured
  23.99 ms, a small difference. **60 FPS remains unmet.** All three runs closed
  cleanly and published every accepted query. Certification remains off by
  default because the loading transition still took about four seconds.
  A later CPU profile shows memory comparison greatly reduced; guest query
  polling now dominates that thread's active CPU samples. Guest callchains
  were mostly unavailable, so the report distinguishes self-symbol evidence
  from the command thread's usable callchains. The exact build, cache seeds,
  timing windows and profile are in `action-perf/candidate-16-descriptors-shared`,
  `candidate-17-certified-fast-hit`, and `candidate-18-certified-gpu`.
- The normal native cache now includes 409 observed pipeline recipes and 134
  additional validated compiled shader files from this installation's own
  captures. Existing files were preserved. Startup preparation uses these
  records; they are not a whole-game cache. Evidence:
  `pipeline-prewarm/normal-cache-seed.json`.
- Native callback holders and per-draw descriptor batching now pass the CPU
  ownership tests and GPU resource/query fixtures. The 3,330-draw firing replay
  still matches all 172 reference images. Both options remain diagnostic while
  their live performance is compared. The optional certified-memory path also
  passes real MMIO and Memory lifecycle tests, but its first live run regressed
  to 46.885 ms in the stationary view despite 99.85% certificate hits. It remains
  off by default. Profiling review identified an unnecessary full-allocation
  metadata scan on every hit; the bounded replacement is measured above.
  That run rendered the checkpoint, published all 187,009 accepted queries and
  closed cleanly. Evidence: `batched-draw-descriptors/`, `shared-providers/`,
  `certified-snapshots/`, and `action-perf/candidate-15-certified-snapshots`.
- The current optimization pass still falls short of **60 FPS**. The latest
  same-binary stationary comparison measured 25.66 ms with three GPU barrier
  changes plus compact decoded channels, versus 26.125 ms without them (about
  39 FPS in either case). This small difference is not a robust speedup claim;
  those options remain diagnostic and off by default. All 172 saved firing
  images remain exact with them enabled, and GPU fixtures cover depth, sampled
  aliases, interlocked writes and query retirement. Earlier CPU changes include
  exact parallel snapshot comparisons, lazy state identities, SIMD constant
  packing and bounded metadata caches. Validated lazy state identities, texture
  layout caching, retained raster extent, two bounded comparison helpers and
  16-draw batches are now normal defaults, along with early shader preparation
  and observed pipeline preparation.
- Observed native pipeline preparation now records this user's actual shader
  combinations and can prepare them before guest launch using the existing
  progress UI. In the paired action trials, startup loaded 404 recipes; slow
  pipeline creations during play fell from 16 to one, and selected action frames
  over 100 ms fell from ten to one. The remaining 209.5 ms maximum coincided with
  a newly encountered shader/pipeline. Fixed-time movement changes the camera
  path, and this does not establish a sustained FPS gain or coverage of unseen
  content. Both trials published all accepted queries and closed cleanly.
  Closing during preparation now also exits cleanly: guest main-thread creation
  is deferred until loading finishes. Evidence: `action-perf/`, `draw-barriers/`,
  `compact-alias-channels/`, and `pipeline-prewarm/` under
  `reports/rendering/native-renderer/`.
- Typed live draw records now replace per-draw JSON construction and lookups;
  routine diagnostic JSON is materialized only when requested. The latest
  cache-seed comparison measured 30.1 ms before movement (about 33 FPS), versus
  36.695 ms with JSON draws, and 29.65 ms during movement versus 37.08 ms. The
  selected movement windows had two frames above 50 ms and none above 100 ms;
  the preceding run had 81 above 50 ms. Movement paths can differ with frame rate.
  Both cache layers had identical seeds and neither run had slow shader/pipeline
  creation. The candidate completed 10,414 native frames and all 1,299,595 query
  publications with clean UI shutdown and no fallback/fatal error. **60 FPS
  remains unmet.** Independent complete live-plan exports remain byte-identical;
  ordinary draw emission used 75.53% fewer C++ allocations in the capture fixture.
  Paired typed/JSON GPU tests cover real samples, provider access, malformed
  inputs and immutable owner lifetimes. A full 3,330-draw live replay matches all
  172 existing firing images, both through typed dispatch and its JSON adapter.
  The typed-only candidate measured 30.69/28.64 ms; the later report cleanup does
  not establish an isolated movement FPS gain. Logical query capacity remains
  64; an internal 128-record option
  passes ordering fixtures but has no demonstrated FPS benefit. Evidence:
  `movement-perf/latest-comparison.json` and `typed-live-draw-phase-a/` under
  `reports/rendering/native-renderer/`.
- An earlier executable was also checked with this installation's normal
  native shader cache and a private copy of its Mesa driver cache. It completed
  8,387 native frames and all 981,160 query publications, with clean UI close and
  no fallback/fatal error. Selected movement windows measured 40.28 ms, with
  one frame over 100 ms (167.52 ms maximum); stationary sections measured 39.96
  ms before movement and 23.895 ms in the different final view. Pipeline creation
  for 358 entries totaled 118.72 ms versus 12,972.03 ms with the same build's warm
  native shader cache but a fresh driver cache. **This cache benefit is not a new
  code speedup**, and fixed-time inputs still produce different paths. Native
  shader and driver cache warmth are now recorded separately by the harness.
  The additional validated native shader entry was merged into the normal cache
  without replacing existing entries. `movement-perf/validation.json` collects
  all runs and correctness evidence. The later typed-draw comparison above
  isolates its starting cache layers more closely.
- Movement profiling found a texture conversion reading back from mapped upload
  memory on the CPU. Conversion now reads immutable CPU bytes and writes staging
  memory in bounded chunks. With the same starting native shader cache, selected
  movement windows improved from 80.68 ms to 35.86 ms in the first candidate;
  the latest flat-resource candidate measured 37.285 ms (about 27 FPS), with
  stationary windows at 39.46 ms. Fixed-time movement can produce different
  camera paths, so these are not identical-workload per-frame comparisons.
  **60 FPS and consistently smooth frame times remain unmet**: the latter run
  still recorded a 1.40-second first-use hitch. New shader input/driver creation
  timers expose compilation stalls separately from steady CPU work.
  The latest cold-seed run completed 7,209 native frames and all 805,862 query
  publications with no fallback/fatal error and clean UI shutdown. Evidence:
  `reports/rendering/native-renderer/movement-perf/`.
- Native pipeline/shader objects and immutable CPU index buffers now have bounded
  persistent caches. Extra dormant CPU textures are retained within bounds;
  extra dormant vertices are off by default after profiling showed weak reuse.
  Informational per-operation JSON histories are disabled for normal gameplay;
  explicit offline exports retain them. Small command-prefix resource tables
  use two bounded flat arrays instead of copied string/map nodes (30 to 2
  allocations in the representative fixture). Complete capture plans remain
  byte-identical; all 172 firing replay images matched after the rendering/cache
  changes. Flat tables show no additional live FPS gain yet. Experimental packet
  cleanup offloading was removed for lack of measured benefit; original full-frame
  cleanup remains. Successful query logging is now sparse, with exact counters
  and failures preserved. Runtime changes use portable C++/Vulkan.
- An earlier built executable enabled bounded asynchronous native GPU submission
  by default and keeps diagnostic GPU timestamps off. A same-binary comparison
  measured 47.435 ms with one submission versus 42.35 ms with two in flight
  (10.7% lower); mean host fence wait fell from 15.405 to 9.557 ms per frame.
  Both completed cleanly with every query published. That build, including
  lazy diagnostic serialization, measured 41.72 ms across 840 selected frames
  (window medians 41.21–41.84 ms), about 24 FPS. It completed 5,906 native frames
  and all 471,892 query publications without fallback/fatal errors, then closed
  through the UI. That run disabled GPU timestamp instrumentation; its
  difference from the preceding instrumented run does not isolate that cleanup's
  gain. **60 FPS remains unmet.** CPU draw preparation, metadata handling and
  synchronization remain major costs. Native GPU batch intervals averaged about
  12.27 ms in the synchronous diagnostic run; these are inclusive elapsed GPU
  intervals, not exclusive utilization or an independent addition to host waits.
  Evidence: `reports/rendering/native-renderer/cache-epochs/submission-comparison.json`
  and `lazy-diagnostics/validation.json`.
- Device-state interning now removes a redundant fingerprint/copy while retaining
  canonical SHA identities and failure diagnostics. Complete capture plans remain
  byte-identical for 4,848 operations. CPU-image/pipeline/shader cache epochs pass
  37,974 eviction checks and 1,089 key comparisons; 3,300 shader hits allocate no
  temporary keys. Lazy diagnostic reports preserve pending/drained/failure/export
  counters without rebuilding JSON per transfer. Both GPU submission modes pass
  query/lifetime/report tests; all 172 firing replay images remain byte-identical.
  Original texture-byte comparisons remain necessary: existing write watches do
  not cover every raw physical writer. Source audit and requirements are saved in
  `reports/rendering/native-renderer/snapshot-write-watch-audit.json`.
- Retaining exact texture generations until their consuming draws fence removes
  replacement-time full waits. The selected checkpoint measured 42.96 ms,
  versus 47.64 ms in the preceding clean asynchronous run. All 5,622 native
  frames and 453,988 visibility results completed, with clean UI shutdown and
  no fallback/fatal errors. GPU fixtures cover two in-flight fences, exact
  samples, replacement during draw preparation and memory-pressure retirement;
  all 172 saved firing images remain byte-identical. These sequential results
  are not an isolated simulation A/B. Asynchronous submission was disabled by default during this earlier test;
  the later paired result above supports its current enabled default. **60 FPS remains unmet.**
  Evidence: `reports/rendering/native-renderer/alias-generations/validation.json`.
- Queued 2D GPU texture conversions and cached bridge shader-input decoding
  reduced the selected checkpoint interval to 49.05 ms, versus 51.62 ms for
  the preceding shared-queue run. All 4,943 native frames and 366,360 deferred
  queries completed without fallback/fatal errors, then the run closed through
  the UI. Exact sampling tests cover changed/reused textures; all 172 firing
  replay images remain byte-identical. Complete shader-input capture oracles
  passed 6,480 events. These sequential runs do not isolate each change's gain.
  A subsequent texture-version memoization run measured 54.91 ms, with increased
  GPU waits despite a slightly smaller version-lookup timer; it establishes no
  frame-rate gain. The later two-submission validation above supersedes this measurement. **60 FPS remains unmet.** Evidence:
  `reports/rendering/native-renderer/queued-alias-loads/validation.json` and
  `texture-version-memo/validation.json` under the same report directory.
- Batching the GPU stages within each native clear/resolve reduced the measured
  checkpoint interval from 59.38 ms to 51.52 ms (median of complete 120-frame
  window medians, about 13% lower). The run completed 4,694 native frames and
  all 377,190 deferred query publications, then closed through the UI with
  no fallback or fatal error. All 172 firing replay images remain byte-identical;
  real GPU tests cover transfer ownership, pressure, readback and state export.
  That earlier build still fenced each clear/resolve operation; the later
  shared-queue results above supersede its synchronization behavior.
  **60 FPS remains unmet.** Evidence:
  `reports/rendering/native-renderer/transfer-batching/validation.json`.
- Deferred native visibility queries now share GPU submissions while retaining
  real sample counts and exact report/generation ownership. Same-binary runs
  measured 64.62 ms with synchronous queries and 59.38 ms with deferral
  (medians of complete 120-frame window medians, about 8% lower). Native draw
  submissions fell from about 237 to 79 per frame. The deferred run completed
  4,031 native frames and all 327,584 query publications, closed through the UI,
  and left no pending results; no fallback or fatal error occurred. CPU and
  real GPU tests cover empty-query ordering, slot pressure, address reuse and
  exact sample counts. Forced fallback at an ended query retired all 1,000
  tickets without reopening that query. **60 FPS remains unmet.** Evidence:
  `reports/rendering/native-renderer/deferred-query/query-benchmark-comparison.json`.
- Native-renderer performance work now removes GPU-owned texture snapshots,
  empty compatibility submissions, a redundant fixed16 resolve load, repeated
  index byte swapping, temporary shader-constant buffers, and recursive JSON
  walks when collecting command-batch dependencies. Completed CPU
  frame data is destroyed by a portable bounded worker after native completion;
  graphics resources retain their existing fence ownership. The same checkpoint,
  input sequence and 1,117-file shader-cache seed measured 87.24 ms before these
  changes and 64.18 ms afterward (median of complete 120-frame window medians
  outside profiling: three baseline windows, four final windows, about 26%
  lower). Window medians ranged 85.10–88.71 ms before and 63.91–64.67 ms
  afterward; these are sequential live runs, not identical simulation frames.
  All 3,885 final candidate frames completed
  natively without fallback or fatal errors and the run closed through the UI.
  All 172 recorded firing replay images remain byte-identical. CPU tests cover
  endian/index bounds, constant ABI bits, worker lifetime, deferred snapshots
  and compatibility completion; direct fixed16 resolve/state-export GPU tests
  also pass. A test-only legacy collector confirms exact batch dependencies,
  ownership and retained-byte counts on both firing captures and reuse cases.
  Evidence: `reports/rendering/native-renderer/native-performance-progress.json`
  and `native-perf-round3-01/`. **60 FPS and 100+ FPS remain unmet** at this
  checkpoint. The later deferred-query results above supersede this round's
  roughly 20 ms per-frame draw submission waits.
- Weapon-effects texture corruption now has a measured coherence fix. CPU
  texture snapshots rearm write notifications before capture, and native CPU
  memcpy uploads only the destination instead of retaining its unwatched
  source as authoritative GPU history. Before the change, 12 of 16 probed
  assets differed from the reference in consumed texture blocks. Afterward,
  all five selected particle/smoke/distortion assets match every decoded mip
  and block; three corrected assets previously differed by 32,195, 4,859 and
  51,850 consumed bytes. Remaining allocation differences are padding.
  The isolated LT-aim/RT-fire run completed 2,118 native frames and 582,602
  native packets without fallback/fatal errors, then closed cleanly. A close
  race found in the baseline now lets an admitted prefix finish while keeping
  failed GPU completion fatal. CPU coherence/probe/completion regressions and
  the real 8,192-byte Vulkan copy fixture pass. Both captured reference firing
  frames retain all 172 replay images byte-for-byte after the fix.
  Evidence: `reports/rendering/native-renderer/weapon-texture-after-01/`,
  `weapon-texture-after-vs-reference.json`, and
  `weapon-reference-01/native-coherence-replay/replay-parity.json` under the
  same report directory. This verifies sampled texture data and the tested
  firing sequence, not all effects or cross-frame pixel equivalence. The
  100+ FPS target remains unmet; diagnostic capture runs are not benchmarks.
- User-supplied assets now have a portable, standard-library Python importer:
  `python3 tools/import-assets.py import /path/to/gow3.iso`. It verifies the
  supported XEX, reads XDVDFS without platform extraction binaries, stages fresh
  installations, and checks SHA-256 before publishing. Existing extractions
  can be adopted only after byte comparison against the ISO; originals are not
  overwritten. The current `game/` tree was adopted successfully. A separate
  fresh extraction matched all 2,592 files (7,927,812,868 bytes), and a repeated
  import verified/reused it. Eighteen synthetic importer tests pass. Preparation
  creates a local catalog and 225 global shader inputs; optional local
  compilation produced 225 SPIR-V shaders with no failures. The Vulkan semantic
  test using the newly imported constant-color shader passes 5,120 comparisons.
  Full verification covers all original files and 905 prepared files. Normal
  launch/build wrappers check imported assets and direct users to setup when
  missing. Renderer capture tests use synthetic data by default; the three local
  capture checks skip when game captures are absent. Game assets, native shader
  outputs and captures must not be distributed. This does not yet decode all
  UE3 textures/meshes, replace the game renderer or precompile every pipeline.
  Windows importer execution is not yet validated. Details and extension points:
  `tools/assets/README.md`; evidence: `reports/asset-import-validation.json`.
- Display settings now expose **Windowed / Borderless fullscreen** and apply
  window width/height live. Leaving fullscreen restores the configured logical
  client size after the compositor acknowledges the transition. GTK allocation
  notifications now update the Vulkan surface, and pending Wayland layout runs
  even when the opaque Vulkan child suppresses the parent's frame callbacks.
  The earlier window-only test missed stale rendering; its replacement attaches
  the actual Vulkan presenter and checks the rendered target dimensions too.
  It passes startup-fullscreen exit to 1280×720, live resize to 1024×576,
  fullscreen rendering at 2560×1440, and a second exit to 1280×720, in isolated
  Wayland (100% and 125% scale) and Xwayland sessions using lavapipe.
  Vulkan subsurfaces now register with GTK's frame clock so the parent's
  committed geometry stays current too; resizing only the Vulkan target left
  the displayed image clipped/off center. A visual regression draws colored
  edges and a center marker, then checks compositor screenshots. The previous
  implementation fails this check; all 24 final captures pass across windowed
  and fullscreen startup, both Wayland scales and the Xwayland fallback.
  The GTK test is Linux-only in CMake; this platform fix is contained in
  `surface_gnulinux.*`, leaving the Windows window/surface interfaces intact.
  Eight settings GUI tests previously passed. Desktop tiling may constrain
  client-requested sizes; the tests use a private compositor to isolate that
  policy. Logs: `reports/rendering/native-renderer/window-size-*.log`.
- The native renderer executes complete consecutive captured frames and live
  campaign scenes with raster, query and presentation ownership. **Normal launch
  originally selected native rendering with compatibility fallback (now removed). Native campaign
  output, movement and clean shutdown pass; performance work remains.**
  Campaign run 05 reached the saved checkpoint with visible Marcus, squad and
  HUD, completing 2,111 native frames, 542,785 native packets and 72,029 query
  publications without fallback. Movement visibly changed the player/camera,
  and window close returned exit 0. The overlay upload double-free exposed by
  campaign run 03 is fixed and both runs 04 and 05 close cleanly. Warm campaign
  samples improved from roughly 700 ms to 74–100 ms after caching exact surface
  footprints and reusing draw resources, before the draw-batching change below.
  That run was about 10–13 FPS in the tested checkpoint; it does not measure the
  batched build or establish a 60 FPS-ready renderer. First-use shader compilation
  still causes long pauses. Hardware/GLES raster run 05 completed 713 native frames, 15,402
  raster/copy packets and 24 real query publications without fallback, then
  closed cleanly. These include the opening movie and title scene, not campaign
  coverage. Earlier raster run 04-display verified visible native movie output
  and correct reference continuation after coherent state handback. Hardware
  screenshots use GLES in the private compositor; its pixman path could not
  import GPU buffers.
- Capture 11 pairs API frames 7698–7699 with initial GPU state: 611 physical
  ranges, 35,037,184 bytes plus 10 MiB EDRAM. Native replay executes 1,523 draws,
  36 clears, 62 resolves and 358 GPU texture bindings. Physical history persists
  between frames; EDRAM storage handles tile overlap, wraparound and packed
  depth/stencil. Texture reads fail before drawing if any required history is
  undefined. CPU write watches invalidate stale page ownership, and epoch
  checks prevent known later CPU writes from being overwritten during handback.
  Exact native state transfers pass 32,768 byte comparisons; executor validation
  includes the complete 10 MiB EDRAM seed and partial physical ownership.
- Resolve destination channel swaps now match guest metadata, fixing the
  duplicated character blur. Native sampling honors the configured anisotropy
  and forces trilinear filtering when anisotropy is active. A reference SPIR-V
  stacked-texture interpolation bug also biased the comparison; after fixing
  it, an isolated final pass given identical inputs differs by at most 1/255
  per RGB channel. The hard particle shapes and larger lighting differences came from stale CPU
  mip data where newer GPU-owned mip bytes existed. Overlaying the exact defined
  GPU bytes reduces final raw RGB RMS error from 0.02824 to 0.000948. Of 921,600
  pixels, 921,481 have every RGB channel within 1/255 of the reconstructed
  reference. Remaining edge/motion differences and original-capture equivalence
  are not yet fully resolved.
  Old reference traces omitted some resident CPU textures. Future captures
  include them; recovered diagnostic replays are marked as reconstructed state
  and cannot establish original-capture equivalence.
- Focused native validation covers mixed signed texture filtering, packed
  fixed/7e3 blending, depth/stencil, polygon offset and constant blending. The
  production raster epilogue passes 26 GPU cases and 14,080 packed-word checks.
  All eight alpha comparisons pass 192 GPU cases, including NaN/Inf inputs.
  A pipeline-key reduction preserved all 67 captured raw outputs byte for byte
  while reducing the distinct pipeline keys from 162 to 146. Multisampling,
  gamma targets, extended half formats and broader early-depth cases still
  need implementation or validation before claiming general coverage.
- Live command ownership validates immutable packet spans by address, bytes and
  reuse generation. Predicates are accepted only for an exact executing draw
  whose CP callback proves the condition passed; other control-dependent spans
  remain unsupported. The recognized memory-copy intrinsic has replaced three
  real startup exports totaling 20,480 bytes. Native query authority, exact
  generation publication, and native-to-reference subtotal handoff are wired;
  forced mid-query fallback now passes both observed transition edges: native
  230,400 + reference 0 samples, and native 0 + reference 230,400 samples. Both
  publish the actual guest count, clear its sentinel and close cleanly. Mixed
  nonzero native/reference contributions within one query remain unobserved.
  Campaign run 03 now publishes actual varying native query results in gameplay.
  Shadow run 10 previously matched three consecutive query
  generations and four frame queries, while reference still owned publication.
  State handback and clean window close passed raster run 03 after a real native
  clear. Shutdown quiesces native work before terminating guest threads.
- Campaign run 02 reached Continue under native authority (1,302 frames,
  110,275 native packets and 168 query publications), then handed control back
  on an unsupported format-2 resolve. Reference continuation reached the saved
  checkpoint and closed cleanly. R8 physical storage and unsigned 8-bit resolve
  conversion are now implemented; the GPU pass suite passes 1,719,296 component
  and byte comparisons, including partial-byte ownership, neighboring-byte
  preservation, explicit resolve address, half-up rounding before host R8
  quantization, channel swap and exponent. Signed/nonzero-endian R8 resolve
  variants reject before mutation pending validation. Evidence:
  `reports/rendering/native-renderer/format2-resolve-validation.json`.
- Immutable physical snapshots now reuse bounded cached allocations after an
  exact byte comparison, including after guest address reuse. Consecutive
  frames can also reuse verified content identities; no address or non-cryptographic
  fingerprint alone is trusted. The full capture-11 planner still matches all
  1,623 operation layouts/same-provenance payloads, with five explicitly excluded
  initial-ownership differences. CPU-write/lifetime/eviction fixtures pass.
  Texture binding reuse reduces creation from 4,326 to 351 for the same replay;
  all 67 raw images and 67 previews remain byte-identical. Per-packet dirty-state
  flushes no longer construct discarded ownership-export vectors; the GPU state
  export regression passes exact packed bytes, preserved ownership and repeated
  flush idempotence. Bounded descriptor/upload reuse creates five descriptor
  sets and five upload buffers for the 1,523-draw capture. Exact EDRAM layout
  caching reduces 770-draw completion bookkeeping from 484.5 ms to 0.68 ms;
  all 134 raw/preview images remain byte-identical. Campaign runs 04 and 05
  validate normal movement and clean close with these changes.
- Native FSI draws now use bounded batches, defaulting to 64 retained draw slots.
  `NativePending` preserves exact packet ownership until completion;
  `NativeCompleted` still requires a completed fence. Guest-visible writes,
  scratch fences, waits, query publication, primary-buffer completion,
  presentation, fallback and shutdown drain accepted work. Queries retain real
  fenced counts, and failed completion cannot authorize reference replay.
  The restart-only diagnostic `gow3_native_draw_batch_size` accepts 1–64; one
  restores per-draw fencing. In a same-executable capture-11 comparison, all
  134 raw/preview images remained byte-identical and draw submissions fell from
  1,523 to 197. Warm second-frame setup/wait/bookkeeping measured
  28.13/34.71/0.701 ms at batch 1 and 28.22/12.38/0.559 ms at batch 64.
  These are offline, CPU-inclusive elapsed timings, not GPU timings or proof
  of gameplay FPS. Live measurements are summarized separately below.
  Focused GPU checks passed 64 pending query command buffers with exactly
  16,384 samples, 1,719,296 pass-suite component/byte comparisons and full
  10 MiB EDRAM state export. Completion-policy tests cover control/scratch
  barriers, committed split spans and failed-fence ownership retention.
- Campaign run 09, with accelerated SHA-256 and shared immutable operation
  metadata, completed 2,778 native frames, 989,135 owned packets and 173,180
  query publications without fallback, then closed through the UI with exit 0.
  Runs 06–09 all started with the same 703 shader-cache files and checksum
  manifest. Comparing whole stationary checkpoint windows after the scene
  screenshot and before the recorded movement input, run 09's three 120-frame
  window medians were 89.87–92.65 ms, versus 99.89–101.85 ms in run 06 and
  114.65–114.68 ms with batch size 1 in run 07. Run 08 retained only one
  comparable unprofiled window, at 102.41 ms; profiler-overlap windows were
  excluded. The median of window medians fell from 101.58 to 90.36 ms relative
  to run 06 (about 11%), with NPC/animation and scheduling variation still
  present. These are presentation-callback wall intervals, not GPU timings or
  pooled frame statistics. Run 09's post-movement windows were 60.06–60.57 ms,
  but its changed camera view makes them unsuitable for a strict comparison.
  **The requested 100 FPS target is not achieved**: the stationary workload is
  roughly 11 FPS. Tests used a 30 FPS cap and do not certify higher-rate
  capability. Evidence, scene/input selection and source checksums:
  [performance-comparison.json](reports/rendering/native-renderer/performance-comparison.json).
- Live snapshot planning reuses immutable content only after a fast fingerprint
  and exact byte comparison, retaining SHA-256 payload identities. It no longer
  hashes or stores unused CPU texture bytes for GPU-owned images. The identical
  capture-11 planner check matches all 1,623 operation layouts and same-provenance
  payloads in 1.51 seconds versus 9.25 seconds before, with 80,957,772 versus
  86,919,684 peak retained payload bytes. This is a CPU planning benchmark, not
  a gameplay FPS measurement. Evidence: `live-plan-snapshot-reuse.json` in the
  native renderer report directory.
- Earlier shared settings exposed Native (with compatibility fallback) and
  Compatibility, with a restart notice; nine GUI tests pass. Native rendering
  is now the runtime/config/GUI default after campaign rendering, movement,
  query publication, clean close and deliberate fallback checks. It uses the game's D3D boundaries, translated game
  shaders and a separate Vulkan raster path. Shared SDK device, command control
  and presentation services remain; this is not recovered original engine
  source. That earlier fallback policy has been replaced by the exclusive
  Native/Xenia modes described above.
- FSR 3.1 integration is **not complete**. The pinned FidelityFX SDK now builds
  a native Linux Vulkan library, and a native API query reports the 3.1.4
  upscaler provider. Build fixes preserve shader permutation arguments and
  handle this workspace's spaces/parentheses and Wine compiler dependencies.
  The normal game still uses FSR 1. Scene depth is identified in a saved GPU
  capture, but motion-vector encoding/coverage and camera jitter remain
  unverified; temporal dispatch has not been enabled or validated. Evidence:
  `reports/rendering/fsr31/assessment.json` and `sdk-build.patch` in that folder.
  A source review of Unleashed Recompiled confirms it hooks guest graphics
  calls above Xbox GPU command generation, with Hedgehog-specific layouts and
  asset-loading integration. Reusing its approach requires Gears-specific
  replacements. An opt-in native renderer prototype is now integrated, but
  reference GPU control/publication still runs; the original assessment is in
  `reports/rendering/unleashed-renderer-assessment.json`.
- Native Wayland presentation, fractional-scale 4K output and native HDR10
  presentation are implemented. The renderer checks the current output's active
  color description and 10-bit PQ Vulkan surface support; the standalone editor
  probes independently. Missing capability disables HDR and uses SDR, with
  Xwayland as the launcher's fallback. Actual runs selected 3840×2160 HDR10 on
  DP-2, and an unavailable Wayland connection successfully fell back to Xwayland
  SDR. HDR is highlight expansion from the completed SDR image, not recovery of
  pre-tonemap engine lighting. The shared transfer shader passed 109 GPU samples.
- Settings Apply now uses a live file watcher and runtime acknowledgements for
  reviewed presentation settings. Other settings produce a restart notice;
  reverting them clears it. The standalone editor and in-game Guide/F4 menu
  share the schema and file. The shader loader is a plain red bar with white
  text. Native Wayland now repaints overlays independently of the occluded GTK
  parent's frame callbacks, fixing invisible menus and loading progress.
- The in-game menu uses explicit controller navigation: A enters options,
  up/down skips disabled options, left/right changes values, B returns to the
  category list (B again closes), X applies and Y reloads. Categories remember
  their selection and focus scrolls into view. Tooltip outlines are red. The
  standalone editor notes the Guide/F4 shortcut. Current focused validation:
  444 C++ assertions in 15 cases, eight settings GUI tests and the codegen audit
  passed. Evidence and original-source backups: `reports/rendering/native-hdr/`.
- Rare long-freeze investigation: the user reported mostly smooth gameplay after
  query batching, interrupted by occasional severe freezes. Both GPU frame logs
  and MangoHud captured six 571–1,653 ms frames. Each issued 1,638–3,595 full
  shader-export readbacks; the five frames meeting the 3D-query filter had no
  overlapping shader compilation. The working Xenia reference explicitly used
  `readback_memexport=false`, whereas the SDK defaults to true with a buffered
  path that falls back to full synchronous copies. The game config now disables
  these optional CPU copies and the Advanced GUI exposes both readback controls.
  GPU exports and their synchronization still execute. This is a config change;
  the native binary is unchanged. Four GUI tests and four config assertions
  passed. The diagnostic run copies the preceding game/Mesa caches and records
  frame times from startup. Longer gameplay and scene compatibility validation
  remain pending. Run: `reports/performance/reproducible-spot/no-memexport-readback-1/`;
  evidence and rollback patch: `reports/performance/memexport-readback/`.
- Visibility-query submissions are now grouped at normal submission boundaries
  when asynchronous queries are enabled. Previously every query still ended its
  own Vulkan submission. Pending results remain fence-gated; guest memory waits,
  command-thread idle and query-index reuse flush and await the pending batch.
  `vulkan_batch_occlusion_queries` is enabled in the config and Advanced GUI tab
  and can be disabled for comparisons. Diagnostic logs now separate deferred
  command replay and queue submission from total submission time. Release build,
  403 assertions in 12 focused C++ cases, four GUI tests and the codegen audit
  passed. The new run copies both caches from the preceding run; only query
  batching differs in the settings. Initial gameplay uses about four submissions
  for 70–90 queries, with total submission time commonly below 1 ms. Fence waits
  remain and occasional spikes are still present, so this is not proof of a
  stutter-free result. Live recording: `reports/performance/reproducible-spot/query-batching-1/`;
  source/binary backups and measurements: `reports/performance/query-batching/`.
- First-use shader investigation: a fresh-cache run confirmed that translation
  still ran on the GPU command thread despite asynchronous pipeline compilation.
  One 123 ms frame overlapped about 96 ms of translation; the initial 296 ms
  3D frame overlapped 219 ms. Translation now uses the existing pipeline worker
  pool, with per-shader serialization for shared bindings and acquire/release
  publication of complete results. Layout caches stay on the GPU command thread.
  Memory-export draws prepare both translations and pipelines synchronously so
  their guest-visible writes are preserved. The new option is enabled in the
  config and Shaders GUI tab. Release build, 398 assertions in 11 focused C++
  cases, four settings tests, and codegen audit passed. A new empty-cache run
  reached gameplay with 860 of 861 translations on workers and only 1.897 ms
  of GPU-thread translation. The screenshot renders the checkpoint, with no
  shader/pipeline failures logged. First-minute frame tails are not consistently
  better across these different play routes; GPU submission/wait spikes remain.
  The user reported that overall stutter felt about the same. Broader rendering
  checks remain pending.
  Baseline: `reports/performance/reproducible-spot/cold-first-use-1/`;
  candidate: `reports/performance/reproducible-spot/cold-async-translation-1/`;
  source/binary backups and patch: `reports/performance/async-translation/`.
- GPU visibility queries now retire asynchronously. Instrumentation confirmed
  that the old path waited for the entire GPU queue after every query. In the
  uncapped baseline, frames with at least 500 draw packets averaged 58.3
  blocking fence waits and 7.66 ms inside those waits. The asynchronous run
  averaged 1.31 waits and 0.59 ms. Submission time increased from 1.24 to
  4.93 ms, so some backpressure moved into the driver; the fence-time reduction
  is not equivalent to an FPS gain. Scene paths and pauses differ between runs.
  Real results remain pending until completion. Query-index reuse checks the
  previous submission, reused guest report addresses discard stale results,
  and results occupy separate non-coherent memory atoms. Command-thread idle
  and memory waits drain pending batches so guest polling can make progress.
  `vulkan_async_occlusion_queries` defaults to true and is in the config/GUI.
  The Release build, 169 assertions across nine focused C++ cases, four GUI
  persistence tests, and codegen audit passed. Live play renders and progresses;
  one heavy combat capture still showed 46 FPS with 99% reported GPU use.
  Full-campaign correctness and locked 60 FPS remain unverified. The final
  diagnostic run restores the saved 60 FPS maximum. Timings, profiles, captures,
  and the incremental source patch are in `reports/performance/gpu-waits/`.
- Gameplay CPU overhead: POSIX multi-object and alertable waits now preserve
  fractional-millisecond deadlines instead of truncating them into busy polls.
  The audio worker fell from about 78% of one core in the initial sample to
  0.34% in the subsequent run. A regression reproduced the spin before the
  fix: 100 short waits used 99.8 ms of CPU; afterward they used 0.93 ms of
  CPU over 105.8 ms elapsed. Timeout and event-consumption checks also pass.
  Per-function guest tracing checks are now compiled out of normal builds;
  `GOW3_ENABLE_GUEST_TRACE=ON` retains the diagnostic build option.
  Release build and 128 focused assertions in four cases passed.
  Gameplay is rendering and audio diagnostics reported no starvation in the
  measured interval. Heavy sections still averaged roughly 49–56 FPS in
  selected 30-second windows: stable 60 FPS is **not established**. These
  samples cover different scenes, so they are not a controlled FPS comparison.
  That CPU profile motivated the GPU query investigation described above.
  Evidence and incremental patches: `reports/performance/gameplay-optimization/`.
- Graphics settings now load from `config/gow3.toml`; `tools/settings.sh`
  provides an independent Qt editor. Default maximum: 60 FPS, VSync off,
  asynchronous compilation on, all detected logical CPU threads available.
  Custom, monitor-refresh and unlimited host caps are supported. The FPS
  unlock and host cap are separate; neither forces a minimum frame rate.
- Startup movie pacing: found a 25 ms idle-render tick threshold at
  `0x823F4148`, using shared float `0.025` at `0x820CDB5C`. A guarded native
  hook changes only the loaded threshold to 1 ms. The video clock and shared
  constant are unchanged. Two full 643-frame logo runs each measured a
  33 ms median, 35 ms 95th percentile and 36 ms maximum; before the fix,
  the median was 25 ms and the 95th percentile/maximum were 51 ms.
  Both corrected runs took 21.401 seconds between first/last frames, as
  expected for 643 frames at 30 FPS. MangoHud captures confirm the graph.
  This increases polling of other idle-render tickables too; broad gameplay
  regression coverage is still incomplete.
  Three complete attract-movie loops also rendered all 4,488 frames, with
  33 ms medians, 35 ms 95th percentiles, and 37–38 ms maxima.
- FSR 1 spatial scaling and CAS are compiled into the Vulkan presenter and
  exposed in the editor. FSR 4 is **not implemented**. Fullscreen validation
  on this desktop produced a 3072×1728 Xwayland surface, reflecting desktop
  scaling; true 3840×2160 FSR output has not been validated here.
  Monitor-refresh mode read 119.805 Hz; VSync-off selected Vulkan immediate
  presentation mode. Config file defaults remain at a 60 FPS maximum.
- Settings/frame-limiter/config regression checks: 140 assertions across
  27 C++ cases and 4 Python GUI/persistence cases passed. Native build and
  codegen audit passed. Incremental SDK patch, replay verification, source
  hashes, movie timing and screenshots are in `reports/settings/`.

- ISO SHA-256:
  `c4345f21dff32924cc9d131fb2528d03b2d242a49ee1b230ecf9dc4f22d0978e`.
- XEX SHA-256:
  `4e5d00b8f584f42951344b26a2ae8292bb30c7634b1a75cec0c70a80b3f8f790`.
- Title ID `4D5308AB`, entry point `82CE8EB0`, image base `82000000`.
- 2,592 extracted files, 7,927,812,868 bytes.
- 115 generated C++ translation units with 56,675 function definitions.
  These counts include speculative discoveries; they are not a completeness
  measurement. No generated `REX_UNIMPLEMENTED` sites or unresolved direct
  call/branch diagnostics remain. There are 108 switch-default traps.
- `build/bin/gow3` is an x86-64 Linux ELF executable. Its runtime dependencies
  resolve on this machine. It launched with Vulkan on the Radeon RX 9070 XT,
  detected the DualSense Edge controller, and displayed the title screen.
- A fresh offline campaign now loads and renders the prison cinematic and
  gameplay, and writes a checkpoint naming `sp_nightmare_p`. The user
  confirmed that the scene renders. Black patches on Anya's face in the
  opening cinematic are also fixed and user-confirmed. Campaign completion,
  general save/load correctness, audio, and other modes remain
  unverified.
- Ghidra exported 41,369 recognized functions: 33,892 without emitted warnings
  and 7,477 with warnings. None has been individually validated as a complete
  source replacement.

## Changes from the adjacent workspace

All work is local to this directory; adjacent projects were read, not edited.
`vendor/rexglue-sdk/` preserves the earlier modified SDK source, including
uncommitted files. Its base commit, dependency commits, and inherited tracked
patch are recorded under `reports/rexglue-*`.

This session corrected:

1. SDK dependency checks to accept populated source snapshots without `.git`.
2. Missing CMake `VERBATIM` quoting in shader-tool generation commands, which
   failed in this folder's path containing spaces and parentheses.
3. Missing host build include paths and floating-point compiler settings.
4. Branch classification during code emission. Looking up an instruction's
   owner could select an overlapping speculative function or no function.
   Using the known function being emitted removes nine erroneous fatal
   branches in `sub_8297CCF0`. The regression test failed before the fix and
   passes all six assertions afterward.
5. Duplicate linkage of the `rexcore` object library into SDK executables
   already linked to `librexruntime.so`. This caused duplicate initialization
   and a `TimerQueue` destruction crash. Tests now exit normally.
6. A missing guest tracing include in the PPC test template.
7. An outdated `vmaddfp` test expectation. Exact rational arithmetic and the
   local Xenia Canary fixture both select `0x4013d70b`; the evidence is in
   `reports/vmaddfp-rounding.json`. The runtime implementation was unchanged.

## Validation and unresolved work

The full SDK unit suite must be run through CTest, which runs each case in a
separate process. Running all cases together exposes incompatible memory
fixtures. The CTest run contains 250 cases: 236 passed, nine failed, and five
skipped. The failures are four FILETIME/calendar cases, four outdated template
fixtures, and one CMake reference migration case. Logs retain every failure.

The instruction suite passes all 1,478 cases and 5,805 assertions; its exact
outcome is in `logs/ppc-tests.log`. Passing these fixtures does not prove that
all game behavior is correct. The local SDK changes are saved as
`reports/rexglue-linux-changes.patch`; source and built artifact hashes are
also recorded under `reports/`.

Fresh RenderDoc captures establish that the dark campaign is missing its
environment geometry. The working local Xenia build submits 849 draws with
more than six indices, versus 35 in the native frame. Native file-open tracing
never requests the prison map or the campaign movie. Its checkpoint names
the empty `GearGame_P` world; Xenia's names `sp_nightmare_p`. See
`reports/rendering/findings.json` and `checkpoint-comparison.json`.

The earlier HDR hypothesis from the adjacent workspace is not supported by
these fresh captures. The old `4D5308AB_31356.xtr` also contains material
textures whose RGB data is already zero before GPU upload.

A Linux runtime bug opened files requested for reading and writing as
write-only (`O_RDONLY` is zero and cannot be ORed with `O_WRONLY` to obtain
`O_RDWR`). The corrected access-mode selection passes 54 regression
assertions; five of the six access-mask variants failed before the fix.
With this fix, an isolated copy of the working Xenia checkpoint is recognized
and requests the prison map and its sublevels. That exposed an unregistered
indirect function, `0x8265B260`; its verified 0x28-byte boundary was added to
the recompilation configuration and the native executable rebuilt.

A fresh campaign still failed because its local session requires socket
operations even without Xbox Live. The launcher's forced `network_enabled=false`
caused the engine to skip chapter loading with `Error initializing network
layer.` After enabling local sockets, Xbox socket option/ioctl constants and
scalar byte order needed translation to Linux values. `getsockopt` now reads
the actual socket rather than returning success without output. Linux also
rejected ports 1000–1019 with `EACCES`; low nonzero guest bind ports now map
to port + 10000, as in the local Xenia source. Port zero remains ephemeral.
No Xbox Live service or online sign-in was implemented.

The file and socket regressions pass 86 assertions in two test cases
(`logs/rendering/runtime-fixes-tests.log`). A campaign begun with an empty
save root now requests `SP_Nightmare_P`, plays its cinematic, renders the
prison during gameplay, and creates a 26,375-byte `sp_nightmare_p` checkpoint.
The new native gameplay capture contains 585 geometry draws above six
indices and 202 textures; camera positions differ from the reference, so
exact draw counts are not expected to match. Original saves remain intact;
validation uses `reports/rendering/native-offline-validation`. Older saves
that name `GearGame_P` require starting a new campaign.

The facial artifact was caused by a vertex-buffer residency cache that
skipped dirty-page checks when fetch addresses and sizes stayed unchanged.
CPU-updated vertex data could remain stale on the GPU. Each draw now calls
`SharedMemory::RequestRange` for its vertex fetches; existing dirty-page
tracking avoids uploading unchanged data. The redundant residency cache and
its invalidation bookkeeping were removed. This matches the cache removal
in local Xenia Canary commit `c2674b19d`, without importing unrelated changes.

Repeated live cinematic captures and the user confirm that the black patches
are gone. Earlier tests disabling dynamic rendering and serializing every
draw did not fix the artifact; those experimental settings are not enabled.
The original broken RenderDoc frame replayed cleanly, so live screenshots
and original capture thumbnails were used for the visual check. Evidence is
under `reports/rendering/movie-vertex-sequence/`; the final cleaned build's
captures are under `reports/rendering/movie-final-sequence/`.

The incremental runtime patch is `reports/rendering/runtime-fixes.patch`.
The GPU fix is `reports/rendering/vertex-residency-fix.patch`. Both use the
recorded SDK source manifest as their baseline. The final build and codegen
audit passed, as did 86 focused regression assertions against the final
runtime (`logs/rendering/runtime-fixes-tests-final.log`). The build retains
existing compiler warnings; no full-campaign correctness claim is made.

Full source recovery also requires better function discovery, type/class
reconstruction, and replacement-by-replacement behavior checks. The
pseudocode export alone cannot be compiled into a faithful port.

## Normal launches without RenderDoc — 2026-09-30

The inherited `RenderDocAPI::CreateIfConnected` called the ordinary dynamic
library loader, which loaded RenderDoc even when it was only installed and
not explicitly attached. Detection now takes a reference to an already loaded
library only (`RTLD_NOLOAD` on Linux; `GetModuleHandleExW` on Windows). Normal
launches no longer attach the capture tool or auto-enable its GPU markers.
The native build remains Release; diagnostic logging remains available.

The regression failed before the fix and passes both assertions afterward;
see `logs/rendering/renderdoc-regression-{before,after}.log`. The patch is
`reports/rendering/renderdoc-opt-in-fix.patch`, applied after the runtime fix
patch (its test CMake changes build on the earlier file/socket additions).
Linux is built and tested; the corresponding Windows implementation has not
been compiled in this workspace.

A normal live launch was checked in `/proc`: neither RenderDoc nor the
Vulkan validation layer was mapped, and no RenderDoc API or automatic GPU
debug-marker initialization was logged. The visible game menu has no capture
overlay. Evidence: `reports/rendering/no-capture-validation.{json,png}`.

## Crash after the prison cinematic — 2026-09-30

Two runs aborted when `sub_82A4B578` called the unregistered guest function
`0x829AD8B8` through the vtable entry at `0x821DFA00`. The original executable
contains a 107-instruction leaf function ending in `blr` at `0x829ADA60`.
The independent XenonRecomp export identifies the same function. Its verified
0x1AC-byte boundary is now in `config/function-boundaries.toml`, and regeneration
adds the native implementation and function-table registration.

The Release build and codegen audit passed (56,675 functions; no known
unimplemented-instruction or unresolved direct-call/branch sites). A live test
using a copied checkpoint passed the formerly crashing transition and reached
the outdoor combat scene. The function-state trace recorded 16 entries from
the same `0x82A4B5EC` caller without an abort. GPU capture remained disabled.
Your original save was not changed; a backup and isolated validation copy are
under `reports/rendering/`. See `post-cutscene-crash.json`,
`post-cutscene-after.png`, and `logs/rendering/post-cutscene-function-calls.csv`.
This verifies the reported crash point, not completion of the campaign.

## Audio crackle — 2026-09-30

A recording of the running game's output contained 1,438 samples above full
scale, with a peak of 1.445703. Neither recording contained silent gaps, and
PipeWire reported zero errors in the sampled intervals. This supports clipping
as a cause of the reported crackle; it does not rule out intermittent starvation
in other scenes.

The SDL backend requested six channels before checking the output layout.
PulseAudio accepted that request even though the physical sink is stereo,
bypassing the runtime's normalized stereo downmix. A device probe reproduced
the change from two preferred channels to six after opening. The backend now
checks the preferred layout before opening, selects its existing normalized
stereo converter for stereo output, and retains six channels for surround.
Optional `--audio_diagnostics=true` reports starvation and sample levels every
five seconds; it is disabled by default.

The stereo negotiation regression failed before the fix. Afterward, stereo,
surround, and full-scale downmix tests pass 1,037 assertions in two cases.
The Release game was rebuilt, and a probe using its actual audio backend
confirmed two-channel 48 kHz output through this machine's PulseAudio device.
The existing live game was left running with its older loaded runtime, so
audible confirmation of the fix awaits a restart. Its latest campaign saves
are in `reports/rendering/post-cutscene-validation`. Shader-compilation stutter
is a separate unresolved issue; buffering and renderer settings were not changed.

Evidence and the incremental patch are `reports/rendering/audio-crackle.json`
and `reports/rendering/audio-stereo-fix.patch`. Apply the patch after the earlier
runtime, vertex residency, and RenderDoc patches. Tests use SDL's opt-in dummy
audio backend, compiled only when SDK tests are enabled.

## Stutter, startup preparation, and frame-rate update (2026-09-30)

The user compared Xenia Canary with an empty emulator cache and a separate,
initially empty Mesa cache, and reported no perceived stutter. The binary matches
the adjacent `xenia-canary` build by SHA-256. Its input caches and saves were
preserved. Evidence is in `reports/performance/xenia-cold/`.

The native Vulkan backend queued asynchronous pipeline work but still drained
the queue on the GPU thread and waited for workers at every submission. It also
suppressed presentation of any frame with pending shader draws. Both behaviors
caused visible stalls. Async submissions now leave work to the workers, and
incomplete frames are presented by default. Pending draws are omitted until
their real pipeline is published; no unused discard pipeline is synchronously
compiled. The publication order was corrected to read readiness before handles.
Shader translation and other resource setup still happen on the GPU thread.

The launcher now enables async compilation and uses all detected logical CPU
threads (`vulkan_pipeline_creation_threads=-1`, 16 here). With both caches empty,
the corrected 30 FPS run created 1,473 pipelines on workers and zero on the GPU
thread, with no failed draws or pipeline creations. The user reported “much
better” with occasional stutters instead of constant stutter. This is an
improvement, not proof that all stutter is eliminated. Playthroughs differed;
whole-run percentile comparisons include menus, loading, and different scenes.

Startup now translates and compiles known cached pipelines while a responsive
UI shows phase, completed count, total, and a progress bar. The guest remains
suspended until preparation completes. The normal cache merges 2,472 shaders
and 3,130 pipeline descriptions discovered in our runs; it does not cover all
game content. All record hashes and shader references were validated before
writing. Original caches and timestamped destination backups remain available.
The empty-Mesa startup compiled all 3,130 pipelines successfully in about 14
seconds, with both progress phases captured in `60fps-preload/startup-*.png`.

The version-specific frame-rate hooks are generated from
`config/gameplay-hooks.toml`, following the illusion/boma Xenia Canary Unlock FPS
patch for module EF4AD5E9DD1982ED. Original instructions are checked before
activation; the XEX is unchanged. `gow3_60fps` defaults true; restart with false
to restore the original cap. Gameplay now exceeds 30 FPS, with roughly 40–60 FPS
observed in the tested ship scenes. A fire-scene screenshot shows 53 FPS; a later
30-second sample has a 24 ms median. Locked 60 FPS and full campaign correctness
are not established. Prerecorded movies may remain at their original frame rate.

The latest user-played checkpoint from `cold-async-parity/user` was copied into
the normal `user/` save root. The prior normal root is backed up under
`reports/performance/60fps-preload/previous-normal-user`, with original source
roots retained. The current monitored 60 FPS run uses `user/` and an isolated
Mesa cache. Normal launch remains `./tools/run-linux.sh --mnk_mode=true`.

Validation: Release build and codegen audit passed (56,675 translated functions,
115 units, zero known unimplemented instructions or unresolved direct branches
and calls; 108 switch traps remain). The concurrent progress test passed 8
assertions, including failed jobs and empty phase reset. Cache merge checks pass
for idempotence, duplicate inputs, corrupt records, and truncation without
modifying the destination on rejection. Gameplay and startup progress were
visually inspected; the sampled campaign window had zero failed draws and zero
pipeline creation failures. The 30 FPS run had no audio buffer starvation, but
seven samples marginally exceeded full scale (peak 1.034363); audible audio
correctness is not fully established. The broader pre-existing SDK test failures
were not reclassified or rerun for these targeted changes.

The incremental SDK patch is `reports/performance/async-preload-sdk.patch`,
applied after the previous correctness/audio patches; replay matched all eight
changed files byte-for-byte. Build/source hashes were refreshed. Diagnostics,
comparison protocols, merge provenance, and validation outputs are under
`reports/performance/` and `logs/performance/`.

2026-10-05: Frame ceilings and broader early-depth validation

Normal config now selects custom60FPS with VSyncfalse; diagnostics default to unlimited0 with VSyncfalse. The unchanged SDK FrameLimiter only sleeps before its real-time deadline; late frames add no wait or accumulated debt. Source audits are retained under early-depth-catalog-01. All upcoming performance trials explicitly select cap0. The earlier promoted-default cap60 health check remains a health check, not an unlimited performance comparison.

The current main090d35753608ba1741ab091a4c61cc2d31a50e372eadaf78be0ccfc894f73628 adds a separately keyed default-OFF catalogue for two more pinned materials; runtime5e942bc1 remains unchanged. A live generic-PSO residency lookup initially used descriptor format fields before assignment. The narrow fix reads actual attachment formats and preserves all source/residency guards. Actual Radeon natural replay now selects320extra draws (240a3/8055); all172images/full10MiB and both wrapped-query results match the V1 baseline. Limitzero refuses320missing V2PSOs and keeps the native original path exact. Current V1 also passes newly repeated original-Native image/memory/query/residency comparisons.

The controlled catalogue Scene run at capacity64 passed mixed-source reuse, fence continuity, strong reads and memory reinitialization in both paired arms, then stopped because its arbitrarily chosen captured query draw returned zero samples even in the baseline. That strict failed witness and outputs are preserved. Source review does not substitute actual success; a separately guaranteed positive query witness is being prepared. No V2 performance trial or default promotion has occurred. The>=60FPS/stable-action-frametime goal remains unmet.

2026-10-05: Completed early-depth catalogue correctness and uncapped timing

The corrected controlled Scene fixture passes all eight exact image/full-memory/query comparisons at capacities64 and128, including mixed-source reuse, fence continuity, invalidation, pressure, and owned pending teardown. The earlier vacuous-query failure is preserved. The catalogue remains OFF: matched cap0/VSyncfalse forward comparison21.750→22.405ms (+3.01%) and reverse22.070→22.485ms (+1.88%). It selected47additional draws/frame but required about5additional full-screen depth conversions/frame. Neither pair had a selected-window frame over50ms; this stationary result does not establish action frametime stability or60FPS. See early-depth-catalog-01/default-off-decision.json. The existing frame-limiter test passed7assertions, including no added deadline for a40ms frame under cap60. Normal settings remain custom60/VSyncfalse; every diagnostic performance run remains uncapped.

2026-10-05: Resolve descriptor reuse enabled after repeated uncapped improvement

The Native renderer now reuses resolve descriptor sets after their GPU fences retire. Each resolve memory owns at most 128 cached sets; synchronous work and capacity pressure preserve the original allocation path. Shader code, resource bindings, barriers and submitted work are unchanged. Controlled Radeon lifetime/pressure tests passed 2,232 checks at capacities 64 and 128. Natural and wrapped-query replays match all 172 images and full 10 MiB packed memory exactly; query samples also match.

Both uncapped, VSync-off stationary comparisons improved: 21.69 to 19.86 ms (8.44% lower frame time), and reverse order 22.18 to 19.825 ms (10.62%). This is about 45–46 to 50 FPS. Median window p95 improved in both pairs; selected stationary windows had no frames above 50 ms. The option is now enabled by default. Promoted main d832279558466e59ed20ba53626d6152c68afb1f5e8fc6a89b193fa0277f63b9 uses unchanged runtime 5e942bc1e5443dae8d5f1d6ae345caabbb19dfa3e610dbbc08a7e9f7cc149a6b.

The uncapped default-enabled action run completed aiming, firing, reloading, movement and camera input, stayed Native with no renderer fallback, retired all 365,135 queries and closed cleanly. There were no pool failures or pressure fallbacks. Action frame spikes remain; this health run is not a matched action performance comparison. Stable 60 FPS remains unmet. Evidence: performance-60fps/resolve-descriptor-pool-01, including both quiet-pair reports and default-enabled-action-health-proof.json. Normal settings remain a 60 FPS ceiling with VSync off; all performance and health tests use cap 0.

2026-10-05: Owned resolve image-view experiment stays disabled

The optional load-only identity-view reuse passes 2,694 actual Radeon lifetime/format checks at capacities64/128 and two exact full-capture comparisons (all172images/full10MiB/query samples). Closed owned RGBA32/R32/RG32 outputs use an equivalent existing view; store/stencil paths remain explicit. Uncapped paired medians improve only19.945→19.830ms (0.577%) and reverse19.830→19.810ms (0.101%). Reverse median-windowp95 changes20.99→21.00ms. The forward baseline had unexplained severe tail stalls; those remain in the report and are not attributed to the optimization. An earlier run with observed external compiler load was excluded before comparison. No convincing performance benefit is established, so the default remains false. Descriptor reuse remains true; steady gameplay remains about50FPS. Evidence: performance-60fps/resolve-identity-view-01/default-off-decision.json.

2026-10-05: Action cache census and shipping diagnostic overhead

Added a default-OFF bounded texture eviction/reload census and an optional dormant-entry budget (default512, maximum2048), retaining128MiB/120-frame/binding/fence policies. CPU tests passed158 diagnostic checks and11,057 independent cache-history checks; natural512 and query-wrapped2048 replays preserve all172images/full10MiB/queries. Both uncapped scripted action runs close cleanly, stay Native and retire all queries with no metadata/overflow failures. Larger entry budget is NOT promoted: recent exact reloads503→561 are mostly age/byte causes; entry-only reloads1→0. Both still exceed250ms. The worst512 action window has at most6.331ms summed texture creation in any frame, versus a150.790ms shader-input event and20PSO creations totaling492.961ms across the nearly corresponding120-frame sample. These inclusive measurements overlap; they are not additive GPU costs.

A separate same-build uncapped stationary comparison of the normal diagnostic flags shows20.04ms/49.90FPS instrumented versus19.565ms/51.11FPS lean (2.37% lower median time), windowp9521.06→20.535ms, zero>50ms both. This measures diagnostic overhead, not a new shipping-code gain; normal defaults already disable those flags. Stable60FPS remains unmet. Current main3311e907194f3ef8ff927a8b2c75d9206703131a3edad0e9306d131fdb79a04c, runtime5e942bc1 unchanged. Evidence: cache-churn-census-01/action-comparison-summary.json and shipping-diagnostics-overhead-01/comparison-01.json.

The initial shader/PSO investigation finds839 valid observed recipes in the existing seed, but current48MiB module admission selects730 and silently omits109. All loaded sources are compiled before residency selection, so this omission explains possible later PSO compilation, not by itself the separate150ms source-compiler wait. Exporting newly learned action sources/recipes and a subsequent startup-coverage test are underway. No startup residency or pipeline-cache behavior has yet changed.

2026-10-05: Learned action shader/PSO coverage remains incomplete

The completed uncapped, VSync-off learn/warm coverage runs use identical main3311e907/runtime5e942bc1, settings, action input and non-cache user seed; the second deliberately loads the first exported Native cache. Both close cleanly, stay Native and retire/write all399,049 and413,608 real queries with zero pending tickets or visibility mismatches. Learn exports4 additional shader-cache files and3 recipe records (839→842); warm then exports3 more recipe records (842→845). This is a cache-coverage experiment, not a renderer performance A/B.

The existing48MiB startup-selection model still admits the same730 recipes/745 original modules after learning, omitting112 recipes including all3 newly learned ones. A source/hash/header/file-size model admits all842 learned recipes within the hard bound using882 original modules/56.466MiB; that alternative has not been measured by these runs. Logged runtime shader-provider maxima remain94.331/64.944ms (3/1 calls at least10ms), while24/19 runtime PSO creations total938.255/566.274ms (21/12 at least10ms). Shader-module creation maxima are only0.029/0.022ms. Complete120-frame windows bounded within the existing action schedule still contain293.68/231.31ms peaks and5/2 frames above100ms. These overlapping host timers and window spikes do not establish exclusive causation or an FPS gain. Both background preload reports preserve124 nonfatal failed jobs; Native execution remains healthy. The findings support testing startup PSO admission rather than assuming warmed source files eliminate all first-use stalls. Evidence: performance-60fps/action-prewarm-coverage-01/coverage-analysis.json.

2026-10-05: Overflow startup prewarm enabled; compact R32 remains disabled

Overflow pipeline prewarm is now enabled by default and built. Two clean uncapped, VSync-off action pairs confirm preparation of all115 known omitted PSOs while keeping the original resident shader selection. The first pair reduces logged runtime PSO creation725.53→0ms and action maximum253.53→48.54ms. The opposite-order confirmation reduces PSO creation575.77→192.33ms but encounters fresh sources and a318.36ms action spike; it exports4 new recipes beyond the845-recipe seed. These differing action workloads do not establish a steady renderer FPS gain. The stationary control is19.64→19.55ms, about51FPS, with no observed median/p95 regression. Current-device replay checks preserve all173 artifacts, including full packed memory and query results. ON02 is excluded in full because two externalcc1 samples were recorded. The promoted main identity is archived by the root separately; the tested candidate was606e2571 with runtime5e942bc1.

The isolated compact R32 trial passes current-device artifact parity and executes11,886 R32 loads over the selected1,320-frame counter extent, with RG32 support/loads zero. It records114.65GB of logical decoded-output bytes avoided, which is not a physical bandwidth measurement. Median19.64→19.61ms is negligible and windowp95 worsens20.62→20.70ms, so compact channels remain defaultOFF and no reverse trial is planned. Evidence: compact-resolve-isolated-01/compact-default-off-decision.json and overflow-pipeline-prewarm-01/comparison-01.json/comparison-03.json.

A new early-depth readonly-boundary retention candidate is integrated and built but remains defaultOFF and unvalidated; no gain is claimed. User caches remain untouched. All cited gameplay trials used cap0 and VSyncoff; stable60FPS remains unmet.

2026-10-05: Restore original per-texture filtering

User requested removing forced16x and always using the game samplers. Both Native and Xenia now enforce anisotropic_override=-1 at startup, including legacy config/CLI/environment values. The shared settings schema no longer exposes the override; both editors normalize saved legacy values, and the live watcher acknowledges the normalized value without a false restart notice. Only this saved configuration value changed. Release game/settings/unit builds passed, Python settings passed17, C++ configuration passed49 assertions in3 cases, and controller navigation passed with assertions enabled.

Two uncapped fixed-camera pairs using the previous identical build and frozen caches measured forced16x→original:20.51→19.30ms and reverse19.77→19.28ms, about2.5–5.9% lower median frame time; windowp95 improved in both. Original filtering reached approximately52FPS, so stable60FPS remains unmet. These are quality-setting comparisons, not pixel-exact optimizations or full-game guarantees. Reports:performance-60fps/anisotropic-original-01/comparison-01.json, comparison-02.json and game-default-decision.json. No other added Native quality pass was found active beyond HDR (disabled throughout these tests); Native already uses1× resolution/bilinear and no extra post-AA.

The early-depth readonly-boundary candidate remains defaultOFF. Its controlled GPU fixture and source peer audit are preserved, unapplied and unexecuted, under early-depth-unchanged-boundary-01/controlled-fixture-source for the next performance step.


2026-10-05: Current camera-angle investigation and rejected atlas crop

A current strict-Native, uncapped/VSync-off camera sweep reproduces the view-dependent drop with original game filtering. The warm run without GPU timestamp diagnostics measures about48.2FPS in the original room view,59.2FPS looking right and101.8FPS facing the floor. All selected shader/PSO creation, miss and eviction counts are zero. The instrumented original view has fewer draws than right (833 versus860/frame), but more transfer lists (103.3 versus76.0), submissions (75.5 versus67.3) and explicit drain waiting (4.892 versus3.444ms/frame). Inclusive GPU timers and CPU waits overlap and are not additive exclusive costs. This demonstrates a view-dependent rendering-work problem beyond repeated compilation, not a complete exclusive bottleneck attribution.

Two new API captures identify a1120x688 UNORM24 atlas at EDRAMbase1440: original6clears/5resolves/6consumer draws versus right1/1/1 each frame. Four original-only small resolves regenerate the whole sampled atlas; other buffers also add transfers. Depth/shadow-map semantics are inferred, not established by recovered UE3 function names. Current API captures omit frame-boundary EDRAM, so paired replay explicitly uses the same zero seed; the older weapon replay retains authenticated captured history. Evidence:performance-60fps/camera-angle-current-01/diagnosis.json, profile-01/camera-angle-analysis-v3.json, warm-lean-analysis.json and exact pass/source audits.

The new source-region restore experiment stays defaultOFF. All161original images/memory artifacts,161portable-stencil artifacts,129right artifacts and173weapon/query artifacts match. The actual Radeon crop fixture passes depth/stencil inside/outside-bit preservation, offset/border rectangles, same-image full repair, full10MiB unchanged memory and18scratch fence-owner witnesses. The standalone test target needed its existing JSON header dependency added. CPU model checks also pass. Actual original atlas full restorations14→12 plus8small crops save720384source pixels/frame, but total depth-restore passes31→37 and submission/transfer counts stay291/197 across two frames. Matched uncapped timing regresses19.32→20.80ms (+7.66%), p9520.34→21.94; ON has one58.64ms frame. No reverse trial is justified for this variant. The next source-level lead is fusing canonical conversion work; arbitrary partial packed clears would change untouched UNORM24 quantization and stencil. Evidence:performance-60fps/atlas-depth-region-01/default-off-decision.json and preserved comparison/fixture reports. No Native-to-Xenia fallback was introduced; stable60FPS remains unmet.


2026-10-05: Certified constant upload experiment rejected after reverse confirmation

Added a default-OFF exact-module-certified upload path for the a14c/3f77 null-pixel depth draw families. It preserves the full10816-byte allocation,2624shared bytes, mapped-range flush, original fences and all shader bytes, while writing only the certified176-byte vertex-float prefix. CPU footprint/mutation and byte-packing fixtures pass; actual Radeon capacities64/128 match all173artifacts including full10MiB and exact wrapped query results. Each full ON replay retires3330draws, selects1020 and omits8176320host float-store bytes, with no refusal.

The first uncapped matched pair improves19.50→19.085ms (-2.13%), but a current-build reverse-order confirmation regresses19.13→20.35ms (+6.38%), p9520.72→21.85. All reverse health, identity, source/cache and mechanism gates pass, so it is not excluded. Both selected reverse windows have zero frames above50ms. No repeatable gain is established; Runtime default remainsFALSE, as does the atlas crop. Current main3898c7a2be29f5afcc028ddc4064c79170caca2afdf3d0d4bbc807d368a3c4e2, runtime5e942bc1 unchanged. Ordinary game configuration/user caches were not changed. Evidence:performance-60fps/pinned-null-constants-01/default-off-decision.json. Camera-dependent steady drops and stable60FPS remain unresolved despite identifying extra atlas/transfer/wait work; the next supported lead is reducing entire canonical conversion passes, not assuming fewer written bytes or pixels yields a gain.


2026-10-05: Combined atlas conversion removal promoted after repeated timing

Implemented fused full-surface UNORM24 atlas depth-one clear with an immutable pre-encode depth/stencil receipt, and direct atlas resolves from an exact receipt or stale/absent packed source. Current D32 hosts, active queries, unsupported layouts and unsafe provenance retain the original Native path; no Xenia renderer fallback is introduced. The receipt preserves noncanonical host precision and later consumers, while packed direct resolves eliminate unnecessary intermediate D32 conversions. Shader/CPU fixtures, actual Radeon primitive and controlled Scene cases, and all original/right/portable-stencil/weapon replay artifacts pass, including full10MiB EDRAM, physical output and query results. The original two-frame capture reduces total depth restores31→17 and atlas restores14→0, with all161artifacts exact; submission/transfer counts are not claimed eliminated.

Fused clear alone regresses19.74→19.935ms and is not independently justified. The combined same-build uncapped/VSync-off pairs improve19.60→19.12ms (-2.45%) and reverse19.76→19.17ms (-2.99%). Windowp95 improves21.11→20.45ms and21.03→20.32ms; no selected frame exceeds50ms. Reverse maximum worsens23.56→26.06ms, so this is a modest typical-frame improvement, not a guarantee against spikes. Both atlas runtime defaults are promoted together; the user config/caches are unchanged. About52FPS in the slow view remains below60. Evidence: performance-60fps/direct-atlas-depth-resolves-01/default-on-decision.json and both comparison reports. Default build verification follows with the next diagnostic-only candidate.

The next default-OFF candidate normalizes primary FP24 depth and clears stencil directly from packed memory only when the host image is absent/stale. A raw stencil mask is incorrect because the original D32 restore clamps packed values above1; tests must also preserve future host depth bits. This candidate is integrated for validation, not yet qualified or enabled.


2026-10-06: Packed primary stencil clear qualifies for default enablement

The exact primary1280×720 FP24 stencil-only clear now normalizes packed depth and changes stencil in the requested rectangle, avoiding temporary depth images for clean absent/stale hosts. Depth above1 must clamp to0xf00000 across the entire surface; a raw stencil mask was rejected. Current/dirty hosts and active queries keep the original Native path, and existing host tokens remain stale. Actual Radeon64/128 fixtures each test every24-bit depth code through both stencil paths,38whole10MiB pairs,38original/futureD32/S8 pairs and76owner-fence witnesses. CPU68,030,480numeric/address and92,331guard checks pass. Eight controlled Scene cases pass at both capacities. The first Scene test exposed portable restore diagnostics being omitted; detailed reports now expose them with the actual requested flag. Rendering is unchanged by this report fix.

All161original/161portable/129right/173weapon artifacts are exact. Original primary restorations13→7 across2frames; right has no eligibleprimary stencil clears and stays7→7. A template wrongly expected selection in right; its failed report is preserved, and the unchanged checker qualifies right as a zero-selection control using the immutable source census. Weapon activequeries refuse all6eligible clears and retain exact results.

Both uncapped/VSync-off samebuild pairs pass fixed-window/cache/health gates:19.72→19.06ms(-3.35%) and reverse20.04→19.22ms(-4.09%). Windowp95 improves21.21→20.62 and21.37→20.445; maxima31.33→27.66 and29.75→22.34ms, zero>50ms allfourwindows. The candidate is approved for default enablement over the already-enabled atlas optimizations. These are separate paired comparisons, not additive FPS guarantees; the tested slowview remains about52FPS and stable60 is not met. Evidence:performance-60fps/packed-primary-stencil-clear-01/default-on-decision.json. Main default rebuild follows the next diagnostic-only color-copy integration.


2026-10-06: Direct primary color-copy bypass qualified but not enabled

A default-OFF exact fmt2 primary color→rawfetch54/descriptivefmt7 resolve path bypasses the redundant RGBA32F host identity copy for absent/stale packed sources. Current/dirty hosts and queries preserve the original Native path. Source review caught the54→7 identity alias before integration; correctedv2stages admit exactraw54. Actual GPU64/128 tests compare every1024RGB/all4alpha code, complete float images, unchangedfull10MiB and ownership across producer retirement/consumer submission. Eight Scene cases at bothcapacities include full/96×64 destination repair, full4MiB oracle, future generic readers/aliases/queries and a consequential packedhalf-up1 versus currenthostround-even0 refusal. CPU42guard checks and builds pass. All161original/129right/173weapon artifacts are exact; actual primary color copies6→0 original and2→0right over2frames.

The uncapped samebuild comparison shows20.41→20.37ms(-0.196%), p9521.67→21.61, p9922.41→22.63 and max27.95→28.14ms, no>50ms. This is not a convincing gain, so directcolor remainsFALSE with no reverse trial. The three prior atlas/primary-stencil defaults areTRUE and built (main86f65f21…, runtime51c873b3…). AbsoluteFPS varies across test series; do not add the prior percentages or present the current49FPS comparison as a guaranteed52FPS baseline. Stable60 remains unmet. Evidence:performance-60fps/direct-packed-color-resolves-01/default-off-decision.json.

Existingreports show four remaining640×360 FP24 stencil-clear restore chains across2frames in bothviews; a bounded secondary candidate is staged but not integrated. A separate source audit finds13×4KiB synchronous CPUuploads/frame in the slowview versus3right, but APIcaptures omit these livecoherence-generated writes, so contiguous cohort sizes are still unknown.


2026-10-06: Secondary packed stencil clear correct but slower; leave OFF

Integrated an exact640x360/base1440 FP24 stencil path preserving20,480padding bytes and atlas receipt invalidation. Main initial build needed its generated secondary SPIR-V header added to the explicit target source list; retry and standalone builds pass. CPU67,344,400numeric/address and152guard checks pass; actual Radeon64/128 primitive tests each pass1,844,103checks. Six controlled Scene cases pass in portable/export arms at bothcapacities, including consequential shared-atlas receipt invalidation and futureconsumer bit equality. All161original/129right/173weapon replay artifacts match; originaldepthrestores11->7 and right7->3,12transferstages removed ineach but queuedlistcounts unchanged.

Fixed samebuild uncapped/VSyncOFF pair passes allhealth/cache/source gates but regresses19.78->20.12ms(+1.72%), p9520.96->21.27,p9921.55->22.04,max24.11->25.15; no>50ms. Candidate remainsFALSE, no reverse justified. Three prior qualified defaults remainTRUE. Currentmainaf7ff9e3/runtimec327a3ea. Evidence:performance-60fps/packed-secondary-stencil-clear-01/default-off-decision.json.

DefaultOFF CPU-write prefix census is integrated separately. All98samples/97deltas conserve, rawlog matches, strictNative/queryretirement pass. Fullcamera parser correctly refuses missing complete timingwindows; metadata report preserves limitation. Originalhold8879..8999:1305singletons+332pairs=1969nonnullpages; later exact120frame intervals show9singletons+2pairs/frame=13pages/frame, but crosshold intervals cannot be assigned to cameraangles. There is no13pagecohort. Instrumented frame timings are excluded from performance conclusions. Next bounded leads: reusable checked-fence synchronous4KiB uploadbuffer (preserve submissions/waits), and fused canonical packedatlas decode into existing resolve output to remove full-source intermediate. Neither staged lead is enabled or qualified.60FPS remains unmet.


2026-10-06: Checked synchronous upload-owner reuse correct but not faster

Added defaultOFF exactaligned4KiB CPU upload-owner reuse. Private native checked-completion marker permits only live SharedQueue; rawuncheckedqueues, ordinaryinitialization, queued uploads and unsupportedwrites retain original behavior. Every originalsubmit/wait/version remains. Uncertain submit/wait poisons reuse and preventsunsafe destruction. Mainbuild passes. Existing transferfixturetarget required its bundledJSONincludepath; initialfailedbuild preserved,retrypasses. Actual Radeon64/128 fixtures compare96writes/control, eight retainedearlierimages, full64KiBbytes andversions, exactsubmission/waits, positive1allocation/95reuses andbypasscases. Bothrealafter-submit/before-wait exceptionprobes produce requiredpredeathmarker+exit85 aftertest-onlyfencecleanup.

Fullybound uncapped pair passesstrictNative/cache/load/query/mechanism checks. Median19.57->19.69ms(+0.61%), p9521.08->20.88,p9921.90->21.47,max28.57->27.66,zero>50ms. No convincingFPSuplift; remainsFALSE,no reverse. Game17b3d9a6/runtimec327a3ea. Evidence:performance-60fps/cpu-write-upload-reuse-01/default-off-decision.json. Onlythreepriorqualifiedatlas/primarydefaults remainON; stable60unmet. NextstagedMRTcandidate combinescanonicalpackedatlasdecodewithoriginalresolveoutput whilepreservingstencil/physicalstore; notyetintegrated/qualified.


2026-10-06: Fused packed atlas MRT resolve correct but not faster

Added a default-OFF path combining canonical packed atlas depth decoding and resolve into an R32 depth output plus a packet-owned stencil carrier. It retains the original physical store, precision, query guards, destination repair and submission boundaries. Actual Radeon tests at capacities64/128 pass all16,777,216 depth codes against the original GPU path and an independent integer oracle, five captured rectangles, all stencil values, complete memory comparisons and scratch-owner retirement checks. Eight controlled Scene cases pass with both stencil implementations, including consequential current-host/receipt precision refusals. All161original/129right/173weapon replay artifacts match. The original capture removes10full atlas decodes and10transfer stages; transfer-list counts remain unchanged.

The valid same-build uncapped/VSync-off pair measures18.865->18.95ms(+0.45%), p9519.985->20.49, p9920.665->21.59 and maximum25.76->28.09ms. Both selected windows have zero frames above50ms. No gain is established, so the switch remainsFALSE and no reverse trial of this unchanged variant is warranted. The three previously qualified defaults remainTRUE. Stable60FPS remains unmet. Evidence:performance-60fps/fused-packed-atlas-resolves-01/default-off-decision.json. Next investigation compares simultaneous CPU/GPU diagnostics while holding each camera view; no new rendering optimization is yet claimed.


2026-10-06: Camera-view stalls traced to completion waits

Recorded held original/right camera views using archived exact main/runtime ELFs, GPU timestamps and bounded CPU sampling. Original01 CPU data finalized after clean game exit but perf propagated SIGTERM from its sleep workload; it is explicitly partial. An attempted256-page perf buffer failed the system mapping limit and is excluded. The right capture used the working default buffer and finalized normally. Both useful records contain lost-event warnings, so no exact off-CPU fractions or calibrated CPU/GPU timeline are claimed. Source-only diagnostics now support clean close during profiling with a bounded lifetime. Exact native callchains show increased guest GetData polling in the slow view, not increased CPU draw-preparation share. GPU samples show distributed material costs rather than a single dominant shader. Reports:performance-60fps/camera-held-post-mrt-01.

Added default-OFF caller attribution for outer Runtime completion, without changing flush, submission or publication behavior. Same-build uncapped fixed-window diagnostics pass conservation, Native and query-retirement checks. Original/right means in ms per API frame: SDK command completion4.756/4.042; CPU-write pre-drain0.850/0.228; Runtime query pressure0.291/0.151. Total outer caller time5.897/4.422. Queue fence counters are nested subsets, not additional costs. SDK opcode counts are about4cache waits and9/8completion-marker drains per frame; frequency does not allocate SDK time by opcode. Instrumented frame medians20.29/18.355ms are observations, not shipping-performance or recoverable-gain claims. Evidence:performance-60fps/completion-causes-01/completion-cause-summary.json.

Re-evaluating the existing exact cache-only wait progress option on this current build. The real SDK decoder passes4518checks. The current GPU Scene fixture needed its existing shader-compiler link dependency restored; initial link failure is preserved and the corrected build passes. Actual GPU Scene tests pass cache-progress/full-fence alias visibility, exact query counts, readback and shutdown, alongside the existing suite. Timing qualification follows; cache-coherence-waits remains defaultFALSE.60FPS remains unmet.


2026-10-06: Current cache-only wait optimization remains OFF

Current CPU decoder and real GPU coherence tests pass. The valid same-build quiet uncapped comparison measures19.655->19.60ms(-0.28%), p9520.975->21.21, p9921.575->21.66, maximum26.42->38.88ms; neither selected window has frames above50ms. Actual submit-only admission is observed, but no useful FPS benefit is established. Keep gow3_native_cache_coherence_waits=FALSE; no reverse test of this unchanged variant. Evidence:performance-60fps/cache-coherence-waits-current-01/default-off-decision.json. The next diagnostic measures actual later accepted work following synchronous0x58 markers, plus SDK per-opcode completion time, before undertaking lifetime-safe asynchronous marker publication. No asynchronous marker behavior has been implemented.60FPS remains unmet.


2026-10-06: Completion-marker cost separated from cache and presentation waits

Added opt-in marker-following counters and per-opcode completion timing under the existing gpu_command_summary switch. The fixed-size census observes accepted Native work without submitting, polling or modifying marker stores; normal rendering has no new clocks or queue observations. Both shutdown paths clear the observer before device teardown. The helper passes69checks and the main build succeeds. Same-build original/right diagnostics close cleanly, remain strict Native and retire all exact queries.

Complete held-window SDK wall time (ms/reference frame):0x58 markers3.680/2.760,0x3C waits0.720/0.417,0x64 swap0.109/0.108. These include Runtime completion and overlap GPU execution; they are not additive or wholly recoverable. All12,921/12,565 selected marker windows admit no later accepted draw/transfer before the next currently mandatory SDK boundary, with no rejected/censored/overflow samples. Repeated paired0x58 packets do not close the census. Current no-pending packet boundaries may be conservative; source review identifies cache-provider handling as a separate probe before rejecting all asynchronous opportunities. Marker target lifetime and publication are not authenticated by these diagnostics. No asynchronous marker implementation or new performance default is claimed.

Instrumented complete-window medians19.295/16.950ms reproduce a camera-dependent difference but do not establish shipping performance or stable60FPS. Asynchronous presentation is parked because measured swap completion is small and removing its first wait would move it into Scene drains; future work must also preserve compute consumer barriers and frame-owner retirement. Reports:performance-60fps/marker-following-01/summary.json and swap-source-audit.json.


Marker/cache coupled probe: enabling the existing cache-progress provider while keeping synchronous0x58 stores exposes later accepted work in10,088of12,969 selected marker windows (10,076mixed,12draw-only;2,881zero). Counts conserve with no censor/overflow. This avoids prematurely dismissing the asynchronous lead: the earlier zero-work result depended on conservative no-pending0x3C boundaries. However NoPending returns before canonical-wait classification, so this observation does not prove all intervening waits can be crossed with pending markers. Ownership/free/reset/query-publication work remains necessary. The initial reused parser correctly rejected the changed expected-OFF configuration; a separately archived parameterized parser checks expected-ON explicitly and all checks pass. Neither cache progress nor asynchronous markers are enabled by default. Evidence:marker-following-01/coupled-probe-analysis-02.json.


2026-10-06: Queued CPU-write improvement fails reverse confirmation

Rebuilt current positive CPU policy, transfer and deferred-Scene fixtures; all pass on hardware, including48CPU checks, immutable queued page history, exact queries, report overlap and failure ownership. Current uncapped/VSyncOFF original-camera pair improves19.49->18.825ms(-3.41%) with betterp95/p99. The reversed pair instead measures19.105->19.39ms(+1.49%) withp9520.23->20.87,p9920.68->21.77 andmax21.95->28.76. Both pairs pass exact same-build/source/cache/input/Native/query/mechanism checks; all13eligible4KiB writes/frame actually queue inON versus13synchronous writes inOFF. No selected frame exceeds50ms. Screenshots occur outside the complete-window timing interval; reviewed first-pair room images show correct comparable rendering. No repeatable gain is established, so gow3_native_queue_cpu_memory_writes remainsFALSE. Evidence:performance-60fps/queued-cpu-writes-current-01/default-off-decision.json.

A separate defaultOFF PassTransfer graphics-descriptor pool is now integrated for validation. It extends the existing fence-retired backing pool to Resolve/RestoreDepth while retaining every view/framebuffer, binding, shader, barrier, draw and submit/wait. Source and focused ownership/output/failure fixtures passed independent review. No GPU execution or speedup is yet claimed for this new candidate.60FPS remains unmet.


2026-10-06: PassTransfer descriptors qualified for timing

Actual hardware descriptor fixtures pass at64/128 capacities:136full images,218484color/D32/stencil bytes,128simultaneously held cached sets plus4uncached owners, oldest-only reuse, exact submissions/waits and accepted-prefix exception. Both real uncertainty probes retain owners, refuse reuse and terminate with expected85 after test-only GPU completion. Initial full-capture comparison correctly rejected a disabled baseline: primary/secondary shader CMake pins still referenced the old PassTransfer file. The two hash-only reviewed corrections restore both modules without changing raster math or shaders; initial builds/replays/failure remain archived.

Corrected main/fixture builds and all four GPU tests pass. Original/right/weapon comparisons pass all161/129/173images and full memory/query checks with actual117/89/144pool hits,3/3/4backing allocations, no outstanding leases and unchanged queue/transfer/restoration counts. Camera replays still use explicitly matched zero EDRAM because captured initial history is absent; weapon history is captured. The candidate remains defaultFALSE pending quiet timings. No new FPS gain is yet claimed. Currentmain e2107dec/runtime4a7896ff. Evidence:performance-60fps/pass-transfer-descriptor-pool-01/correctness-summary-02.json.


2026-10-06: PassTransfer descriptor reuse enabled after reversed confirmation

Both same-build uncapped/VSync-off comparisons pass strict Native, fixed seeds/input, query conservation and mechanism checks. OFF->ON frame times:19.82->18.77ms(-5.30%) and19.365->18.555ms(-4.18%) in reversed order. Median-window p95 improves21.14->19.785 and20.91->19.61; p99 improves21.58->20.11 and21.695->19.97; maximum28.26->21.34 and26.27->21.17ms. No selected frames exceed50ms. Both reversed screenshots reviewed with correct comparable rendering. Together with corrected exact GPU/replay qualification this supports enabling gow3_native_recycle_pass_transfer_descriptors=TRUE. Core ownership/math/submission behavior remains unchanged. Other rejected candidates remainFALSE. Reverse ON has no active-run host-load snapshot, only after-exit; other snapshots are intermittent. No task build/GPU test overlapped. The slow view remains about53–54FPS, so stable60FPS is NOT achieved. Evidence:performance-60fps/pass-transfer-descriptor-pool-01/default-on-decision.json.


2026-10-06: Optional CPU stage timer cleanup did not improve this view

Added a startup timing switch with pure gates around per-draw, texture and Runtime operation clock reads. Rendering and ownership/counts are unchanged; disabled durations are null, while frame interval, completion and creation-on-miss timers remain. Main/replay builds pass; actual captured-history weapon ON/OFF GPU comparison passes173full image/memory artifacts, exact queries, GPU submissions and counts. Same-build uncapped quiet pair is18.255ms timingON versus18.645ms timingOFF(+2.14%), p9519.605->19.79,p9920.365->20.415,max27.67->22.47; neither window exceeds50ms. No FPS gain established, so gow3_native_time_cpu_stages remainsTRUE; no reverse of the unchanged variant. Confirmed PassTransfer descriptor reuse remainsTRUE. No60FPS claim. Evidence:performance-60fps/optional-cpu-stage-timing-01/keep-timing-enabled-decision.json.


2026-10-06: Render-pass object cache correct but reversed timing fails

Added default-OFF bounded exact-description Vulkan render-pass reuse owned by PassTransfer. Fresh framebuffers/views, draws/barriers and submission/wait order are preserved; retained pass owners survive cache deletion until each packet retires. Main, fixture and replay builds pass. ActualGPU64/128 partialRGBA/rawD32/stencil, key/topology/capacity/unsupported, two-fence ownership, accepted-prefix and both expected85uncertainty probes pass. Original/right/weapon161/129/173artifacts match exactly with unchanged queries/submissions/counters; cache hits225/179/279 and7created objects each. Both protected stencil programs remain byte-identical.

The first quiet uncapped pair improves18.475->17.94ms(-2.90%), p9519.8->19.17,p9920.62->19.785. Reverse instead regresses17.965->18.37ms(+2.25%), p9518.95->19.52,p9919.625->19.965. Selected windows contain no>50ms frames. No repeatable gain, so gow3_native_recycle_pass_transfer_render_passes staysFALSE. Confirmed descriptor reuse remainsTRUE. Evidence:performance-60fps/pass-transfer-render-pass-cache-01/default-off-decision.json. Stable60FPS remains unmet.


2026-10-06: Query/memory-bounded completion-marker opportunity measured

Added default-OFF metadata-only census under gpu_command_summary. Original completion/store/query-publication order is unchanged; query-close all-work bounds are separate diagnostics when prefix retirement is OFF. Main builds and actual33CPU helper checks pass. Single instrumented original-view cache-progress-ON probe exits cleanly with strictNative/query/cause checks and all population/distance/boundary conservation passing. Of12975marker attempts across1440selected reference frames,11489have zero older query tickets:8562have later accepted work (8321mixed,238draw-only,3transfer-only),2927none. Some-pending group1486has1434mixed-work windows,52none; all-ready group0. No unknown/rejected/censored/mismatched/changed-frontier/capacity-refused samples. Zero group closes at6954primary ends,2750scratch writes,1484CPUwrites,299query begins and2command boundaries. Windows overlap and are not additive saved work.

This supports prioritizing a zero-older-query deferred-marker cohort without first enabling query-prefix retirement. It does NOT authenticate target allocation lifetime, producer snapshot/read overlap, or legal canonical-NoPending crossing. Mutable owner/free/reset work remains unimplemented; no new async behavior or FPS gain claimed. Evidence:performance-60fps/marker-query-cohorts-01/summary.json.


Reference-semantics audit: recorded Canary config selects occlusion_query="fast". Local Canary source publishes cached/optimistic visibility before real results and replaces it later; its strict mode also has a deadline fallback. Local reference0x58 PS_DONE_TS stores marker bytes without the Native full drain. Source differences can explain part of an advantage but do not quantify its FPS share or bind the archived reference executable to that checkout. Vendored reference code has real asynchronous batched queries, so not all Xenia query paths are synthetic. No approximation adopted. Exact source/config hashes and lines:marker-query-cohorts-01/xenia-native-query-completion-audit.json.


Next exact marker scope is documented in marker-query-cohorts-01/zero-query-marker-owner-design.json. A known emitted pair is32command bytes storing8target bytes (position+serial) inside a dedicated96-byte title allocation; timestamp lanes and unrelated shapes must not be admitted. The8562positive zero-ticket census windows have not been classified by decoded operands/owner and are opportunity, not admitted coverage. Allocation/free receipts, same-allocation ring reset/recovery writers, physical protect/free/SystemHeapFree/publicReset bypasses, producer snapshot preflight and read leases must be handled. No owner registry or asynchronous store path is implemented. Current normal build keeps exact synchronous signaling and confirmed descriptor reuse; all diagnostic overrides are confined to isolated runs.


2026-10-06: Marker pairs identified; submission allocation cleanup built

The live SharedQueue now uses per-call stack storage for up to130 command handles (the bounded128-list queue plus two timestamp markers), retaining the existing vector path for larger callers. Command order, validation, semaphore stages, locking, errors and fence behavior are unchanged. Main and fixture builds pass; actual GPU130/131-list tests compare every copied word, alongside existing two-semaphore, image and host-survival checks. This removes one temporary heap allocation per ordinary submission; no paired FPS uplift is claimed. Evidence:performance-60fps/inline-submit-handles-01/decision.json.

Added default-OFF decoded marker and title-allocation observations under gpu_native_marker_operands plus gpu_command_summary. Original completion and stores remain unchanged. Title hooks observe exact initialization, ring reset/revalidation, recovery, destructor and CPU-direct mode; copied metadata is explicitly not a lifetime pin. The first fixture build targeted the wrong build directory; failure is preserved and the corrected build passes. Actual CP/helper4578checks and separate concurrent registry metadata77checks pass. The registry prototype is archived, not wired into production mutation or publication paths.

The single uncapped/VSyncOFF instrumented slow-view run exits cleanly, all454465queries retire/write with zero pending, and all strictNative fallback counters are0. The185-check analyzer passes. Across1440selected reference frames,14414pairs match the observed96-byte allocation with no unknown/mismatched allocations. Of12941full-completion seeds,11463are zero-query and8527have later work; every one of those8527 is an inactive-query matching pair. No capacity refusals, rejected/censored windows, changed frontiers or lookup failures. Single/pair windows overlap and must not be summed as savings. This narrows the actionable scope but does not establish legal asynchronous publication. Actual command-publication receipts, mutation gates and producer snapshot/read leases remain unfinished; deferred marker stores are still disabled.

Instrumented median-of-window-medians17.895ms (~55.9FPS), median-windowp9518.715/p9919.605ms, max24.98ms, no selected frames>50ms. This is a single diagnostic observation, not a before/after improvement or stable60FPS result. Only a post-exit host-load sample is available; no task build/GPU fixture overlapped the game run. Main674cdcb6/runtime7f15ac5f; screenshot reviewed. Evidence:performance-60fps/marker-operands-01/summary.json. Confirmed descriptor reuse remainsON; new census diagnostics remainOFF in normal launches.


2026-10-06: Exact completion-marker submission and current room investigation

The exact title-marker path is implemented behind gpu_native_submit_completion_markers, together with gow3_native_cache_coherence_waits. It authenticates the emitted pair and its dedicated allocation generation, requires zero outstanding Runtime/SDK query obligations, submits immutable Native inputs, and then performs the original immediate stores. It does not queue mutable guest marker addresses. Allocation mutation and strong completion boundaries remain guarded. CPU decoder/ownership validation passes. Same-build uncapped paired and reversed tests improve median block frametime from18.985 to17.350ms and22.060 to19.615ms, respectively. The reverse test has a worse maximum40.61 versus34.71ms; neither selected window has a frame above50ms. These comparisons establish a useful gain, not stable60FPS.

A six-control bundle additionally includes exact readonly-depth no-write metadata, canonical packed-depth render-pass groups, GPU-alias state lookup caching and shader lookup caching. GPU fixtures and captured scene comparisons preserve full packed memory, image bytes and real query results. The bundle comparison improves median22.075 to20.240ms, wall mean22.343 to21.487ms and p9523.635 to21.890ms. Individual speedups for the extra controls have not been established. The six controls are now enabled in this installation’s normal config; source defaults remain opt-in. The exact config diff and its evidence are recorded in normal-performance-bundle-decision.json.

The user's subsequent fullscreen4K/HDR reproduction confirms the next room is still slow: reviewed video shows about43FPS and later33FPS. The latter interval overlaps an unrelated host build; that overlap does not explain the earlier slowdown. The final presentation pass measures about0.178ms, whereas draw workload and guest completion polling grow substantially. Native fallback counters remain zero and all real queries retire. CPU profiles motivate reducing repeated shader-data hashing; the faster fingerprint path retains full byte equality and unchanged canonical identities. Collision/ownership tests pass and a captured weapon-scene plan matches exactly. The first paired comparison improves median19.60 to18.04ms and p9522.07 to20.375ms, but max worsens29.84 to34.24ms. The reverse comparison regressed18.545 to35.100ms; contemporaneous GPU clock/other-client telemetry was unavailable, so no causal speedup is established and the faster fingerprint default remainsFALSE. V2 also covers bridge shader-input hashing; its collision/ownership fixtures pass but live performance remains unqualified.

Three rejected experiments: boundary scratch recycling version2 has effectively unchanged median17.43 to17.42ms with worse tails; typed query commands worsen19.62 to20.02ms; query-prefix retirement on the current bundle worsens18.82 to20.44ms. The pool's runtime integration and typed-query representation were removed; their archived fixtures/evidence remain. Query-prefix retirement staysFALSE. These changes do not meet the60FPS goal. Evidence and exact limitations:performance-60fps/room-fix-20261006-02, including desktop-bundle-01, marker-comparison-02-v2.json, bundle-comparison-01.json, fast-fingerprint-validation.json and the candidate decision files.


2026-10-06 next-room capture and preserved readonly-depth experiment

The desktop-bundle-02 fullscreen3840x2160HDR run remains below target (reviewed held screenshot48FPS; live blocks around20.25–20.53ms). Strict Native counters all zero; all7,873,759 deferred queries retired and wrote guest results; clean close. The new2-frame API capture contains2,380 actual draws,3,290 events and a complete footer. It lacks initial GPU history and live query boundaries. The20s CPU profile has6,551 samples and zero lost samples. Concurrent DRM client monitoring places most graphics work in gow3 rather than the compositor/recorder. Video is an SDR desktop-monitor capture with a terminal overlay during its opening; it is not an HDR or game-only color oracle.

The existing draw-descriptor batching candidate passed actual GPU ownership/query fixtures but worsened the paired stationary median16.335 to17.410ms; it staysFALSE. A compositor workload difference prevents claiming that entire regression is caused by the candidate.

A new default-OFF candidate, gow3_native_readonly_depth_read_only_pass, explicitly uses LOAD+STORE_NONE and DEPTH_READ for certified immutable D32 visibility queries. Core Vulkan1.3 or actually enabled EXT_load_store_op_none is required; unsupported hosts retain ordinary Native D32. It never uses LOAD_OP_NONE or changes query sample counts/shader math. Consecutive immutable reads omit redundant depth-image barriers; unrelated conversions and image uses preserve their original transitions. Early-depth and retained-writer paths are unchanged, with incompatible compositions rejected. Both64/128-owner hardware tests preserve exact depth bits, visibility counts and full packed memory; integrated Scene tests include missing pipeline/capability refusals, reversed viewports, writer invalidation and queue ownership. The new room’s matched diagnostic frame-query replays preserve all77 output artifacts, full10MiB packed memory and both query results while avoiding356 depth barriers. These are captured-suffix comparisons, not proof of missing prior history or live query-boundary equivalence. Timed gameplay qualification is pending; normal config does not enable this new candidate.

Evidence: reports/rendering/native-renderer/performance-60fps/room-fix-20261006-02/desktop-bundle-02/validation.json, readonly-pass-gpu-64.log, readonly-pass-gpu-128.log, readonly-pass-scene-64.log, readonly-pass-room-parity.json. No stable60FPS claim.

### 2026-10-06 follow-up: room replay correctness and repeated timings

The preserved read-only D32 pass plus scoped packed-memory barrier omission passes isolated GPU fixtures and exact captured room/weapon replay comparisons. Offline synthetic query groups are explicitly diagnostic: they are not the missing original live query boundaries. They establish simultaneous early-depth and read-only-query execution without claiming original query timing. Full memory, images and query results match in each paired replay.

The read-only pass alone regresses median16.75→17.97ms. Combining it with the scoped packed-memory barrier omission improves19.135→17.410ms, then17.330→16.760ms in reverse order. Repeated GPU interval reduction is only about0.12ms; background compositor/browser load differs and the reverse maximum worsens. Both controls remain OFF in normal configuration.

Fast fingerprint V2 passes paired health/config/camera/query checks, with medians17.930→16.940ms and17.220→16.910ms in reverse order. Both OFF intervals have greater compositor activity, so an isolated gain is not established; this control also remains OFF. These are private720p first-room comparisons, not proof of60FPS in the fullscreen next room. Evidence:performance-60fps/room-fix-20261006-02/candidate-decisions-02.json and the referenced comparison/host reports.

Optional Clang IR profile-guided host-renderer compilation is being evaluated next; no PGO speedup has yet been measured. The normal configuration retains the six previously qualified controls. Stable60FPS in the user's slow view remains unresolved.

### 2026-10-07: PGO evaluation and exact hardware-assisted query rejection

Added optional Clang IR PGO to the host Native renderer, Plume and graphics bridge, preserving generated guest options and floating-point policy. Training completes cleanly;30host objects produce a matching LLVM22profile. Uninstrumented baseline/candidate pairs use identical common SDK runtime. Median21.020→19.005ms and reverse19.930→19.340ms; first baseline has substantial external browser activity and90frames above50ms, so their removal is not attributed to PGO. Reversep9521.290→20.720ms,p9921.890→21.490ms,max23.650→25.320ms,zero>50ms. PGO is retained as an optional build workflow and currently OFF to isolate the following GPU test. Saved profile/binaries/evaluation are under room-fix-20261006-02/pgo.

Added default-OFF gow3_native_readonly_depth_lower_bound_query. The unchanged audited query VS feeds hardwareGE against a frozen full-FP24 predecessor D32 image. A separately keyed, audited readonly fragment preserves the original packed-depth comparison/discard and late occlusion counting, removes no arithmetic, and has no FragDepth output, interlock or packed-memory stores. It retains packed-buffer dependencies and refuses the old skip-EDRAM and conservative-FragDepth modes and retained-writer compositions. Unsupported preparation stays within the ordinary Native path. No Xenia fallback is introduced.

Isolated GPU correctness passes64/128capacities; coherent1280×720stress intervals repeat about2.53ms for the prior late-depth query vs1.15ms for the lower-bound candidate, whereas random-depth surfaces are slightly slower. These synthetic intervals exclude initial conversion and CPU preparation and are not game frame times. Controlled Scene fixtures pass36,056/38,040checks at64/128, including reversed unit depth, writer reconversion, missing pipelines and unadvertisedSTORE_NONE. Captured weapon173and room153non-JSON output files are byte-identical including raw/preview images and full10MiB packed memory; synthetic query groups match and both early material depth and query optimization have positive coverage. The captured groups lack original live query boundaries and initial GPU history, so they establish paired correctness only.

Actual uncapped baseline/candidate gameplay measurement is underway. The new option is not enabled in the normal config; stable60FPS is not yet established. Evidence:performance-60fps/room-fix-20261006-02/query-lower-bound/validation.json and capture-parity.json.


### 2026-10-07: guarded query dependencies and CPU query resets

The first lower-bound query live comparison is negative: median20.120→20.350ms, p9521.660→22.025ms. The option remains OFF. A revised version shares the packed-buffer read dependency only across the same immutable depth owner and unchanged global mutation/physical epochs; transfers, ordinary consumers and ownership changes invalidate it. Scene64/128fixtures pass36,056/38,040checks. The next-room captured pair replaces356buffer barriers with2while all153non-JSON artifacts/fullEDRAM/query groups remain exact; weapon173artifacts also match. Both subsequent live runs pass functional/strictNative/query checks, but concurrent external WebKit compilation makes their timings unqualified (median29.330→29.170ms, worsep95/p99). No speedup is claimed or enabled from this pair.

A separate default-OFF gow3_native_host_query_reset now uses Vulkan hostQueryReset only when actually enabled by device creation. Initially free query slots are reset once; completed prefixes are reset only after their covering fence and successful result copy/validation. Later in-flight slots remain untouched. Disabled/unadvertised devices keep GPU resets within the Native renderer. Exact64/128-slot tests cover full-pool overflow, repeated visibility changes, logical record pressure, two in-flight fences and wrapped-prefix reuse. Captured weapon/room paired outputs and query groups match exactly:184/356GPU resets become15/27CPU reset calls respectively. API capture limitations about original query boundaries/history still apply. The main build and live functional smoke test pass:146,438deferred query obligations all retire/write, zero pending/superseded, strict Xenia counters zero, screenshot reviewed, clean close.137,749GPU reset commands are replaced by15,165CPU range-reset calls. Smoke timings are excluded because the external build is still active. FPS comparisons await the user's unrelated build completion. Normal configuration remains unchanged and stable60FPS is unresolved.

Evidence:performance-60fps/room-fix-20261006-02/query-lower-bound/validation.json and host-query-reset/validation.json. The replay comparisons verify correctness, not original-frame timing or absolute equality to missing capture history.


2026-10-07: Host query reset measured; immutable visibility-pass grouping under live evaluation

The unrelated compiler workload finished. A same-build uncapped stationary pair for host query reset passes strict Native, copied inputs/cache, camera and real-query retirement checks. Median block time changes20.655→20.500ms(-0.75%); inclusive GPU interval/frame remains10.2086→10.2080ms. Background Discord CPU/GPU activity differs, so this is not an isolated useful speedup. Host query reset remains OFF by default and in normal configuration. Evidence:performance-60fps/room-fix-20261006-02/host-query-reset/comparison-01.json and host-comparison-01.json.

A separate default-OFF readonly-query render-pass grouping experiment combines completed-slot host reset with immutable D32 LOAD+STORE_NONE passes and certified packed-buffer barrier omission. It retains separate real query slots/results and the existing bounded FIFO; different image owners, writes, transfers, sampled timestamps and unaudited draws close the pass. Missing capabilities use ordinary Native rendering. Extended Scene tests pass36,815/39,269checks at64/128capacities, including empty queries between members, reversed depth, writer reconversion, missing capabilities and shutdown with an accepted group. Captured weapon173/room153artifacts and query groups match exactly. The room replay reduces356readonly passes to24; this is suffix correctness, not proof of original-frame history or performance.

The main build passes. First baseline gameplay completes, but the first optimized command is rejected before launch for an omitted explicit --native-readonly-depth-attachment prerequisite. Corrected pair02 explicitly supplies the already default-enabled prerequisite in both arms and uses fresh runs. Live timing is pending. Normal configuration remains unchanged; stable60FPS remains unresolved. Evidence:performance-60fps/room-fix-20261006-02/readonly-query-groups/validation.json and launch-rejection-01.json.


2026-10-07: Immutable visibility-pass grouping enabled for this installation

Both same-build uncapped paired/reversed gameplay checks pass strict Native, real-query retirement, copied inputs/cache, matched camera/workload and warm-cache gates. Median block times19.840→18.605ms and17.360→17.030ms; median blockp9521.470→20.360ms and19.730→17.980ms. GPU inclusive intervals/frame decrease10.2775→9.2793ms and10.3221→9.4315ms. Desktop load differs (first favors ON; reversed compositor load is higher ON), so no exact isolated percentage is claimed. Maximum individual frames increase24.93→25.73ms and23.45→25.79ms; selected windows have no>50ms frame. Four screenshot pairs reviewed with matching camera and differing idle animations. The original command prerequisite rejection is preserved. Reverse output folders are under sibling room-fix-20261006-03 due report suffix substitution; live-paths-03.json records their location.

The user's fullscreen3840x2160 nativeWayland/HDR run completes cleanly. All2,833,238real query obligations retire/write with zero pending/superseded and every Xenia/mixed/rejected counter zero. The185-second SDR portal video includes the desktop if Gears is obscured; user informed and relevant held screenshots reviewed. Selected reviewed video163..185seconds has960complete frames, median block19.260ms (~51.9FPS), p9520.275ms, p9920.840ms, max42.69ms, no>50ms frame and no shader/PSO creation/miss/eviction. Earlier brief view/menu change is excluded explicitly. This is one observation, not a fullscreen before/after gain or stable60FPS.

The four companion controls host_query_reset, readonly_depth_read_only_pass, readonly_depth_skip_edram_barrier and readonly_query_render_pass_groups are now TRUE in config/gow3.toml; source defaults remain opt-in. This keeps the tested modest improvement while preserving all prior controls. Backup and decision:room-fix-20261006-02/readonly-query-groups/config-before-query-groups.toml and normal-query-groups-decision.json.

The held room still has1172draws,89transfer lists,87.77submissions and2.428ms host submission time per frame. A new existing-setting comparison keeps query grouping enabled and tests draw target32 versus16 with the same64owner/two-fence limits. Captured weapon173/room153image and full-memory artifacts, exact diagnostic queries, expected draw/frame coverage and accepted/completed ownership match. Queue submissions decrease439→361 and347→293 across the two-frame replays. The first checker wrongly expected the unused report complete flag; corrected actual coverage checks and that failure are recorded. Live timing pending; normal target remains16. Evidence:room-fix-20261006-02/batch32-after-query-groups. Stable60FPS remains unmet.


2026-10-07: Larger batches after query grouping did not improve frame times. Same-build target16/32 pair passes functional/query/camera/cache gates and exact captured memory/images. Median16.430→16.660ms, p9517.635→17.780ms, p9918.295→18.750ms; submissions72.05→55.35/frame but host submit time1.789→1.777ms is essentially unchanged and fence waits increase4.201→4.500ms. Normal target remains16. Evidence:performance-60fps/room-fix-20261006-02/batch32-after-query-groups/keep-target16.json. Conservative unused descriptor binding experiment is next; not enabled pending correctness and timing.


2026-10-07: Conservative SPIR-V descriptor reflection skips only sets undeclared by both actual selected modules. All1519archive modules and malformed/refusal fixtures pass; unknown syntax binds all sets. Actual Scene64/128tests retain exact full memory and independent empty/positive query oracles; captured weapon173/room153artifacts and diagnostic queries match. Room omits7468of11900set bindings. First live pair17.03→17.08ms with uneven external CPU load; reverse24.71→16.585ms is invalid for attribution because baseline overlaps another process consuming~10.6CPU cores. Both pairs close cleanly, retire real queries and keep Xenia counters zero. Option remains defaultOFF and absent from normal config. Evidence:room-fix-20261006-02/unused-descriptor-bindings/validation.json and host-assessment-01/02.json. Stable60FPS remains unresolved.


2026-10-07: Existing pinned atomic depth can now compose with bounded packed-depth groups, keeping access models separate and all boundaries intact. CPU oracle passes 3,686,004 checks; 64/128 GPU fixtures cover independent full-memory/ordering/query/retirement behavior. Captured weapon173/room153 artifacts and query groups remain byte-exact; normal early-depth and D32 paths retain coverage. Live pair passes functional gates but median16.115→17.240ms, p9517.130→18.230ms, inclusiveGPU9.20972→9.28122ms/frame. Desktop compositor load differs, preventing exact regression attribution. No gain demonstrated; atomic remains defaultOFF and normal configuration unchanged. Evidence: performance-60fps/room-fix-20261006-02/atomic-depth-groups/validation.json. Stable60FPS remains unresolved.


2026-10-07: Default-OFF packed primary color/material pass groups implemented with pinned unchanged modules, disjoint pixel/word planes and immutable sampled-image owners. CPU proof1,855,854checks;64/128 Scene tests4,293/7,365checks cover actual materials, prewarm/unknown module refusal, independent33-draw color/depth/stencil ordering oracle, exact10MiB, queries and partial shutdown. Captured weapon173/room153artifacts and diagnostic query groups match; room removes133pass breaks/762sample barriers across2frames. Both uncapped live/reversed pairs pass strict Native, query retirement, camera, cache and workload gates. InclusiveGPU9.228→8.898ms and9.328→8.821ms, but totalmedian16.55→18.26ms and16.16→16.445ms. Candidate desktop load is higher in both; no end-to-end gain demonstrated, normal config unchanged. Stable60FPS unresolved. Evidence:performance-60fps/room-fix-20261006-02/color-pass-groups/validation.json.


2026-10-07: Wider queue after query grouping remains disabled. Exact captured weapon173/room153 artifacts, full packed memory and diagnostic queries match. Same-build uncapped capacity64/two-fence vs128/four-fence pair retains target16 and passes functional/query/camera/cache gates. Median16.710→17.060ms, p9517.79→18.11ms, inclusiveGPU9.245→9.221ms, hostwait4.230→4.008ms. Compositor load differs; no end-to-end gain established. Normal stays64/two fences. Evidence:performance-60fps/room-fix-20261006-02/queue-window-after-query-groups/validation.json. Stable60FPS remains unresolved.


2026-10-07: Default-OFF uniform-block page versions preserve exact assignments/maxima including nonmonotonic values, wrapped epochs, overlap and byte ownership. CPU341,611checks, real GPU overlay fixtures and captured weapon173/room153full-memory/images/query/queue parity pass. Synthetic CPU workload13.6x faster is not FPS. Live pairs17.605→16.670ms then16.920→17.120ms; background desktop load differs and no overall gain repeats. Draw finish is lower by~0.06ms. Normal config unchanged; stable60FPS unresolved. Evidence:performance-60fps/room-fix-20261006-02/page-version-blocks/validation.json.

### 2026-10-07: checked Native submission worker (experimental, OFF)

The same fullscreen run captured two user-held views: the slow room averaged about1282 queued draws/frame versus260 at the fast angle, with block-median frame times23.66ms versus8.37ms. Warm shader/PSO creation counters were zero. These are different camera views, not an optimization A/B. All5,101,033 query obligations retired, every strict Xenia counter was zero, and shutdown was clean. Evidence: `performance-60fps/room-fix-20261006-02/page-version-blocks/desktop-blocks-01/angle-comparison.json` and `terminal-health.json`.

Added default-off `gow3_native_submission_worker`: one portable bounded driver worker overlaps checked Native queue submissions with subsequent preparation. It preserves the existing logical FIFO, two/four-fence bounds, GPU resource owners, original shader operations and required completion barriers. A driver receipt must precede fence polling/reset/reuse even if the GPU already signaled; exceptions poison the queue without retry and shutdown joins before borrowed owners can die. Separate receipt-wait timing avoids reporting overlapped driver CPU time as main-thread blocking. Ordinary unchecked replay queues explicitly refuse this option.

Validation passed29,999 ordered CPU requests, cancellation and shutdown tests; hardware64/128-list capacities with two/four fences, timestamps off/on, every transfer write checked, and an intentionally held already-signaled fence. Uncertain actual submit and wait tests retained render-pass ownership, refused replay and exited85 as expected. Main build passed after correcting a macro semicolon. Four automated gameplay runs closed cleanly with all queries retired, no Native mixing, matched stationary camera, warm shaders and unlimited FPS/VSync off. First pair median16.05→15.775ms; reverse16.82→16.085ms, but higher desktop compositor load in reverse OFF arm confounds attribution. Max frame time did not consistently improve. Normal configuration remains unchanged and the stable60FPS next-room goal is **unresolved**. Fullscreen confirmation is pending; reports/source backups are under `performance-60fps/room-fix-20261006-02/submission-worker/`.

### 2026-10-07/08: fullscreen worker result and checkpoint-copy repair

The submission-worker fullscreen test still measured24.945ms block-median (~40FPS) at the user-held slow next-room view, with1309 queued draws/frame, zero warm shader/PSO misses, and12.823ms/frame inclusive GPU queue intervals. These intervals overlap other work and are not exclusive frame components. The worker remains default OFF; this was not a matched A/B against the previous block-version run. Recording and a separate20-second CPU profile are in `performance-60fps/room-fix-20261006-02/submission-worker/desktop-worker-02/`.

Checkpoint reload in the preceding run aborted on an undefined Native buffer-copy source. Fixed a concrete planner error that treated any GPU-owned overlap as a fully GPU-owned source. Mixed copies now retain immutable CPU bytes and exact GPU-owned fragments, reject missing required history, preserve byte-sized holes/source ownership and update only the destination. The GPU8192-byte fixture and asynchronous Scene tests passed. The user successfully reloaded the checkpoint afterward. The original failing source range was not logged, so exact retrospective causal attribution is limited. Final run closed cleanly:3,437,821 query obligations completed, all strict Xenia counters zero. Evidence/source backups: `checkpoint-mixed-copy/`.

A bounded exact vertex-lookup index passed717,261 CPU checks, sanitizers and326 captured artifact comparisons, but its first uncapped live pair worsened median16.93→17.675ms despite91.4% index hits. Background activity varied; no gain was established. Archived this experiment under `vertex-lookup/candidate-source/` and restored active sources. The stable60FPS next-room goal remains **unresolved**.


### 2026-10-08: captured-room per-draw timing

Added an explicit offline every-frame draw-timestamp mode; the live sampling period/default remains unchanged. The ledger CPU test passes2346checks. Paired captured-room replays measure all2380draws over two frames with no unavailable/pending samples or diagnostic failures; all153 image/full-memory artifacts and diagnostic query results are exact. The initial command was rejected before rendering for missing deferred-query mode, then corrected. Both successful arms disable pass grouping and use synthetic query wrappers; the room also lacks initial GPU history. Timings include dependencies and overlapping command-list execution, so neither their sum nor this diagnostic replay is original live frame time. The ranking points to a distributed material/depth workload, led by a3c9/75d5, rather than one dominant draw. Evidence:performance-60fps/room-fix-20261006-02/room-draw-timing/validation.json and ranking.json. No FPS gain or60FPS completion claim.


### 2026-10-08: CPU texture footprint hashing rejected

A same-capacity exact hash table for immutable CPU texture footprint keys passed7113CPU checks (including total hash collision, allocation failures, eviction/owner lifetime,1600range and4800version cases), asynchronous Scene tests and326exact weapon/room artifacts with matching LRU/queue/query statistics. Uncapped matched first-room medians16.87→16.76ms then17.27→17.56ms in reverse; mean wall time worsens in both and desktop activity varies. No repeatable end-to-end gain. Candidate source is archived and active sources restored; normal configuration unchanged. Reports:performance-60fps/room-fix-20261006-02/footprint-hash.


2026-10-08: Experimental sequential Type0 shader-constant batching uses the existing bulk constant loader only for complete single-family float/fetch/bool-loop packets. All other register semantics remain scalar. CPU decoder/oracle checks pass72,390cases, including wrapped ring reads, family boundaries, repeated registers, truncation and side effects. Four uncapped live first-room runs pass functional/query/camera/warm-cache gates. Pair medians17.47→17.08ms, reversed16.80→16.88ms; desktop load varies and reverse ON includes compiler activity. No repeatable end-to-end gain claimed. Default OFF, normal configuration unchanged; fullscreen slow-room diagnostic now running with Type0 ON and submission worker OFF. Evidence:performance-60fps/room-fix-20261006-02/type0-constants/validation.json. Stable60FPS remains unresolved.


2026-10-11: User reports the same turret/gas-tank fight drop to39FPS. Complete sparse physical-GPU capture preserved70,767samples and all query/native-path counters. Analysis identifies a recurring16x16x16GPU-alias volume decode/assembly with16slice waits plus assembly wait; its pre-submission gap is~1.4ms, not proof of recoverable FPS. Implemented default-OFF queued volume alias loads in the original Native FIFO using unchanged RGBA32F decoding/copy semantics, bounded preflight and original fence ownership. CPU87admission checks, four hardware fixtures(64/128capacities, workerOFF/ON), room153 and weapon173bit-exact image/full-memory/query artifacts pass. Main/replay builds pass. Same-binary uncapped off/on measurements are in progress; no FPS improvement or stable60claim yet. Evidence:performance-60fps/room-fix-20261006-02/queued-volume-alias-20261011.

2026-10-11: Queued-volume live first/reverse pairs both pass functional gates, exact16synchronous slice-dispatch reductions/APIframe, query retirement and strictNative unmixed controls. Mean15.567→14.266ms(first),15.700→14.334ms(reverse); p95blockmedians16.700→15.370ms and17.300→15.315ms. ProjectTorque and unequal desktopload remain disclosed in host-review; both results retain performance_attribution_allowed=false. Experimental fullscreen HDR test launched with queued volumesON, MangoHud, no recording/profiler/timestamps; manual fight confirmation pending. Normalconfig/default remainunchanged. tools/test-native-performance.sh nowpoints at queued-volume fullscreen01. Stable60FPScombat is not established.

2026-10-11: Volume fullscreen01 completed cleanly:8,814queued volumes/141,024slices, zero volume fallback, all queries retired, strictNative only, normal files preserved. User reports much steadier frame times but~57idle/46action. Log-derived late held average56.91FPS agrees. Remaining action cache churn and FSI misses identified; the test used older339-state seed instead of learned combat observations. Two isolated follow-ups prepared: existing raw-float GPU-only64KiBtexture path with volumeON, and learned FSI state cache with unchanged shader code. No new FPS gain/promotion claimed yet.

2026-10-11: The learned combat cache preserves all 153 room and 173 weapon artifacts, full packed memory and query results. Its first stationary comparison passes functional checks but does not demonstrate a gain: mean 13.329→13.732 ms with substantially greater Discord/compositor activity in the candidate. No reverse comparison or isolated FPS claim follows. The subsequent uncapped 4K HDR run exits cleanly, retires all queries and remains exclusively Native. The user confirms two checkpoint fight attempts: 60+ FPS stationary, 55–60 during movement/action, and the same 49 FPS explosion drop. The checkpoint reload is excluded from combat interpretation. Presenter GPU timestamps cover 6,875 frames at 0.191 ms mean, 0.351 ms maximum; scaling/HDR GPU work cannot account for the multi-millisecond deficit, while CPU acquire/present remains outside this timer. Normal files remain unchanged. Evidence: performance-60fps/room-fix-20261006-02/learned-combat-cache-20261011/fullscreen-01.

The raw GPU float texture option remains OFF after its separate comparison showed no gain. In the latest fight most sampled recurring 64 KiB uploads have no GPU overlay/readback, so that option cannot be assumed to select them. The next focused candidate is the existing guarded CPU-upload drain deferral: ordinary CPU-only uploads currently flush pending GPU work, while the guarded path retains required waits for GPU overlays. Current hardware fixtures already prove owner retention and exact full-memory/query results; composition with the learned cache and queued volumes is being checked before a combat trial. Stable 60 FPS during combat remains unresolved.


2026-10-11: CPU-only texture drain deferral completed a clean fullscreen trial with 10,126 texture creations: 2,733 pure CPU, 843 with pending GPU work, and 7,393 requiring an overlay. User reports approximately60FPS idle,54 during the NPC gun battle, and49 at the explosion, with camera-panning drops. This does not establish an improvement; the experiment remains OFF for normal play. Existing presenter samples remain below0.351ms inclusive and do not explain the combat deficit. Evidence: pure-cpu-drains-after-learned-20261011/fullscreen-01/run/result.json and user-feedback.json reports.

The previously validated FSI material composition, FSI cache headroom and queued volume alias loads are now enabled in this installation's normal config. Imported208 exact learned-cache files while preserving2599 canonical files and all normal saves/preferences. Promotion receipt: learned-combat-cache-20261011/promotion/applied.json. Immediate repeated validation writes no files. Old experiment manifests intentionally retain their pre-promotion config identity. No stable60FPS or isolated percentage gain is claimed. Camera-panning/action texture reload costs are the next diagnostic target.

A private same-binary texture-panning diagnostic is prepared and independently reviewed at texture-pan-census-20261011/fullscreen-01. Static validation passes with2,816 exact seed files, private saves/config/cache, strict Native, uncapped/VSyncOFF, both rejected texture experiments OFF and GPU/video/perf captures OFF. Existing reload/full-overlay census enables inclusive CPU-stage timers, so this is cost attribution with diagnostic overhead, not clean FPS comparison. tools/test-native-performance.sh points to this new unconsumed stage. The requested fight/panning/held-view run has not yet launched; user availability is pending.


2026-10-11: Texture/pan census finished cleanly (115s), with strict Native, all queries retired and normal config/saves/cache unchanged. User reports mostly60+FPS with51FPS minimum during action and remaining panning drops. Complete per-frame census reconciles4,159 frame/epoch records; inferred gameplay averages0.599ms texture creation including0.319ms pending-work flush,0.049ms overlay,0.136ms decode and0.083ms upload. Observed recent reloads average0.068ms and do not support blind cache-capacity growth. These are overlapping inclusive host intervals, with timing overhead and9,161 overwritten bounded tombstones. Sampled texture version/key/lookup median0.642ms (max2.067), draw setup outside texture median4.260ms. No matched-route gain claim. Evidence: texture-pan-census-20261011/fullscreen-01/analysis and user-feedback.json.

User also observed one startup shader failure message. Investigation identified a valid77,348-byte optional FSI module refused with1,244bytes remaining in its16MiB source budget; current progress accounting labels that expected skip as failure. All853 canonical/overflow pipelines prepared. A separate progress accounting repair and default-OFF exact CPU image slot lookup candidate are under implementation; no new FPS gain claimed.


2026-10-11: Startup shader status now distinguishes optional admission skips from real preparation failures. The reported one failure was a valid77,348-byte optional source exceeding its16MiB budget with1,244bytes remaining; all853 ordinary pipelines prepared. Separate skipped/failed counters and phase-aware white labels retain the red progress bar; true exceptions remain failures. The rebuilt runtime logs exact skip reasons with zero source validation/compile failures. Focused progress tests pass20assertions/2cases. Evidence: shader-startup-status-20261011.

A default-OFF32-slot CPU image lookup hint retains fresh physical versions, exact fetch/base/mip/version equality, the owning ordered map and original cache policy. Per-slot nonCPU invalidation and global nondraw/before-erase invalidation prevent stale owners. CPU2490checks pass in optimized, debug-iterator and ASan/UBSan builds (LSan unavailable in sandbox); independent review passes. New OFF matches prior saved room153/weapon173artifacts, and new ON matches OFF byte-for-byte including full10MiB memory, query results, cache and shipping paths. Room/weapon avoid3617of6013 and6049of9157ordered searches.

Both uncapped, timers-OFF live orders pass functional/query/cache/camera/workload gates. Mean frame time13.877→13.610ms and14.015→13.847ms; block-median13.730→13.360 and13.875→13.590. Tail metrics do not consistently improve: reverse median-blockp9916.470→17.110ms and max22.94→28.27ms. Desktop graphics load is lower ON in both, with varying external CPU activity; isolated FPS attribution remains false. Normal option remainsOFF. User confirmed availability for an experimental fullscreen4KHDR combat/panning check, launched with MangoHud and no profiler/video/timing census; outcome pending. tools/test-native-performance.sh now points to cpu-image-slot-lookup-20261011/fullscreen-01 (currently running). Normal saves/config/cache remain protected by copied inputs. Stable60FPS during combat remains unresolved.

2026-10-11: CPU-image-slot fullscreen01 closed cleanly after102.8s with all533,300real query obligations retired, strictNative unmixed and4,708,880slot hits/9,503erase invalidations. User reports41FPS minimum and55–60average; no separate held/panning values. Candidate does not solve combat and remainsOFFnormally. This unmatched route cannot establish a causal regression against prior51FPS report. Existing logs are being correlated with slow-frame workload; offline conservative float-constant read coverage is the next bounded investigation, without new runtime changes. Evidence: cpu-image-slot-lookup-20261011/fullscreen-01/{run/result.json,user-feedback.json,decision.json}.

2026-10-11: Fresh sparse physical GPU capture with learned combat cache and queued volumes completed cleanly in73.04s. All37,712physical records are available with complete retention; real queries retired, strictNative unmixed, normalconfig/saves/cache unchanged. Slotlookup remainsOFF. User confirms reproduced drops and39FPS minimum. Postloading API1321..2400 contains multiple17–19ms median blocks and25–28ms p95blocks; exact route phase markers were not captured. This is an instrumented diagnostic, not a performance comparison. Submission gaps and actual selected shader cohorts are now being analyzed; no new optimization/promotion or stable60FPS claim. Evidence:performance-60fps/room-fix-20261006-02/combat-gpu-after-learned-20261011/fullscreen-01.

2026-10-11: Fresh capture analysis finds increasing GPU interval coverage plus late submission preparation in slow blocks; no incorrect newest-fence wait or major reservation-pressure fix. Unknown101physical transfer tags all pair with adjacent same-event conversion draws; raw unknown flags remain. New default-OFF inactive-early-catalogue admission reuses existing FSI state proofs for exactly VS75d5/PSa3c9 and55c79, only with catalogueOFF; baseearly and activecatalogue priority remain. Seven source files updated; main/replay builds and20,880Release/debug policy checks pass, independent review passes. New main4abf4992/runtimeb4346ed7/replayb933f09f. Room64 OFF preserves previous153artifacts; ON preserves all153includingfull10MiBmemory/queries and positively selects180newdraws. Weapon and precedence/refusal checks ongoing; no FPS result or promotion.

Global prewarm simulation reveals two-pair ON displaces3 existing successful states under unchanged256PSO/16MiB limits: bec8deef,f350cca9 (oneexactraster),4539f983; all were used in latestfullscreen. This is a real cache tradeoff absent from smallcapturereplay; do not claim all254baseline states preserved or enablebydefault. Separately PS1a70 was morefrequent in slowcapture windows, absentfromseed; it hasresidentVS and anoverflowcanonicalPSO but Runtimeoptionalprewarm searches onlypersistentprepared. Privateoffline1acompile58,836bytes validates and reproducescanonicalbytes. A private512rowreplacementseed removesonlypreviouslyunpreparedc5c5 and canretainall254baselinepreparedstates+1awithinexistingbudgets if exactready-overflowlookupisadded. Proposedlookup is report-localonly, defaultOFF, CPU2,727checks acrossRelease/debug/sanitizersPASS; neither savedroomnorweaponcontains1a, so targetGPUparityisperformanceuntested. No productionoverflowfix/cacheimport performed. Evidence:combat-gpu-after-learned-20261011/{canonical-material-admission,overflow-action-audit}.

2026-10-11: Inactive-catalogue FSI admission now passes all five qualification pairs: room153, weapon173, active-catalogue precedence153, missing-residency153 and full-frame-query refusal153 exact image/full10MiB/query artifacts. Candidate selects180/320 captured room/weapon draws; when absent it refuses safely, and queried draws retain canonical behavior. The original query command combined mutually exclusive wrappers and was rejected before rendering; frozen evidence remains and separately authenticated qualification-query-v2 removes only that conflict. No production correction was needed. Same-binary uncapped live comparisons are next, with known three-state optional-budget displacement disclosed. Normal configuration unchanged; stable60FPS combat unresolved.

2026-10-11: Inactive-catalogue candidate remains OFF after first uncapped live pair. Bothrunsclosecleanly, exactcamera reviewed, strictNative/allqueriesretired; mean13.173→13.062ms and blockmedian13.000→12.890ms are not isolatedgains. Comparison fails two unrelatedsmallmaterialrate gates (0.016→0.247 and1.016→1.247draw/APIframe), whileONhaslowerexternalCPU/gfxactivity. No checker weakened. Given tiny difference and knownthree-statecombatcachetradeoff, reverse andpreparedfullscreen trial are notrun. Normalconfiguration unchanged. Decision/evidence:combat-gpu-after-learned-20261011/canonical-material-admission/live-stage/decision.json. Separatepoint-UNORM aliasnumericprobe and currentcombatCPUattribution are next; stable60FPSunresolved.

2026-10-11: First current-CPU fullscreen run closed cleanly but the user completed combat before the separate profiler attachment; no CPU profile exists for fullscreen-01. Preserved that run. New fullscreen-02 combines launch and attachment into one authorized command, checks perf access before opening the game, validates the exact original game PID/starttime/private paths, and starts99Hz cycles:u DWARF32768 recording before gameplay. Recording was confirmed growing at6,436,688bytes (gamePID1765755/perfPID1765802), bounded300seconds orgameexit. Same qualified main/runtime, default candidateOFF, private learnedcache/saves, uncapped4KHDR. User told recording is active before replay. Raw stack analysis remains pending; this instrumented run is not clean FPS evidence. No normalconfig/default promotion.

2026-10-11: Current combat CPU capture fullscreen-02 completed cleanly; user reports34FPS minimum with profiler active, otherwise prior behavior. This is not a clean regression comparison. Safe gameplayAPI2641..6960; worst complete block4441..4560 median21.60/p9534.61ms, later6241..6840 med15.27..15.54ms. Extra work:1453vs1151draws/frame,7.34vs1.005CPU texture creations,6.53vs0budgetevictions. All46 timestamped LOST records predate gameplay; untimestamped loss summaries and36/42throttle transitions limit sampling precision. Direct leaf attribution recovered all25,670retainedsamples despite absent producer-threadDWARF stacks. GetData hot cycles mostly common polling/ABI, not recoverable report arithmetic; existing optimized leaf already enabled. Two scalar byte stores account for28/51DecodeTexture samples and59.15%ofits sampledcycles, supporting a narrow exact block-copy optimization. No evidence for blind shader-cache growth or staging-release FPS claims. Reports:combat-gpu-after-learned-20261011/combat-cpu-current/fullscreen-02/run/cpu-profile/{analysis,window-analysis}.

2026-10-11: Implemented exact CPU texture decoder4/8/16-byte block copies and word endian permutations in texture_data.cpp, retaining original1/2-byte path, texture layouts, output bytes and failures. Independent source alignment/portability review passes; differential/sanitizer checks and capturedGPUqualification pending. Original main/runtime/replay preserved under combat-gpu-after-learned-20261011/texture-decode-blocks/binary-before. Separate PRIVATE point-UNORM reconstruction probe passes2,365,440floatcomponents per corrected path; rawcontrols retain775,424one-ULP differences. That GPUformat probe is not integrated or a performanceclaim. Stable60FPS combat remains unresolved.

2026-10-12: Exact CPU texture block decoder qualified and rebuilt. Release plus ASan/UBSan/LSan pass43,801checks, including612capturedfetchdescriptors and exact invalid-input errors. Original isolated harness C++20 mismatch and sandboxLSan failure preserved; corrected C++23 and unsandboxed samebinary checks pass. Forty isolatedCPUbenchworkloads allimprove; familymedian1.58–3.22x acrossbothorders, notFPS. Newmain936dd9e6/runtimeunchangedb4346ed7/replay37c742dd. Authenticated savedroom153/weapon173artifacts, full10MiBEDRAM, queries, cacheowners andshaderselection arebitexact. qualification-receipt.json binds current binaries/source+proofs. Normalconfig/cache/savesunchanged beforetrial, GPUUNORMprobe remainsprivate/unintegrated. Evidence:combat-gpu-after-learned-20261011/texture-decode-blocks.

User confirmedtestavailability. Cleanfullscreen-01 stage independentlyreviewed/frozen/validated and launchedgamePID2090850 withMangoHudonly, uncappedNative4KHDR/VSyncOFF/gameaniso, allprofilers/video/censusOFF. Window/swapchain verifiedbefore user told tostartfight. Unifiedexecsession69087 remainsactiveuntilnormalgameexit; routefeedbackpending. tools/test-native-performance.sh nowtargets thisstage (one-shotoutput; do notrerun afterconsumed). No stable60FPS or end-to-endgainclaim.

2026-10-12: Clean exact-decoder fullscreen01 completed normally after118.82s, all471,308queries retired/written, strictNative unmixed, noCPU/GPUprofiling/video/census, normalconfig/saves/cache unchanged. User reports samecombatproblem, minimum43FPS. Decoder microbenchmark gain is real isolatedCPUwork, but no end-to-endcombat gain established and stable60FPSunresolved. User feedback saved texture-decode-blocks/fullscreen-01/user-feedback.json. Next work compares existingGPUconsumer/aliasandCPUsubmission-preparation evidence before anotherroute; no immediate repeatcapture requested.


2026-10-12: User still reports 43 FPS during combat after the exact texture decoder change. Added an opt-in experiment that retains an immutable early-depth lower bound across certified monotonic packed-depth material draws. Original packed rendering, queries, synchronization and submission ownership remain authoritative; Native does not fall back to Xenia. The option remains OFF in source defaults and normal configuration.

The first captured-room trial retained 442 draws but saved no conversions. Static event/actual-variant analysis exactly explains all eight refusals: six inter-pass failures came from missing derived-module certificates, while two out-of-cohort tail draws correctly invalidate after the useful passes. Added only five freshly compiled, byte-authenticated module pins. All 178 modules cold-recompile exactly; release 7,284 checks, debug and ASan/UBSan 6,600 each, plus 57 structural refusal controls pass. Main and replay rebuilt successfully. The ten-mode controlled GPU fixture passes full memory, image and query parity. The earlier synthetic alias test race and failed work-reduction trial remain preserved; neither is treated as a successful qualification.

Current evidence is under combat-gpu-after-learned-20261011/early-depth-material-retention-20261012 and monotonic-depth-modules-v2-20261012. Full captured-room/weapon work-reduction qualification and shipping-baseline FPS comparisons remain pending. An older lower bound can weaken early rejection, so fewer conversions alone will not establish a speedup. Stable 60 FPS combat remains unresolved.


2026-10-12: Retention qualification-v3 passes all 153 room and 173 weapon artifacts, full packed memory and queries; both two-frame replays reduce shared depth conversions from eight to two. First live shipping comparison does not show a gain: block median 12.755 to 13.065 ms, p95 13.49 to 14.06 ms. Five ordinary-material workload checks fail and desktop activity differs; these gates were not relaxed. Live reuse still stops at about three module refusals per frame, retaining only 43.66 draws per frame and performing about four conversions versus the shipping baseline's one. Reverse timing and another user combat trial are deferred until these actual interruptions are identified.

Added bounded module-refusal provenance under the existing optional detailed-report flag. It records selected source/variant, module and PSO certificate matches, state, counts and first/last event; it never changes admission or rendering. Normal detailed reports and retention remain OFF. Diagnostic builds passed; a private automated diagnostic run is being prepared, with no timing claim. Prior measured evidence remains intact under early-depth-material-retention-20261012/live-stage; diagnostic authority is separate under diagnostic-refusals-v1. Stable 60 FPS combat remains unresolved.


2026-10-12: The bounded diagnostic run completed cleanly and captured actual room frames 17672/17673. All 26 refusal rows identified missing exact modules; none showed two certified modules with an uncertified PSO. Ordered analysis found 263 eligible material draws between useful early-depth passes. Added exactly five vertex and nine specialized pixel module pins, preserving all previous 178 pins and the runtime predicate. The offline structural auditor gained narrowly constrained signed comparison, conversion and clamp forms, with 197 negative controls and independent review; all 192 certified modules cold-recompile exactly. Release 7,908 checks and debug/ASan+UBSan 7,146 each pass. Nine supporting canonical pixel forms remain excluded.

Fresh main and replay builds pass. Actual-room OFF/ON replay matches all 161 image/raw-memory artifacts, full 10 MiB packed memory and query results; shared conversions fall from 10 to two, with 263 retained draws and only two expected tail refusals. The capture manifest contains one unsupported unused vertex shader; all 231 draw-required variants are separately verified, and the raw manifest failure is preserved. This establishes rendering parity and work reduction, not FPS. A fresh shipping-baseline live comparison is running with diagnostics off and private state. Normal configuration still disables the experiment. Evidence: early-depth-material-retention-20261012/material-cohort-v3 and monotonic-depth-modules-v3-20261012. Stable 60 FPS combat remains unresolved.


2026-10-12: The expanded retention candidate remains OFF. Fresh shipping comparison completed cleanly with normal state unchanged, but shows no useful gain: mean 13.5485 to 13.5568 ms, block median 13.35 to 13.37 ms, p95 14.62 to 14.52 ms. Five small ordinary-material workload gates fail; no checker changed. Live selection improves to 97.23 retained draws/frame but still has 1.49 module refusals/frame and 2.38 conversions/frame versus shipping 1.015. This demonstrates that the two captured frames do not cover all live animation materials. No reverse comparison or user combat replay is justified for this candidate. Decision: material-cohort-v3/live-stage/decision.json.

Next private investigation targets an entire primary opaque phase using authoritative hardware packed-color and D32 attachments, eliminating the packed-memory shader critical section across all compatible materials. Two actual captured phases contain 198/201 draws, 53 unique pixel sources, 20 vertex sources and 65 exact raster recipes. Existing three-material retained rendering is insufficient and its prior regressions remain valid. The 79/81 texture bindings sample separate physical resolve storage produced before each phase; existing preparation preserves retained ownership. A private exact C++ source-adapter inventory is running; no new production integration or FPS claim. Full phase coverage, precision, actual module/PSO ownership and cache budget handling must be qualified, and captured coverage must not be mistaken for complete live coverage.

2026-10-12: Full opaque-phase prototype is implemented behind a default-OFF diagnostic flag; the normal main binary remains unchanged. The first GPU pair is rejected: an inverted front-face field in the offline inventory restricted startup to23 programs/33 PSOs and selected212/399 draws across18 generations. Independent calls to the actual C++ DecodeDrawState for all399 captured draws proved and corrected all65 raster recipes; module bytecode remains unchanged. Build03 succeeds. Two intermediate color components also differ at nearest-even packing thresholds (one final8-bit component differs in frame17672); exact depth resolves, final memory and query results do not establish full image parity. The numerical cause remains under investigation. All failed outputs are preserved under opaque-phase-implementation-20261012/qualification, with corrected raster evidence in opaque-phase-frontface-audit-20261012. No FPS improvement or normal-build promotion is claimed.

2026-10-12: Corrected full opaque replay prepares53 modules/65 PSOs, selects408 draws across7 runs, including both198/201 main phases. An independent full1566-draw C++ state scan accounts for the extra9 draws and all counters (opaque-phase-full-eligibility-audit-20261012). Queries and all14 updated depth-resolve regions match, but33 image/memory artifacts differ sparsely; the candidate remains unqualified. The compiler precision investigation found that unreachable format0/2 packing branches add NoContraction restrictions to material calculations. A phase-only exact format3 packing adapter is now adopted with53 regenerated byte pins, leaving legacy adapters unchanged. Cold compiler checks,53 Vulkan validations,795 effect/ABI mutations,212 source mutations and bounded material SSA correspondence pass; exact GPU parity remains pending. New derived modules total3,391,056 bytes under the unchanged3,432,836 cap. Build05 includes standalone owner-loss and before-clear prefix fixtures; normal main/config remain unchanged and no FPS gain is claimed.

2026-10-12: Full opaque-phase qualification-v3 build05 exits cleanly in both arms but remains rejected by exact framebuffer/memory comparison. Coverage is unchanged:408 draws/7 generations/401 reuses/all65 recipes with53 programs; queries match. Precision adapter removes the old scattered first-resolve errors, leaving15 one-code RGB differences in a tight9-pixel region at x764..773,y281..284. Final packed memory differs by6 bytes across4 words. All161 OFF artifacts remain identical to prior OFF, establishing no baseline drift. Actual selected variant8 material modules also have matching bounded SSA contraction permissions; producer localization via private prefix replays is next. Q3 failed proof is preserved, normal main/config remain unchanged, and no FPS gain is claimed.

2026-10-12: Exact prefix localization proved full10MiB equality after event309 and nine changed color words after actual event310 (PS333ce299..., VS75d5...). Narrowed phase admission for this source only; all53 module pins and65 catalog recipes/capacities remain, ordinary65 pipeline preparation unchanged. A fresh1566-draw C++ scan proves only310/1556 excluded:406 retained draws/9 generations/397 reuses/max180. Release, Debug andASan/UBSan each pass42,919 policy checks. Build06 succeeds with stable sources; strict new full capture and owner/prefix GPU checks pending. Original build05 and failedQ3 remain preserved. Normal main/config unchanged; no FPS gain claimed.

2026-10-12: Full opaque phase Q4/build06 now passes the strict full two-frame comparison: all161 image/memory artifacts and query results exact;406 retained draws across9 generations,397 reuses/max180. Main build01 succeeds with unchanged qualified source cohort (main85932120, runtime267dabf1); experiment remains default OFF and normal configuration unchanged. The combined transition fixture stopped because actual draw290 changes no memory, making its chosen positive witness vacuous. Failed validation is preserved; a bounded actual-draw inspector will select a nonvacuous control. No FPS result or promotion.

The separate private point-UNORM actual-consumer test gpu-01 passes single and six repeated full10MiB/query/queue pairs. Median inclusive queue interval per draw0.0732166→0.0731538ms (~0.086%); individual alternating-order results vary in both directions. This does not demonstrate a useful speed gain. No compact alias integration is justified by this result, and production texture representation remains unchanged.

2026-10-12: Full opaque phase now completes its additional GPU qualification. A controlled actual-prefix289 then immutable308→292 fixture proves visible first depth/color writes, a visible same-pipeline second draw, and that the second cannot mask lost first writes. Actual shader-owner replacement and missing-PSO controls export and refuse correctly; all seven full10MiB owner outputs match their intended oracles. Four continuous OFF/ON captured prefixes also match exactly, with nonvacuous depth/color witnesses and exact406-draw coverage. Combined validation-supplement-v2 passes all15 full-memory artifacts, preserving Q4's161-artifact full comparison. Earlier vacuous-donor and cutoff-bookkeeping failures remain saved. Only the replay fixture helper changed after main01; other linked code and the main binary are unchanged. The first uncapped live OFF/ON comparison is now running with private settings/saves/cache and no performance claim yet.


2026-10-12: Full opaque-phase first live pair is rejected. Mean API-frame time13.3647→18.5813ms; blockmedian13.12→18.38ms. The sole failed frozen functional gate is immediate-marker conservation: the phase flag explicitly disables that established shortcut, producing zero selected markers. Exact timed counters also show waits56.942→64.348/frame and transfer lists101.011→118.696/frame. Phase selects206.23 draws across9.344 generations/frame, has zero residency/resource refusals, and removes the shipping21 early-depth selections/frame. Both cameras match; normal config/saves/main remain unchanged. A short browser spike affects part of ON, but all ON block medians exceed all OFF blocks. No reverse comparison, normal promotion, user combat request or isolated gain claim is justified. Evidence: opaque-phase-implementation-20261012/live-first-decision-20261012 and live-stage/comparison-first.json.

Follow-up is narrowly separated: admit already-owned queued exports at typed draw boundaries while retaining strong CPU/control/query completion; prove phase-owned input safety for existing immediate marker submission; independently measure loss of shipping early-depth culling using preserved build08. All new behavior remains within the default-OFF phase experiment. Stable60FPS combat remains unresolved.


2026-10-12: Implemented narrow phase-only support for existing queued typed-draw exports (five admission guards, unchanged FIFO/shaders/bounds) and removed only the phase veto from existing zero-query owned immediate-marker submission. Normal phase and queue flags remainOFF. Added separate replay fixtures for pending typed export/strong CPU closure and dirty retained SubmitGpuWork→same-generation continuation. CPU guard3401checks and freshly rebuilt SDK coherence4850checks pass; two earlier stale SDK test timeouts remain preserved. Main02/replay09 compile with stable sources, config unchanged; main2b136109/runtime267dabf1/replayd8ea8784. New GPU qualification is running, no FPS gain/promotion.

Preserved-build08 early-depth isolation OFF/ON passes161 exact artifacts/full10MiB/query results. The21 second-frame49f intervals decrease0.83856→0.34168ms; all individually improve, without other child draws in those lists. Dependency-inclusive timing is not a measured live frame-time recovery. Leaving those interior draws outside the phase adds one export/restore and one depth conversion per captured frame; benefit remains unmeasured. Evidence: opaque-phase-implementation-20261012/early-depth-isolation-20261012/analysis-first. No phase admission change from this finding.


2026-10-12: Queued typed phase exports pass the corrected build10 full captured pair:161 artifacts/full10MiB/query results exact;406 selections/9 generations/9 accepted pending exports, peak23,777,280bytes under unchanged32MiB cap, zero resource/residency refusals. The first build09 ON replay rendered all artifacts exactly but failed its legacy report-key postassert; that failure is preserved. Only one replay report-selector expression changed; all other linked payloads and main02 remained exact.

The supplementary closure fixture then failed because it incorrectly expected an early TakeCompletedQueries call to return empty in all-work retirement mode; the existing API correctly rejects this premature consumption. Build11 changes only the test helper to demand the exact rejection while retaining the pending ticket and original later full-memory/query assertions. All non-helper linked inputs and main02 unchanged. A fixtures-only supplement authenticates the successful build10 full proof using its preserved binary and old helper preimage; no rendering implementation changed and no duplicate full replay is required. Corrected closure/marker GPU results pending. No FPS gain or default promotion.


2026-10-13: Corrected GPU query/marker supplement passes5 queued-export and8 marker full-memory artifacts, plus preserved161 full artifacts. Restored markers are observed live with zero provider refusal. First live-v2 queued arm was rejected before game launch by outdated diagnostic CLI guards. Its completed queueOFF arm and failed launcher are preserved. A hash-bound private launcher changes only repository-root discovery and the two phase/queue admission guards; actual argparse/control checks pass all3 arms,32 combinations and10 incompatible cases. Original production tool remains unchanged.

Fresh live-v3 three-arm screen completes all arms normally, matched6 camera screenshots, strictNative/all query retirement and normal state preservation. Mean19.2049ms phase/queueOFF,17.3795ms phase/queueON,15.0344ms shipping; queued phase is still15.6% slower descriptively. Frozen analyzer fails exactly its bounded-resource gate:2,648 image allocation refusals/1,560frames, with5,296 budget refusals. Peak31,703,040bytes=4generations under32MiB cap. Queued waits57.77/frame and sampled submit_wait3.463ms are near shipping56.97/3.546, so generic extra waits do not explain the remaining regression. QEMU contributes~213%one-core externalCPU in all arms; no causal percentage gain/promotion or reverse test claimed. Decision/evidence: opaque-phase-implementation-20261012/live-three-arm-decision-20261013.

Next candidate reuses one immutable conversion scratch/descriptor lease through the same NativeFIFO, instead of two fresh scratch allocations per generation. It remains phase-only/defaultOFF, keeps per-conversion transfer reservation, fresh textures and original32MiB image bound, and requires exact batch/canonical/resource owners. Two-pass stencil-assisted culling is only an architectural audit; no implementation adopted because allocator pressure and late-depth cost still need separation. Stable60FPS combat remains unresolved.

2026-10-13: Shared conversion scratch is implemented behind the default-OFF phase-only flag. Release/debug/Clang ASan+UBSan each pass85 admission/ownership checks. Replay12 and main03 build with the same496-file Native source cohort; normal settings/saves/cache unchanged. Replay d755f867, main82484890, runtime267dabf1; preceding replay11/main02 are preserved. The initial build-script preflight stopped before compilation because the old20-file fingerprint omitted Writer; the corrected receipt expands source/header coverage. No renderer build failure was hidden.

Fresh shared-scratch-qualification-v1 passes the same-binary OFF/ON capture:161 bit-exact image/memory artifacts, full10MiB packed memory and queries,406 selected draws/9 generations. Sharing uses one allocation/17 reuses for9 restores+9 exports, with no sharing/resource refusal. Both arms retain queued exports. The separate queued closure and marker fixtures pass5+8 full-memory artifacts and prove actual nonzero immutable scratch/descriptor/canonical identities across submission and continued drawing; premature query access still rejects correctly. Existing barriers,32MiB image cap,128MiB transfer accounting and fresh image ownership remain unchanged. This is correctness/work-reduction evidence, not an FPS result; normal phase/queue/sharing stayOFF.

The private live-shared-scratch-v1 three-arm comparison is prepared and independently reviewed, awaiting clean host timing. A fresh5-second QEMU sample consumed11 CPU-seconds; the user's pause response remains pending. No timing comparison or causal performance gain is claimed. A separate report-local pressure-poll candidate targets GPU-complete image owners still charged before allocation; it preserves original exact admission and adds no GPU wait or submission. Its69 CPU policy/ownership checks pass after a test-only indentation fix; the initial strict-compiler failure remains preserved. Production does not yet contain this second candidate. A focused real-fence pressure witness is being built. Stable60FPS combat remains unresolved.

2026-10-13: The direct GPU pressure fixture passes on RX9070XT. Four completed but unretired image pairs retain31,703,040bytes; one nonblocking poll releases them and permits the next allocation, without another submission or wait. Separate staging-only and external-owner controls correctly refuse admission. All four10MiB artifacts match. This fixture proves the queue/owner mechanism, not natural Scene pressure or FPS improvement.

Pressure polling is now integrated as a default-OFF phase option. Replay13/main04 build successfully with the same498-file source cohort; prior binaries/sources are preserved. Fresh pressure-poll-qualification-v1 OFF/ON full-scene comparison passes all161 image/memory/query artifacts. This capture does not reach pressure and records zero polls; positive Scene coverage still needs the private live diagnostic. Closure/marker checks are in progress. Normal configuration/saves/cache remain unchanged. Active QEMU still prevents a clean timing comparison; the upcoming automated run is for allocation/retirement counters and correctness only. Stable60FPS combat remains unresolved.

2026-10-13: Pressure polling also passes all5 closure and8 marker full-memory checks, with actual immutable shared bindings and unchanged premature-query rejection. The private live-pressure-counts-v1 run completes normally and passes all Native, queue, query, budget and input-preservation checks. It observes4,389 pressure polls:935 allow admission after actual retirement;3,454 still refuse with unchanged image bytes. Total reclaimed bytes10,279,710,720 are cumulative across the run, not peak memory. The two held-camera screenshots show the expected room without obvious corruption. These counts establish natural Scene coverage and remaining pressure, not FPS benefit.

QEMU had exited before this live run; all174 host samples observe no QEMU. No user pause acknowledgment was inferred. Fresh matched timing runs can proceed after checking current competing work. Normal phase/queue/share/poll options remainOFF. Source audits also identify redundant depth-image creation after color allocation refusal, conservative repeated shared-scratch accounting, and a32MiB image limit that is allocation policy rather than an Xbox correctness requirement; none of those follow-up changes is implemented yet. Stable60FPS combat remains unresolved.

2026-10-13: Current main04 shipping-versus-phase/queue/share/poll first pair completes both arms normally, with matching held-camera views and unchanged normal state. All functional/workload checks pass. Mean stationary API-frame time is14.4198ms OFF and16.2244ms ON; median block-p95 is15.89 versus17.09ms. External CPU/GFX activity is lower ON (42.7% one-core/4.16%gfx versus63.7%/5.90%), so the host-comparability gate is explicitly failed and no isolated regression percentage is claimed. The candidate nevertheless shows no benefit; reverse and user combat trials are not justified for this version. Normal options remainOFF. Evidence: live-phase-pressure-pair-v1 and phase-pressure-pair-host-review-20261013.

The next report-local candidate reuses one immutable color/depth/framebuffer set across accepted FIFO generations. Distinct logical authority tickets, exact restore/export acceptance acknowledgments, clean unused-generation closure and rejection of stale Prepared work guard reuse; original GPU conversion commands/barriers remain byte-exact. Its pure policy passes242 checks each in Release, debug and Clang ASan/UBSan (LSan disabled). Writer/Scene compilation and actual GPU reuse proof remain pending. Build13/main04 binaries and all498 source inputs are preserved before adoption. Stable60FPS combat remains unresolved.

2026-10-13: Single attachment-set reuse is adopted behind default-OFF gow3_native_share_retained_opaque_phase_attachments after independent source review. It requires the separate retained phase and shared scratch. Eleven files changed, including diagnostic/replay wiring and phase-OFF fixture resets; no normal config/save/cache changes. Replay14 and main05 build successfully with the same500-file source cohort: replay4693092c, maina0721fdc, runtime267dabf1 unchanged. Original conversion GPU commands, phase admission and strong completion paths remain unchanged. Focused real-GPU ownership/reuse and full-scene parity checks are still pending; no performance benefit or default promotion is claimed.

2026-10-13: Single attachment GPU fixture completes successfully on RX9070XT. Independent postcheck matches all ten full10MiB artifacts (eight generation outputs plus two poison witnesses) and verifies actual same physical owners remain alive across FIFO reuse. Original wrapper failure is preserved: its blanket minimum12 stale-request checks was incorrect for the clean-first arm (11 versus dirty13); corrected check verifies exact ordered refusal labels and unchanged frontiers. No production change or GPU rerun was needed. Current replay14 shared-attachment-qualification-v1 OFF/ON passes all161 artifacts against current and preserved prior outputs,406 draws/9 generations/397 reuses. ON uses one actual7,925,760-byte image pair,8 reuses/9 logical closes; original budgets and Native-only rendering remain intact. Query closure execution passes; marker/full fixture postcheck pending. No FPS benefit or promotion claimed.

2026-10-13: Current shared attachment qualification also passes5 closure+8 marker artifacts. The live-attachment-pair-v1 comparison completes both arms with matching held-camera screenshots and1874 functional checks passing. Candidate keeps exactly one7,925,760-byte pair and eliminates resource refusals; it performs7.2905 restore/export pairs per API frame. Observed mean14.0125ms shipping versus16.075ms candidate and block-p95 median15.42 versus17.315ms do not demonstrate improvement. Host-comparability gate explicitly fails: reported external CPU37.67%→61.67% of one core and external gfx4.68%→9.76%. No attributable slowdown percentage or combat/60FPS claim. Keep normal phase/queue/share/poll/attachments OFF; reverse deferred while measuring conversion and CPU predicate costs. All500 current source inputs and replay14/main05 binaries are preserved before diagnostic-only work. A separate one-draw immutable classification prototype is source-reviewed but remains report-local and unbuilt.

2026-10-13: Added diagnostic-only retained_phase_restore/export work tags (original enum IDs preserved) and cumulative RetainedOpaquePhaseDrawAllowed CPU time under existing timing switches. Rendering/ownership/command bodies unchanged; independently source-reviewed. Replay15/main06 build successfully with the same500 source inputs, main557239a2/replay88987fe3/runtimeunchanged. Existing GPU attribution CPU test passes2082checks, and one untimed currentON captured replay matches all161 AQON artifacts/full10MiB/query values/behavioral counters; host fence-observation counts are explicitly excluded. Private phase-cost-diagnostic-20261013 run is frozen and running with existing sparse physical GPU timestamps/CPU-stage timers, original uncapped route and unchanged normal settings. These intervals are inclusive and may overlap; no additive GPU/CPU or performance improvement claim.

2026-10-13: Both phase-cost diagnostics finish normally with unchanged normal state and matching held-room camera screenshots. The ON held window spans1680 API frames; predicate CPU time is0.6334ms/frame. Fourteen sampled GPU frames have0.5893ms combined restore/export interval union,9.6184ms total selected-list union within16.8338ms envelope. Overlapping intervals and unsampled work prevent treating the difference as idle CPU or exclusive GPU cost. All2862 sampled phase draws are separate one-child physical lists (~204/frame); the source also restarts the attachment render pass for each one. Shipping reference overlaps a Gast/Wine/xalia process, so it is diagnostic evidence, not a clean FPS comparison. Normal phase options remainOFF and stable60FPS combat is unresolved.

A separately reviewed, default-OFF phase render-pass grouping candidate is adopted for qualification. It keeps compatible draws in one exact logical generation/attachment pass, retains each draw's owners/token and the existing16-draw bound, and preserves transfer/query/CPU/pressure/failure boundaries. Same physical images across different restore generations cannot share a pass. No shader, precision, queue-budget or normal settings changes. Initial strict-compiler CPU test passes120 policy/ownership checks; it does not prove GPU ordering. Actual follower-failure, full-memory rendering and live timing qualification remain pending. All500 main06/replay15 sources and their binaries are preserved before adoption.

2026-10-13: Phase grouping replay16/main07 build successfully with the same504-source cohort (replaybeaddfdb, main9db1f2ee, runtimeunchanged). Current CPU targets pass120 phase-policy and29330 bounded-group checks. The root adoption preflight initially counted a replay file twice (13 versus12 unique changed/new files); receipt creation stopped and no compiler ran before that bookkeeping correction. Source and failure-test receipts preserve the correction explicitly.

The frozen phase-pass-groups-candidate-20261013/qualification full OFF/ON comparison passes all161 artifacts, full10MiB packed memory and exact query results against each other and the preserved prior oracle. Both arms retain406 phase draws/9 generations/397 reuses. ON groups321 draws into45 passes, avoids276 pass restarts and1825 sampled read barriers;85 preparation draws remain standalone, maximum group16. Total physical lists fall1204 to928 with unchanged1566 logical draws,217 transfers and292 submissions. This is work-reduction/correctness evidence, not an FPS improvement. Supplemental transfer/query tests execute successfully; resource replacement and fatal follower controls are still pending. User reports competing games closed for subsequent timing; actual host monitoring remains required.

2026-10-13: Supplemental phase-pass-group qualification now passes all5 closure,8 marker and15 owner/prefix full-memory artifacts, plus the actual poisoned-follower/destructor checks. Original follower execution failed exit86 because its command omitted existing --retire-query-prefixes, making the asserted poll operation a disabled no-op. Same-binary GDB confirms that specific test-contract error and no post-fault submission. The original failed receipt/log remain preserved; follower-v2 adds only that existing flag and fresh private paths, passes all14 unchanged rejection checks and expected destructor exit85. No production source/binary change or weakened assertion. Aggregate follower-v2/fixtures-parity-v2.json revalidates the original successful fixtures and full161 proof. Clean current16/main07 shipping-versus-all6-phase-controls comparison is prepared; no new FPS benefit or promotion yet.

2026-10-13: Current pass-group live first pair completes normally and1862 functional/workload checks pass. Both cameras match. Mean14.9875ms shipping versus14.6828ms all6-phaseON, median block-p95 16.425 versus15.75ms. The apparent2.03% gain is below the3% screen threshold and not attributable: external browser/compositor load differs materially (external gfx11.89%→2.26%, reportedCPU85.48%→20.81% one core); host-comparability gate is explicitly failed. No other game/compiler observed. No reverse/default promotion or combat60FPS claim. Preserve source504 and replay16/main07 before a bounded draw-local immutable classification optimization; phase options remainOFF.

2026-10-13: Adopted a bounded draw-local phase classification optimization plus existing-gated accepted-bookkeeping timing. Three files changed/added: Scene, immutable facts helper, focused CPU test. All110 strict release checks pass (input/state binding, static false/true reuse, exceptions and independently changing actual resident VS/PS/PSO callback decisions). Facts exist only within an owned typed Draw; public preflush and JSON/provider-only inputs retain independent evaluation; dynamic query/writer/debug guards and final actual PSO/module certificates remain live. Independent source review passes. Diagnostic clocks are gated OFF normally and preserve the four SurfaceVersion expressions and original writes/authority order. Current source504/replay16/main07 copies preserved before adoption; build17/main08 and one full161+owner15 qualification pending. No new normal setting or FPS claim.

2026-10-13: Draw-local classifier replay17/main08 builds pass with identical506-source cohorts (replay7d37b073, main2bd931a8, runtime267dabf1 unchanged). Single all6ON current replay matches all161 preservedPGON artifacts/full10MiB/query results, phase/grouping/shader work and startup counters exactly. Current owner/missing-PSO/prefix fixture passes15 full-memory artifacts and the original nonvacuous/dynamic-residency assertions. CPU diagnostic is frozen/launched, using only existing CPU-stage timing and private uncapped720pSDR route; no GPUtimestamp, video or profiler. No isolated FPS gain or normal setting change claimed.

2026-10-13: Main08 CPU-only diagnostic completes normally with matched held-room screenshots and unchanged normal state. Across1800 anchored API frames,205.158phase draws/frame: predicate0.426579ms/frame (includes live lookup/timer overhead); accepted bookkeeping0.647721ms/frame; nested precolor0.145466, predepth0.143432, postcolor0.142354, postdepth0.141896ms/frame. The four queries total0.573148ms within bookkeeping, not additive. Two post-write queries account for0.284250ms measured inclusive time, supporting an exact assigned-serial receipt investigation; no scan removal implemented yet. Earlier predicate0.6334 had different build/timing/host circumstances, so the difference is diagnostic evidence, not clean FPS attribution. Current normal flags remainOFF; fresh no-clock17/main08 comparison prepared.

2026-10-13: Current17/main08 timing pair completes with matched four room screenshots and1892 functional/workload checks passing; host-comparability check failed. Mean13.986458ms shipping versus14.923333ms phaseON; median block-p95 15.255 versus15.7ms. Extra Steamwebhelper/desktop activity in ON (externalCPU35.84%→64.76%onecore;gfx4.96%→6.73%) prevents causal attribution; candidate does not demonstrate benefit. No reverse/promotion. Preserve506source files/replay17/main08, then adopt exact selected-phase primary write receipts to remove measured redundant post-version scans. Strict CPU1320checks and independent source review pass; five files changed/added. Existing assignments/alias invalidation/change tracking/prequeries stay intact; actual footprint/current owner/epoch/adjacent nonzero write serial guards otherwise fall back to original queries. Build18/main09/full-memory qualification pending.

2026-10-13: Exact primary write receipts replay18/main09 build successfully with508 source inputs (replayfeb4a1cd/main0c5dece1/runtime267dabf1 unchanged). Current all6ON replay passes all161 image/memory artifacts, full10MiB and exact query results against CL2. Original phase behavior matches after excluding only the new receipt subrecord; receipt selection/fallback conserves accepted phase draws. Owner replacement/missing-PSO/prefix fixture passes15 full-memory artifacts and17 phase observations. Existing strict1320 CPU checks remain passed. Private CPU-only diagnostic launched with uncapped learned route; normal configuration/saves remain unchanged, all experimental controls defaultOFF. No FPS gain claimed.

2026-10-13: Main09 CPU diagnostic completes cleanly with matched room/camera screenshots and unchanged normal state. Across1680 anchored API frames,205.187phase draws/frame: accepted bookkeeping0.400476ms/frame vs prior CL2 diagnostic0.647721; predicate0.471560; nested precolor0.152188/predepth0.150664/postcolor0.003885/postdepth0.003654.344714 receipt uses, zero fallback and no resource/residency refusal. Enclosing total includes pair-proof cost; nested scopes must not be added. Separate instrumented runs/host activity do not establish FPS gain. Current no-clock shipping-versus-phase first pair next; all phase controls remain defaultOFF.

2026-10-14: WR current18/main09 first no-clock live pair completes cleanly with2272 checks passing, matched four room screenshots and comparable observed host activity (externalCPU27.86→28.66%onecore;gfx4.710→4.981%;no competinggame/compiler/reset/unmatchedGPUinterval). Mean13.956771ms shipping versus14.272917ms all6ON, medianblockp9515.44→15.47, max21.32→23.45; no>50ms held-window events. Candidate is2.265%slower, fails3%improvementgate, so no reverse/promotion. Normalstateunchanged. Earlier QEMUpreflightblockedlaunch untilactualguardclear. The CPU receipt local reduction does not establish whole-frame benefit.

Report-local conservative-depth V4 candidate now has exhaustive all16,777,216-code arithmetic proof (134,217,727checks) and independent source reviews.53 actual cold-derivedmodules passVulkan1.2,1113effect/mode mutations,212source mutations;3,410,136B fits unchanged3,432,836B cap. All53 bounded canonical shared instruction regions preserve precision;24 legacy-V3 region mappings are structurally incomplete and explicitly do not prove whole-material equivalence. New exact lower-cell/tail encoding preserves all24-bit codes inD32[0,1] and declares DepthLess, with matching separate restore/store modules. V3tables/version untouched; V4 versionbe6df7dd. No productionadoption/mainrebuild/GPUparity yet; normalflagsOFF. Private actualWriter all-code conversion fixture is compiling after aligning its shader target toVulkan1.2.

2026-10-14: Conservative V4 actual GPU conversion qualification passes onAMD RX9070XT:19 realWriter restore/D32-readback/poison/store rounds coverall16,777,216 codes; everyfull10MiB/stencil/color/outside-bank artifact exact. Exact extracted HLSLhelper+canonicalpackDepth computeprobe also matchesindependent canonical-Q binarysearch oracle. One sharedscratchallocation/37reuses; peakimages7,925,760B; no claimaboutfragmentearlyrejection orsharedattachmentreuse fromthisfixture. Actualsource/math/mode strict-release test passes150,996,463checks,53sourceprojections/424mutations/allcodes.14reviewedproductionfiles adopted (3newconversions), preservedall508oldsourceinputs/replay18/main09/runtime513entries. Newphasevariant25/versionbe6df7dd;V3pins/versionunchanged;all6flagsdefaultOFF. Replay19buildrunning; fullcapturedGPUsceneparity/performancepending.

ConservativeV4 replay19 andmain10 builds pass withsame511-sourcecohort; replay8c303fcc,main445f8b5c. Mainnormalconfig/user unchanged. Runtime d10a1118 changedfrom267dabf1 after CMake regeneratedtimestamp andrecompileddebugoverlay; ELFcomparisonidenticalcodesections, onlybuildID/lrodata differ, boundedtimestampauditpending. Fullcurrentcaptured-framequalification preparation underway.

2026-10-14: Main10 runtime delta is now bounded exactly: 20 GNU build-ID bytes plus 4 timestamp digits; every other ELF byte is identical to preserved main09 runtime. Current-runtime coherence execution passes all4,850 checks and123 header dependencies, with actual loader path verified. Initial loader preflight failed on the literal (Xbox) path parser before any test; wrapper/probe evidence preserved under conservative-phase-candidate/sdk-current/loader-parse-failure-01, address-anchored parser corrected. V4 qualification dry authority passes511 current sources; full GPU parity pending. No normal config/user change or FPS claim.

2026-10-14: V4 full captured replay passes161 exact image/memory artifacts, full10MiB packed memory, query results and unchanged behavioral/recipe/group counters. Separate owner/missing-PSO/prefix run passes15 exact full-memory artifacts and17 explicit V4 identity observations. Manifest d2668939; full/owner exits0 with unchanged sources/binaries/config/user. Live19/main10 stage dry authority passes45 frozen inputs; independent review pending before launch. New source census identifies272 already byte-audited variant8 draws/60 exact tuples as a possible separate packed-color pass continuation investigation, upper245 avoided passes overtwo frames, but live owner/preparation eligibility and benefit remain unproven. Normal six phase controls remainOFF; combat60FPS unresolved.

2026-10-14: V4 live first pair completes both arms normally, matched held-room camera and unchanged normal state.2322 functional/workload checks pass; sole failed gate is host comparability. Shipping mean14.027941ms vs V4phase14.615000ms (4.185%longer); medianblockp9515.33vs15.59, max23.39vs23.19, no>50ms measured events. ExternalCPU48.48→46.17%onecore but externalgfx3.38→6.52%(mainlylabwc), so no isolated regression percentage is attributable. Candidate demonstrates no benefit, no reverse/no promotion. Allsixnormalphaseflags remainOFF. Next bounded investigation: exact-state variant8 packed-color pass continuation, with actual module/PSO ownership and dynamic interlock proof pending.

2026-10-14: Packed-color variant8 whole-entry proof first rejects all50 raw CFGs, including impossible alpha-test/switch paths; failures preserved. Official SPIRV-Tools2026.1 exact17-value specialization plus bounded literal/Phi/Select switch successor projection proves43PS/53 exacttuples/254 captured draws.7PS remain unproved and excluded.5 reachable pairing mutation controls and independent73-check subset review pass. Current shaderbytes remain unchanged; optimized copies are offline proof artifacts only. Static upper228 avoided passes overtwo saved frames is not actual eligibility or FPS evidence. Minimal default-OFF53tuple candidate and actualScene fixture are report-local work in progress; all511 current19sources/replay/main10/runtime/receipts preserved516entries before any adoption.

2026-10-14: Adopted default-OFF exact53-tuple variant8 packed-color continuation and diagnostic owner fixture (5 files,2 new). Focused CPU policy passes1885 checks. Replay20/main11 builds pass513-source cohort; normal config/user and runtime d10a1118 unchanged. First actual GPU Scene invocation passes primitive33draw/3pass oracle and nonvacuous baseline textured overlap (13457 differing color words), full10MiB and positive queries. Candidate ON is blocked before execution by an obsolete material-composition/packed-color compatibility guard. Failed actual-run evidence retained; positive grouping, full161 parity and FPS benefit remain pending. Entry-point integration audit/fix in progress, no normal enablement or60FPSclaim. Evidence: conservative-phase-candidate/packed-color-variant8-census-20261014.

2026-10-14: Reviewed integration correction removes only four obsolete packed-color/composition vetoes plus diagnostic/test wording (5files);269 guard checks and3305 historical queue checks pass. Replay21/main12 pass513-source cohort, runtime d10a1118 unchanged, defaultOFF. Actual Scene fixture v2 passes6369checks acrossfive modes, nonvacuous actual texture overlap, full10MiB/query/resolve parity and independently replaced module/PSO refusal controls. Full OFF/ON saved replay passes161artifacts/query/work exact: physical lists1188→948;280 grouped draws/240 continuations (V8:203selected/171continuations), zero owner refusals. Normalstateunchanged. First private live packed-only uncapped comparison now running; actual host preflight clear. No performance/promotion claim yet. Evidence: conservative-phase-candidate/packed-color-variant8-census-20261014/proved-packed-color-candidate.

Packed-color first live pair passes1444 checks: held first-room mean14.075→12.913235ms (-8.254%frame time), medianblockp9515.485→14.01ms, max30.77→19.94ms. Matched4camera screenshots and guestwork delta<0.36%; no heavywork/reset/unmatchedGPUinterval. Modestdesktopload favorsON (externalCPU32.28→27.84%onecore,gfx4.85→3.09%), so reverse underway before any repeatable claim. Originalnormalstateunchanged. User available for fresh fullscreen turret/tank/panning test; CPU-stage and draw-recording diagnostic prepared but not launched.

2026-10-14: Packed-color reverse live comparison passes1428 checks with matched held-camera views and no competing game/build/VM observed. Mean frame-time improvement5.1847%, median block-p95 improves6.7003%; first order improved8.2541%. Ordinary desktop residual favorsOFF in reverse (externalCPU42.35OFF/53.39ON percent of one core;gfx4.56/5.80%), unlike first order. Paired summary reports repeatable_stationary_improvement=true; this first-room stationary screen is not combat or4KHDR qualification. All normal state unchanged.

User accepted fresh fullscreen test. Reviewed conditional packed-ON revision adopted only in private combat-recording-cost-20261014/fullscreen-01;549 boundfiles/2816 seededfiles, freeze and launch validation pass. Currentmain12 launched PID1437070, uncapped/VSyncOFF/nativeexclusive,4KHDR3840x2160 confirmed, existingCPUstage+drawrecording clocks enabled from startup; no video/profiler/GPUtimestamps/hostsampler. User notified before route. Production default staysOFF pending combat results. Actual timer rows start atAPI119; test in progress, no combat improvement claim.

2026-10-14: Fullscreen packedON CPU diagnostic exits0/clean111.56s; all health/input-preservation checks pass. Native-only counters allzero, all570768queries retired,4KHDR observed. Actual packed selected758764/continued637565, V8selected509279/continued407181,0module/PSOrefusals.64recordingwindows/65sampledDrawsnapshots saved. Final stable-looking block medians~14.5ms; slower nearby blocks16.87/17.47ms. User confirms MUCH better overall/idle above60, remaining modest combat/panning dips; no exactFPS minimum supplied. Begin/reset~0.146ms and end~0.097ms perAPIframe are small; enqueuehost~1.68ms, sampledsetup~5.8ms includingtextures~1.6ms; slow sampledsetup9.148ms includes3.970mstransienttexturework. Nested/sampledtimers are not additive or exact action labels. Stable60combat remains unresolved.

Qualified old53-tuple packed-color path promoted for normal launches by adding only gow3_native_packed_color_render_pass_groups=true to config/gow3.toml. Config2a69ebba→30b95c8f; original/after snapshots and exact proof/build/run identities saved in proved-packed-color-candidate/normal-launch-promotion. Same testedmain12/replay21 binaries, no source/rebuild, no diagnostics enabled normally, otherconfig/saves/cache unchanged. Low-level diagnostic defaults remainfalse. New108-draw/two-blend-state extension only under report-local proof; no admission broadening yet.

2026-10-14: Completed combat timing assessment confirms command begin/end costs are small (~0.24ms/APIframe). Slow3239→3479 windows sampledsetup7.990ms includes3.116mstransienttexture work; late4559→5759setup5.878ms includes1.560mstextures. Budgetevictions8.20/frame do not prove reloads; prior exactmatchedreloadcost small, retain currentlimits.

Adopted minimal sequential PipelineLess comparator after root review and optimized+ASan/UBSan4,966,316ordering/map/mutation checks; LSan ptrace failure preserved, successfulsanitizer run disablesonlyleakdetection. No keylayout/ownership/cache/shaderchanges. Microbench54–67ns/lookup saved, noFPSclaim. Preserved519currentbaselinefiles/513sources/main12/runtime/replay21/config. Replay22(d9e99986) andmain13(46691c22) buildclean withsame513sourcecohort; runtime d10a1118 andnormalconfig30b95c8f/saves unchanged. ExactcurrentcapturedGPUparity and matchedpackedON old/newbinary comparisons are being prepared under pipeline-key-comparison-20261014. Normalpackedgroupingenabled; stable60combat stillunresolved.

2026-10-14: Sequential comparator current replay22 passes161 exact captured artifacts/full10MiB/query results and identical logical shader/cache/group work against preserved packedON replay21. Initial old/new live comparison stopped before game startup: preserved old executable lacked its $ORIGIN libdxcompiler.so (loader exit127, no native frames). Failed frozen stage and logs preserved; separate v2 dependency-bound comparison being prepared. No FPS result from that launch failure.

2026-10-14: Corrected comparator live-v2 (preserved DXC plus static72-library resolution, same harness/controls) completes both arms cleanly.1428 functional/workload/host checks pass with matched held-room screenshots. Mean12.136→12.002ms (1.1023% improvement), medianblockp95 -0.7675%; below3% screen threshold, no reverse/repeatable FPS gain claim. ExternalCPU20.863→20.832%onecore andgfx2.317→2.422%, no competing game/build/VM/reset/unmatchedclient. Minimal sequential comparator remains correctness-qualified; not counted as a substantial performance gain.

Adopted reviewed defaultOFF exact19-tuple blend packed-pass extension (9 files,4 new;517source cohort). Separate28 immutable module pins and current-module versus creation-PSO guards, old53/legacy/monotonic certificates unchanged. CPU1625 checks and independent source reviews passed. Preserved current22/main13/runtime/DXC and all513 prior sources before edits. Actual alpha/additive/mixed-pass Scene fixture includes donor texture warmup and raw counter deltas, strict overlapping-color/nonvacuity, full10MiB/query/resolve and owner-replacement controls; input75files/3modulecopies prepared. Replay23 build started; no numerical GPU or FPS approval yet.

2026-10-14: Blend replay23/main14 and standalonefixture build pass517sources; main b0ab99bc/replay5bd1cfc2/runtime d10a unchanged. Actual Scene GPU fixture passes12,615 checks,8 full-memory/resolve/query comparisons across alpha/additive5modes each. Alpha14,076same-word noncommutative/both-contribution witnesses; additive952unsaturatedthree-step witnesses andpositive33rddraw; cap16 and mixedold53↔new19 continue bothdirections. Independent currentmodule/creationPSO refusalcontrols pass without alteredoutput; rawwarmup/finalcounterdeltas preserve exact81blend/19old selectedcontrols. Source/normalinputs unchanged. Newblend remainsdefaultOFF; fullcaptured161parity andperformance pending.

2026-10-14/15: Blend current23 OFF matches all161 preserved22 artifacts, full10MiB/query/logical/cache/physical work. Newblend ON/OFF full161 parity also passes exactly, with86eligible/77selected (60alpha/17additive),9 preparationstandalones,42 continuations/physical lists948→906,zero module/PSOrefusals and cap16 preserved. No shader changes or normalsettingpromotion. Same-main14 uncapped live comparison reviewed/preparing; only newblend toggled, oldpackedON/sixphaseOFF. Numerical work reduction is not yet an FPSgain.

2026-10-15: Blend first live pair completes both arms normally with matched four camera screenshots and preserved normal state. 1409/1410 checks pass; host comparability explicitly fails. Apparent mean improvement7.2513% and block-p95 reduction7.8633% are not attributable: OFF external CPU131.4% versus ON37.4% (one core=100%), mostly Discord69.2% versus0.15%, externalgfx9.25% versus3.42%. No competing game/build/VM/reset/unmatched-client flags; workload difference within0.51%. Preserve the confounded pair; no reverse/default promotion from it. Fresh same-build comparison preparation underway. User reports much improved overall/idle above60 with old53 grouping, but modest combat/panning dips persist; no exact minimum or artifact verdict supplied.

2026-10-15: The fresh new19 blend grouping comparison passed functional/workload/host checks (1456) but showed no useful gain: mean frame time increased 1.1864%, median block-p95 increased 2.7957%. No reverse test or promotion; new19 remains disabled. Old53 packed-color grouping remains enabled from the earlier two-order improvement. Stable 60 FPS during combat/panning is still unverified.

2026-10-15: Built UI revision 02 with L3+R3 replacing Guide for settings; both buttons must release before rearming, F4 remains. Added asynchronous read-only compositor VRR status to both settings interfaces. Following the user's clarification, removed the proposed VRR cvar, display-changing API, restoration leases, and all output configuration requests. The game leaves desktop policy unchanged. Initial real toggle testing had shown way-displays immediately restoring its desired VRR-on policy; both displays ended with their original enabled state. No daemon adapter or persistent desktop configuration change was made.

Read-only validation: 117 input/settings assertions in 10 cases, renderer settings/navigation check, 24 standalone UI tests, 65 actual-runtime observer checks, and 24 mock protocol checks pass. Rebuilt probe reports both DP-2 and HDMI-A-1 enabled; this is configuration status, not proof of variable scanout on each frame. Main SHA82153174; all517 Native renderer source inputs unchanged. Full receipts under combat-gpu-after-learned-20261011/ui-vrr-controls-20261015, build-ui-vrr-02.json. A new fullscreen manual test is being prepared with graph-only MangoHud, no limiter/profiler/video, and no MangoHud VSync override; the earlier private vsync=0 override requested adaptive VSync, not off, and was unsupported. This does not establish a cause for previous combat drops.

UI revision02 clean fullscreen test launched PID2191082 (session17353), owned foreground/fullscreen confirmed, final HDR surface3840x2160. Runtime read-only VRR status reports HDMI-A-1 available/enabled, no pending/error. User notified before the route; requested combat minimum/idle FPS and L3+R3 confirmation. Run remains active; no performance result yet.

2026-10-15: UI02 clean fullscreen run completed normally (exit0,246.625s), native-exclusive health and all query retirement passed; normal config/saves/cache and frozen inputs unchanged. No CPU/GPU timing probes, video or CPU profiler. User reports two46FPS dips, one at turret fight and one at tank explosion; no idle minimum supplied. Runtime confirms L3+R3 menu toggle and read-only VRR enabled onHDMI-A-1. This confirms combat slowdown persists under minimal monitoring; no new FPS benefit claimed. Preserved currentUI02/main/runtime/DXC/replay23 and517 native sources before next candidate. Action shader state admission and exact adjacent shader-constant-bank reuse investigations continue under combat-gpu-after-learned-20261011.

2026-10-15: User confirms two46FPScombat dips in minimal-monitoring UI02 run. Split constant-bank reuse rejected: all three boundedCPUvariants slower, final+5.42% across7rounds despite47.3% repeatedfloatbytes; noSceneintegration. Exact action-state overflow lookup adopted defaultOFF (3files) after currentcompiler/4374x3policychecks; main149d4aab builds, runtimeaa4e unchanged. Private headless startup closesnormally70.8s and321checks pass:255statePSOs/209derivedmodules/16,759,960B, exacttargetfound=loaded=1, original732persistent+121overflow readiness/noeviction preserved. Normalconfig/cache unchanged. All existingcapturedrooms lack1a70 target, so noFPSor targetrenderingparity claim; candidate remainsOFF. Useravailableforfocusedfightcapture. CompleteNativeframecapture withactualGPUhistory/orderedCPUwrites/queries/oracles is being staged to avoid incompleteAPI-v6 reconstruction.

2026-10-15: Native completed-frame capture adopted, disabled by default. Saves two consecutive CP-completed frames with immutable CPU/shader owners, ordered writes/copies/actual query controls, real pre-frame GPU history and per-frame full-memory/query oracles. Writer90 optimized+90ASan/UBSan/LSan and reader158 tests pass; sandbox LSan failure preserved and identical binary passed escalated. Main6a4b9eb1/replay16c95018 build successfully after avoiding Xlib Bool macro collision. Automatic first-room capture frames16443/16444 saved2411operations/1617draws/321queries. Actual native replay matches each frame exactly:33,062,592physical bytes in1266ranges,10,485,760EDRAMbytes and all query identities/counts;23checks pass and source screenshot inspected. Target1a absent as expected, so this proves capture/replay mechanism only, not action optimization or FPS benefit. Artifacts: combat-gpu-after-learned-20261011/native-action-capture-20261015.

Private fullscreen manual-fight-01 launched PID2252959/session32348 with current main, native exclusive, uncapped/VSync off,4K/HDR and MangoHud graph. Capture armed from startup for1a70 pixel source, action-state lookup stillOFF. User notified before gameplay and asked to replay turret/tank then close normally. Normal settings/cache/saves unchanged; no recording or profiler. This capture introduces brief intentional readback pauses and is not a timing comparison. Need inspect actual captured target pair/state, establish baseline parity, then compare the defaultOFF action specialization before considering promotion. Cached-hash CPU candidate remains staged/unadopted.

2026-10-15: User closed manual-fight-01 normally; exit0 after336.923s, native health and normal-state preservation passed. Captured actual fight frames2352/2353 contain3008 operations,2294 draws,295 queries and80 exact c2c62310/1a70c066 action draws. The very large reported stall overlaps intentional capture readbacks (maximum456.42ms); it is not a normal FPS measurement. Subsequent120 frames contain no>50ms event. The earlier clean combat slowdown remains unresolved.

Actual fight residency OFF/ON comparison passes187 checks: all162 image/raw artifacts and per-frame38,850,240 physical bytes plus10MiB EDRAM and all query results match exactly. All80 target draws become specialized without gameplay compilation or PSO creation. Added a separately gated exact action packed-pass certificate after independent review and1026 CPU checks. New main9362d409/replay17f42103 share585 source inputs; runtime/DXC unchanged. Both lookup and new action grouping remain defaultOFF; normal config is unchanged.

Current grouping OFF/ON replay passes382 checks, including exact artifact/full-memory/query parity and unchanged non-target work.79 of80 eligible target draws group into six passes, avoiding73 restarts/EDRAM barriers and146 sampled-image barriers. First target draw uploads its textures and remains standalone. Original80/74 prediction failure is preserved; exact counter diagnosis explains one preparation refusal and162 additional successful owner lookups. No source policy was relaxed. Physical-list GPU timing and a clean fullscreen test are prepared; no live FPS improvement claimed. Evidence: native-action-capture-20261015/action-packed-pass-candidate.

Physical-list timing preserves all162 artifacts, both actual state oracles and295 queries, with supported complete timestamp records and zero invalid/unavailable/dropped results. Target effect span falls about0.16ms/frame in the two captured frames; other unchanged cohorts fluctuate, so this is not a whole-frame performance result. Replay forces query-end retirement and therefore exaggerates live query pass overhead: actual live D32 query grouping already works, and all188 prior pressure samples show zero capacity waits. Avoid optimizing that replay artifact.

Clean action fullscreen test exits0 after322.423s, native-exclusive4KHDR and uncapped. All health and normal-state preservation checks pass;255 prepared PSOs,209 modules, no gameplay specialization compilation/PSO creation. Action groups39,364 draws and avoids34,793 pass restarts,44 preparation standalones, zero module/PSO/query refusals. User reports around70FPS overall, typical dips high50s/low60s, one/two brief46FPS counter dips and a41ms heavy-action/explosion peak versus normal~14ms; panning still has short dips. This is encouraging user feedback, not a controlled A/B FPS percentage or proof of stable60. Normal configuration/cache still unchanged. New diagnostic preparation targets individual spikes: bounded in-memory frame journal reusing existing clock/counters, no per-draw profiler or GPU readbacks. Periodic120-frame report timestamps span roughly2–3ms and must be marked in the next interval rather than misattributed to GPU work. Preserve current590 files under native-action-capture-20261015/before-frame-journal.

Optional frame journal adopted after48 CPU checks and independent review. Only Runtime wiring plus new header/test changed; default path empty. Uses existing completion timestamp, seven per-frame operation counts, existing inclusive completion-call timing and cumulative query-pressure counters. Buffers at most65,536 POD rows, marks current/previous diagnostic reports, and writes once with exclusive creation after final Runtime ownership release. New diagnostic allocation/write/logging failures remain isolated. Main e7a34c1f and replay17f42103 build with587 unchanged source inputs; runtime/DXC unchanged and replay is byte-identical to the already-qualified grouping build, so no redundant GPU replay needed. Prepared frame-journal-run-01 authority validates3476 files/2816 private seeds; no launch yet. User availability question is pending for another fight reproduction. The journal measures Native completion boundaries, not display scanout or exclusive GPU time.

2026-10-15: frame-journal-run-01 closes normally after123.811s, all health/preservation gates pass. Complete journal9731 rows/83.732s completion span, no dropped rows or failures. User confirms same hitch. All runtime/SDK pressure counters zero; no inline frame disposal (9731 accepted/completed). No gameplay pipeline creation; shader module/input misses below0.028ms. Draw>500 subset2799 intervals median14.377ms; seven>33ms all in first4.10s from first high-draw frame, including a364ms initial transition. Event-to-display alignment is unavailable, so this does not negate user hitch. Later short spikes reach26.69ms and many intervals exceed16.67ms. Independent matched-neighbor analysis finds detailed report-preceded intervals about1.7ms longer; this explains a smaller periodic cost, not all spikes. Preparing quiet periodic reports and a journal extension using already-timed internal queue completion snapshots; no new GPU work/per-draw clocks intended. Current normal config unchanged.

2026-10-15: Adopted optional quiet frame reports plus frame-journal v2 (five source files only, preserved592 inputs beforehand). Quiet mode retains120-interval summaries/readiness/errors and emits full Scene counters once after existing shutdown synchronization; defaulttrue remains compatible until normal configuration promotion. V2 copies existing matched completed-frame draw/texture snapshots and lifetime queue counts, no new clocks/GPU calls/JSON or locks. Release66 and ASan/UBSan66 CPU checks pass. Main18234377/replayb3a9a123 builds pass same587 source cohort; runtimeaa4e/DXCda5e unchanged. Root merge proves runtime differs by quiet logging plus journal extraction only. New replay includes unused observer method; rendering execution sources otherwise unchanged, inherited numerical parity is not relabeled as a new GPU test. Independent review and private quiet-journal-v2-run-01 launch preparation ongoing.

Quiet-journal-v2-run-01 frozen3491 bindings/2816 seeds after67 parser checks and10 independent source checks. Launched currentmain18234377 PID2993678/session81788; foreground fullscreen verified, HDR3840x2160 and unlimited/VSyncfalse logged. Private quietreportsOFF/journalv2ON, all rendering settings/seeds identical previousmanual test. No video/profiler/GPUtimestamps/perdrawCPUtimers. Test in progress; next user route confirmation pending shader-ready notice. Normalconfig remains unchanged; no hitchfix claim.

Quiet-v2 fullscreen run closes normally178.837s; all strictNative/query and preservation checks pass, 15,882/15,882 complete frame+queue snapshots. Quiet mode yields zero detail records,132 interval summaries and one cold Scene health dump. User reports no obvious fight/explosion hitch but mid50s FPS during action. Post-first5s highdraw median15.403ms,p9518.305,max30.968; this is not a matched A/B against previous route. Existing internal queue completion median3.09ms; texture bursts correlate with some slow frames but do not explain all. Unchanged raw-float path is NOT retried: Oct11 after-volume comparison actually selected one64KiBcopy/frame, but its 4% longer time was host-load-confounded and not an attributable regression; current capture is only a negative/fallback oracle. Promoted only gow3_native_verbose_frame_reports=false in normal config after clean run; journal remains OFF normally, action experimentalflags stillOFF. Promotion receipt quiet-reports-normal-promotion/applied.json; stable60combat unresolved.

2026-10-15: Current-build CPU profile plus quiet journal v2 completed normally114.152s with15,360 user-cycle samples, no reported sample loss, and exact CLOCK_MONOTONIC overlap (11,065 samples across5,813 journal intervals). Initial run01 rejected perf's actual five-byte ACK before handoff; failure preserved, normal state unchanged. Fresh run02 accepts only ack LF or ack LF NUL, as proven from installed perf binary. User reports mid40s action dips under profiling. Collector had appreciable CPU overhead: installed perf7.1.3 retains exited descriptors in poll; upstream fixes this poll storm, and recorded startup thread exit/17.06M wakeups are consistent. This is diagnostic stack evidence, not clean FPS evidence. Per-thread offline decoding recovers saved callchains that global perf output omitted; all15,360 sample identities/counts retained. Later highdraw samples: graphics producer50.47% of sampled cycles, command thread32.02%, main game logic8.54%; texture decode grows in slower frames. No exclusive wall-time attribution claimed.

Current profile justifies a bounded cached-resource-hash trial: LiveResourceMap inclusive work6.353% of producer sampled cycles; instruction-level audit isolates57 resident-key rehash samples (1.370%producer /0.691%all cycles), excludes required query-key hashes and raw shader/payload fingerprinting. Adopted only the prequalified one-header noexcept removal after preserving6 current files and all587 source identities. Prior4,283,176 exact-map checks remain applicable. Main02f6baf4 and replay91fc9171 builds pass with587 identical source inputs, runtimeaa4e/DXCda5e unchanged; mainall5consumers and replayall78steps rebuilt consistently after recovered dependency database. Actualfight GPU oracle qualification and profiler-free live comparison pending; no FPS gain or stable60 claim. Evidence: native-action-capture-20261015/{cpu-profile-journal-v2-analysis,cached-resource-hash-20261015}.

2026-10-15: Cached-resource-hash first profiler-free pair completes with exact cameras and106 functional/workload checks passing. Mean13.620098→13.233824ms (2.836% apparent gain), median block-p9514.78→14.34ms; each2040measured frames. Below3% screen and external desktop load materially favors NEW (CPU71.66→41.34%onecore,gfx8.19→6.06%). No attributable gain, reverse or promotion. Restored exact pretrial header3bceaadd/main18234377/replayb3a9a123; current source header touched so next incremental build regenerates all map-layout consumers. Candidate preserved under cached-resource-hash-20261015/unpromoted, rollback receipt retained. Normal config/runtime/DXC unchanged. Narrow1/2byte texture-decoder candidate remains report-local; release44901checks/612savedfetches passed, sanitizer+CPU benchmark qualification in progress. Stable60FPS unresolved.

2026-10-15: Narrow texture decoding report-local CPU screen: original eight-loop candidate passes44901 release and sanitizer checks including612savedfetches, but wider controls regress6.8% despite faster narrow decoding. Compact three-loop alternative also regresses wide controls7.4%; neither adopted. Final isolated-dispatch variant moves narrow loops into one portable noinline once-per-mip/layer function;44901release checks pass. Across8alternating rounds:69saved narrow layouts aggregate1.2146x,16narrowcontrols1.3270x,40widecontrols1.0163x; no>5%regressions and both orders agree. Exact final sanitizer checks pending; no production adoption, full-game FPS or rendering claim. Next producer proposal is default-OFF whole-batch warm certificate readability: skip redundant second heap probes only after fresh all-member ValidateOnly owner/token validation, otherwise original whole filtering/capture path. Report-local review/tests under certificate-first-readability-candidate-20261015.

2026-10-15: Combined isolated decoder + default-OFF whole-batch warm readability adopted in five files after44901 decoder release/sanitizer checks,657 warm release/sanitizer checks,495880 current-aa4e actual SDK memory checks, and independent reviews. All588source identities include gears_bridge.cpp. Main d43675c3/replay d8b601a1 built consistently, including hash-layout rollback consumers; runtime aa4e/DXC da5e and normalconfig unchanged. Actual saved fight replay exits0 after54.672s; all162artifacts/fullmemory/295queries and semantic report exact. D=decoder-warm-readability-build-20261015. W=certificate-first-readability-candidate-20261015; its same-binary uncapped warmOFF/ON first pair is running (session87137),10462 frozen bindings. User available for clean fullscreen test if comparison is promising; clean-current-01 staged but unfrozen. No live FPS gain/promotion/stable60claim yet.

2026-10-15: Warm certificate first pair completes with both arms exit0 and frozen/normal inputs unchanged. Original frozen analyzer rejects duplicated terminal counters; preserved as failed, separate supplement accepts only exact-equal duplicate reports (both identical), all162 earlier rendering artifacts remain qualified. Supplemental mean13.53333→13.09120ms (-3.267%), medianblockp9514.96→14.01ms, but performance qualification fails: externalCPU62.08→43.14%onecore/gfx7.96→4.44% favorON, CPUuploads/frame differ-8.119% andpayloadbytes/frame-7.900%.107/110 supplemental gatespass; no reverse/default promotion. ON accepts1,380,451warmcohorts, skips11,009,450 repeated second readability probes (81%ofbatchviews), errors0/refusals1;1.93TB is logical repeated coverage, not savedtraffic. Decision at certificate-first-readability-candidate-20261015/first-pair-review.

Root is preparing explicit experimental clean-warm-trial-01 with correctness-qualified currentD decoder+warmON, private saves/cache/512actionseed, uncapped4KHDRMangoHudonly, no journal/perdrawtimers/profiler/video. Previous conditional clean-current-01 remains unfired unchanged; new trial does not assert A/B qualification. User available. Separate report-local CanonicalDigest aggregate hex-validation CPU screen passes266370 differentialcases and8alternatingrounds26.17→8.10ns/constructor; no productionadoption or gameFPSclaim.

Clean-warm-trial-01 passed49focused parser/control checks and exact currentD authority, frozen10500bindings/2816seeds. Launched PID3100809/session87331, ownedactivefullscreenconfirmed; logverifies3840x2160HDR/unlimited/VSyncfalse/Nativeexclusive/warmON. User notified and routequestionpending; no CPU/GPU timing/video/profile/journal. No performance result yet. Report-local digest validation also passes266370 ASan/UBSancases(leakcheckdisabled) and independent review; not adopted during frozenmanual trial.

2026-10-15: Clean-warm-trial-01 closesnormally168.577s andpassesallhealth/unchangedstate.512534/512534queriesretired, native-only4KHDR, zeroGPU/CPUtimers/journal/video/profile. Warmaccepts1467894cohorts/11114238views,error0/refusals53; actiongroupingavoids18643passes. Userreportsidle60-70mostly60 butmid40actiondropstillpresent. This doesnotestablishA/Bbenefit; warmstilldefaultOFF.

Adopted exact independentlyreviewed CanonicalDigest validation reduction (same64-byte acceptedalphabet/throw/text/layout) plus exhaustiveactualtypechecks inexistinglive_draw_test. ReleaseandASan/UBSan266370screenchecks, actualtypedserialization/owner/slice/concurrentexporttestspass. Main39f49535/replay1b655ef3 rebuiltwithsame588sourcecohort,only2sourcechanges; runtimeaa4e/DXCda5e/configunchanged. CPUconstructormedian26.16→8.10ns, notwholegameFPSclaim. Evidence digest-validation-adoption. LargercombatCPU/GPUworkauditcontinues; stable60unresolved.


2026-10-15, next-area stutter investigation: The later CPU texture sampling-storage sharing plus action-state lookup/action packed-pass grouping are now enabled in normal config. User reported two combat runs without sub-60 drops, then recurring stutter entering the next area. Normal config SHA182f8029 and cache/saves are preserved. Reports under combat-gpu-after-learned-20261011/native-action-capture-20261015/next-area-stutter-20261015 (ST) preserve first/repeat normal logs. Both runs compiled genuinely new shader inputs, with zero shader/PSO evictions; increasing resident limits would not address these measured stalls.

Diagnostic01 used a private cache copy with the 93 newly learned shader entries removed and recipe list restored to first853, leaving normal state unchanged. User reproduced stutter, game exited0. New bounded prewarm status samples show all34 slow Get requests already Running, all subsequently Completed with complete queue history. Workers start within microseconds but discovery precedes demand by only about4-9ms, while some compiles take60-200ms. Shader waits total2.687s (21pixel/13vertex); late discovery is established, worker starvation is not. Reports in ST/diagnostic-01, code/queue checks in ST/prewarm-diagnostics-candidate. Canonical compiler/cache-identity inputs unchanged.

A default-OFF pixel block prewarm candidate is built as main d0f7bac2. At the witnessed successful LZX cache-block decode return it validates the selected exact stripped pixel container, offers it first, then scans bounded owned neighboring pixel sources before the guest frees the decoded block. Vertex requests skip this optional speculation because later linking changes their bytecode. Nine instruction witnesses,1MiBblock/output caps,128candidatecap,64SHAhistory. Existing compiler/draw validation remains authoritative; offered counts do not prove queue admission/completion. Release and ASan/UBSan pass3113checks including all642capturedstrippedpixels. Normalconfig and shaderidentity unchanged. ST/block-prewarm-candidate preserves before/build/tests/review. ST/diagnostic-02-block-prewarm launched with private flagON, samecoldseed, uncappedMangoHudonly; boot confirms hook enabled. User needs external heavy work to continue, so this run can test discovery/readiness behavior but not qualify clean comparative FPS/compile timing. No stutter-fix or default-promotion claim pending results.

Diagnostic02 closed normally after67s; user explicitly requested pause until Unreal Editor finishes. Route not completed; do not use as a performance result. Normal user/config unchanged. Pending next action after user resumes: relaunch fresh private diagnostic from same D1 cold seed, not D2 learned cache. Agent audit interrupted; no work continues while paused.
