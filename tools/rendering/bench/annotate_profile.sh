#!/bin/bash
# Flat (no call graph) cycle profile of the producer XThread and GPU Commands
# for 8 s, keeping perf.data for `perf annotate`.
# Usage: annotate_profile.sh OUTPUT_DIR [auto.sh args ...]
# Env: SEQ, RUN_SECONDS, SAMPLE_DELAY, PROFILE_AT (seconds after launch, default 76)
S="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$S/../../.."
out=$1; shift
P=reports/rendering/native-renderer/performance-60fps/pan-20261019
(SEQ=${SEQ:-$P/pan-sequence.json} "$S/auto.sh" "$out" --camera-input "$@" > "$out.summary" 2>&1 &)
for i in $(seq 1 120); do pid=$(pgrep -x gow3) && break; sleep 0.5; done
sleep ${PROFILE_AT:-76}
prod=$(ps -L -o tid=,pcpu=,comm= -p $pid | grep XThread | sort -k2 -rn | head -1 | awk '{print $1}')
gpu=$(ps -L -o tid=,comm= -p $pid | grep "GPU Commands" | head -1 | awk '{print $1}')
echo "producer=$prod gpu=$gpu" > "$out.tids"
perf record -e cycles:u -F 2999 -o "$out.perf" -t "$prod,$gpu" -- sleep 8 >/dev/null 2>&1
while pgrep -x gow3 >/dev/null; do sleep 2; done; sleep 2
cat "$out.summary"
