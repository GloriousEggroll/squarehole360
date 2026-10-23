#!/usr/bin/env bash
# Capture a stationary view or movement sequence when the user is ready.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$project_dir"
game_pid=${1:?Game PID required}
run_name=${2:?Unique capture name required}
if [[ ! "$game_pid" =~ ^[0-9]+$ || ! "$run_name" =~ ^[a-z0-9-]+$ ]]; then exit 2; fi
exe=$(readlink "/proc/$game_pid/exe")
if [[ "$exe" != "$project_dir/build/bin/gow3" ]]; then
  echo 'PID does not match the current game build.' >&2
  exit 1
fi
report_dir="$project_dir/reports/performance/reproducible-spot/$run_name"
if [[ -e "$report_dir" ]]; then
  echo 'Capture directory already exists; use a new name.' >&2
  exit 1
fi
window_id=$(xdotool search --all --onlyvisible --pid "$game_pid" --class gow3 | head -n 1)
mkdir -p "$report_dir"
python3 - "$report_dir/clock-start.json" <<'PY'
import json, sys, time
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({
    "wall_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns()
}, indent=2) + "\n")
PY
date --iso-8601=ns > "$report_dir/start.txt"
cp config/gow3.toml "$report_dir/config.toml"
sha256sum build/bin/gow3 build/bin/librexruntime.so > "$report_dir/build.sha256"
tr '\0' '\n' < "/proc/$game_pid/cmdline" > "$report_dir/command.txt"
import -window "$window_id" "$report_dir/start.png"
python3 tools/rendering/sample-process.py "$game_pid" "$report_dir/threads.csv" --seconds 45 &
sampler_pid=$!
/tmp/gow3-perf/usr/bin/perf record --clockid mono -e cycles:u -F 99 --call-graph dwarf,4096 \
  -p "$game_pid" -o "$report_dir/cpu.data" -- sleep 45 > "$report_dir/perf.log" 2>&1 &
profiler_pid=$!
sleep 22
import -window "$window_id" "$report_dir/middle.png"
wait "$sampler_pid"
wait "$profiler_pid"
import -window "$window_id" "$report_dir/end.png"
date --iso-8601=ns > "$report_dir/end.txt"
python3 - "$report_dir/clock-end.json" <<'PY'
import json, sys, time
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({
    "wall_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns()
}, indent=2) + "\n")
PY
echo "Capture saved to $report_dir"
