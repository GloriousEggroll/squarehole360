#!/usr/bin/env bash
# Use normal saves, caches and graphics settings with bounded native GPU timing.
# Per-draw GPU timestamps sample every 16th API frame; normal launch stays untraced.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$project_dir"
run_name=${1:?Unique run name required}
if [[ ! "$run_name" =~ ^[a-z0-9-]+$ ]]; then exit 2; fi
report_dir="$project_dir/reports/rendering/native-renderer/room-video-analysis/$run_name"
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
sed "s#^output_folder=.*#output_folder=$report_dir#" \
  reports/performance/gameplay-optimization/MangoHud.conf > "$report_dir/MangoHud.conf"
export MANGOHUD_CONFIGFILE="$report_dir/MangoHud.conf"
args=(
  "--gow3_config=$report_dir/config.toml"
  --gow3_renderer=native
  --gpu_command_summary=true
  --gow3_native_time_draw_recording=true
  "--gow3_native_gpu_batch_report=$report_dir/gpu-batches.json"
  "--gow3_native_gpu_draw_report=$report_dir/gpu-draws.json"
  "--gow3_native_failure_directory=$report_dir/failure"
  "--log_file=$report_dir/game.log"
  --log_max_file_size_mb=100
)
printf '%s\n' "${args[@]}" > "$report_dir/arguments.txt"
date --iso-8601=ns > "$report_dir/launch.txt"
echo "Recording frame times, native GPU batches and sampled draws in $report_dir"
mangohud ./tools/run-linux.sh "${args[@]}" > "$report_dir/console.log" 2>&1 &
game_pid=$!
echo "$game_pid" > "$report_dir/game.pid"
python3 tools/rendering/monitor-native-room.py "$report_dir" \
  > "$report_dir/monitor.log" 2>&1 &
monitor_pid=$!
cleanup_monitor() {
  if kill -0 "$monitor_pid" 2>/dev/null; then
    kill -TERM "$monitor_pid" 2>/dev/null || true
  fi
  wait "$monitor_pid" || true
}
trap cleanup_monitor EXIT
wait "$game_pid"
