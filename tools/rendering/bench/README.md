# Benchmark harness

All runs launch the real game through `tools/rendering/verify-native-boot.py`
with a scripted input sequence, so they need an imported game and a save.

| Script | Purpose |
|---|---|
| `auto.sh OUT [args]` | One automated run; samples per-thread CPU load during the held view |
| `pan_profile.sh OUT [args]` | `perf` call stacks (DWARF) of the render thread and GPU Commands |
| `annotate_profile.sh OUT [args]` | Flat `perf` profile kept for `perf annotate` |
| `wall_stacks.sh OUT [args]` | Wall-clock stack samples (includes blocked time) via `eu-stack` |
| `frame_busy.sh OUT [args]` | Per-frame thread busy fraction aligned to the frame journal |
| `gpu_counters.py SAMPLES` | Summarize `amdgpu_top -J` GPU block counters |
| `stack_report.py STACKS` | Inclusive/self breakdown of `perf script` stacks |
| `bands.py JOURNAL...` | Frame-time medians by operation-count band |
| `perop.py JOURNAL...` | 1,500–2,500 operation band: median ms and µs per operation |

Input sequences in `sequences/`:
- `load-checkpoint.json` loads the latest checkpoint and holds still. With a
  save at the Act 1 Tickers fight it measures about 1,600 operations per frame.
- `pan-seeded.json` loads the latest save and performs the camera-pan test.

Example (fight, frame journal for `perop.py`):

```sh
RUN_SECONDS=125 SAMPLE_DELAY=100 SEQ=tools/rendering/bench/sequences/load-checkpoint.json \
  tools/rendering/bench/auto.sh /tmp/fight-01 --camera-input --weapon-input \
  --native-cache-source user/cache/native-shaders --pipeline-cache-source user/cache/native-pipeline-recipes \
  --config gow3_native_frame_journal=/tmp/fight-01-journal.csv
python3 tools/rendering/bench/perop.py /tmp/fight-01-journal.csv
```

Run-to-run noise is about ±2% on the per-operation metric. Check for other CPU
or GPU load first, and compare at least two runs per configuration.
