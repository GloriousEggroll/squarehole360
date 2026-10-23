#!/usr/bin/env bash
# Preserve normal saves/caches and record a fresh launch with pipeline timings.
# Pass "cold" as the second argument to use empty, isolated shader/driver caches.
# Pass "clone" and a prior run name to copy both caches for a warm comparison.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$project_dir"
run_name=${1:?Unique run name required}
cache_mode=${2:-warm}
if [[ ! "$run_name" =~ ^[a-z0-9-]+$ ]]; then exit 2; fi
if [[ "$cache_mode" != warm && "$cache_mode" != cold && "$cache_mode" != clone ]]; then exit 2; fi
if [[ "$cache_mode" == clone ]]; then
  cache_source=${3:?Prior run name required for clone mode}
  if [[ ! "$cache_source" =~ ^[a-z0-9-]+$ ]]; then exit 2; fi
  cache_source_dir="$project_dir/reports/performance/reproducible-spot/$cache_source"
  if [[ ! -d "$cache_source_dir/cache" || ! -d "$cache_source_dir/mesa" ]]; then
    echo 'Source run must contain isolated game and Mesa caches.' >&2
    exit 2
  fi
fi
report_dir="$project_dir/reports/performance/reproducible-spot/$run_name"
if [[ -e "$report_dir" ]]; then
  echo 'Run already exists; choose a new name.' >&2
  exit 1
fi
mapfile -t candidates < <(pgrep -x gow3 || true)
for pid in "${candidates[@]}"; do
  exe=$(readlink "/proc/$pid/exe" || true)
  if [[ "${exe% (deleted)}" == "$project_dir/build/bin/gow3" ]]; then
    echo "Game $pid is still running. Close it before starting this comparison." >&2
    exit 1
  fi
done
mkdir -p "$report_dir"
cp config/gow3.toml "$report_dir/config.toml"
sha256sum build/bin/gow3 build/bin/librexruntime.so > "$report_dir/build.sha256"
python3 - "$report_dir/cache-before.json" <<'PY'
import hashlib, json, sys
from pathlib import Path
entries = []
for p in sorted(Path('user/cache/shaders').rglob('*')):
    if p.is_file() and p.suffix in ('.xsh', '.xpso'):
        entries.append({"path": str(p), "bytes": p.stat().st_size,
                        "sha256": hashlib.sha256(p.read_bytes()).hexdigest()})
Path(sys.argv[1]).write_text(json.dumps(entries, indent=2) + '\n')
PY
sed "s#^output_folder=.*#output_folder=$report_dir#" \
  reports/performance/gameplay-optimization/MangoHud.conf > "$report_dir/MangoHud.conf"
export MANGOHUD_CONFIGFILE="$report_dir/MangoHud.conf"
args=(
  --mnk_mode=true
  "--gow3_config=$report_dir/config.toml"
  --gpu_command_summary=true
  --vulkan_pipeline_creation_timing=true
  --audio_diagnostics=false
  "--log_file=$report_dir/game.log"
  --log_max_file_size_mb=100
)
if [[ "$cache_mode" == cold || "$cache_mode" == clone ]]; then
  # The run directory is new, so both caches start empty. Keep the normal user
  # root for campaign progress; the prior caches remain available for rollback.
  mkdir "$report_dir/cache" "$report_dir/mesa"
  if [[ "$cache_mode" == clone ]]; then
    cp -a "$cache_source_dir/cache/." "$report_dir/cache/"
    cp -a "$cache_source_dir/mesa/." "$report_dir/mesa/"
    printf '%s\n' "$cache_source_dir" > "$report_dir/cache-source.txt"
  fi
  args+=("--cache_path=$report_dir/cache")
  export MESA_SHADER_CACHE_DIR="$report_dir/mesa"
  unset MESA_SHADER_CACHE_DISABLE
  unset MESA_DISK_CACHE_READ_ONLY_FOZ_DBS
  unset MESA_DISK_CACHE_READ_ONLY_FOZ_DBS_DYNAMIC_LIST
fi
printf '%s\n' "$cache_mode" > "$report_dir/cache-mode.txt"
printf '%s\n' "${MESA_SHADER_CACHE_DIR:-default}" > "$report_dir/mesa-cache-path.txt"
printf '%s\n' "${args[@]}" > "$report_dir/arguments.txt"
date --iso-8601=ns > "$report_dir/launch.txt"
mangohud ./tools/run-linux.sh "${args[@]}" > "$report_dir/console.log" 2>&1 &
game_pid=$!
echo "$game_pid" > "$report_dir/game.pid"
echo "Recording frame times and pipeline compilation in $report_dir"
python3 tools/rendering/sample-process.py "$game_pid" "$report_dir/threads.csv" --seconds 900 &
sampler_pid=$!
(
  # Start CPU profiling on sustained 3D work. Frame and compilation logging
  # already run from startup, including the first shader encountered.
  for attempt in {1..1200}; do
    if ! kill -0 "$game_pid" 2>/dev/null; then exit; fi
    matches=$(tail -n 100 "$report_dir/game.log" 2>/dev/null | rg -c 'draw_packets=([5-9][0-9]{2}|[0-9]{4,}).*movie_yuv=false queries=([1-9][0-9]|[0-9]{3,}) ' || true)
    if [[ ${matches:-0} -ge 20 ]]; then break; fi
    sleep 0.5
  done
  if [[ ${matches:-0} -lt 20 ]]; then exit; fi
  date --iso-8601=ns > "$report_dir/profile-start.txt"
  /tmp/gow3-perf/usr/bin/perf record --clockid mono -e cycles:u -F 99 --call-graph dwarf,4096 \
    -p "$game_pid" -o "$report_dir/gameplay.data" -- sleep 60 > "$report_dir/perf.log" 2>&1
) &
profiler_pid=$!
wait "$game_pid"
wait "$sampler_pid" "$profiler_pid" || true
