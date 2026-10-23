#!/bin/bash
# Per-frame thread busy fraction: 1 kHz CPU samples on CLOCK_MONOTONIC for the
# producer XThread, GPU Commands and Main XThread, aligned to the frame journal.
# Usage: frame_busy.sh OUTPUT_DIR [auto.sh args ...]   (env as auto.sh, plus PROFILE_AT)
S="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$S/../../.."
out=$1; shift
("$S/auto.sh" "$out" --camera-input --config gow3_native_frame_journal="$PWD/$out-journal.csv" "$@" > "$out.summary" 2>&1 &)
for i in $(seq 1 120); do pid=$(pgrep -x gow3) && break; sleep 0.5; done
sleep ${PROFILE_AT:-76}
tids=$(ps -L -o tid=,pcpu=,comm= -p $pid | awk '/XThread|GPU Commands|Main XThread|Native Planner/' | sort -k2 -rn | head -5 | awk '{print $1}' | paste -sd,)
ps -L -o tid=,comm= -p $pid > "$out.threads"
perf record -k CLOCK_MONOTONIC -e cpu-clock:u -F 1000 -o "$out.perf" -t "$tids" -- sleep 10 >/dev/null 2>&1
while pgrep -x gow3 >/dev/null; do sleep 2; done; sleep 2
perf script -i "$out.perf" -F tid,time 2>/dev/null > "$out.samples"
rm -f "$out.perf"
python3 -I "$S/frame_busy.py" "$out.samples" "$out-journal.csv" "$out.threads"
