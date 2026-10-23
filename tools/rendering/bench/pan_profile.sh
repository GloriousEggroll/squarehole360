#!/bin/bash
# Pan-test profile: dwarf call stacks of the busiest guest XThread (the D3D
# producer) and GPU Commands for 8 s mid-pan.
# Usage: pan_profile.sh OUTPUT_DIR [auto.sh args ...]
# Env: SEQ (default pan-sequence.json), RUN_SECONDS, SAMPLE_DELAY, PROFILE_AT (seconds after launch, default 76)
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
perf record -e cycles:u -F 1499 --call-graph=dwarf,32768 --user-callchains -o "$out.perf" -t "$prod,$gpu" -- sleep 8 >/dev/null 2>&1
while pgrep -x gow3 >/dev/null; do sleep 2; done; sleep 2
perf script -i "$out.perf" --tid $prod -F ip,sym --no-inline 2>/dev/null > "$out.producer-stacks"
perf script -i "$out.perf" --tid $gpu -F ip,sym --no-inline 2>/dev/null > "$out.gpu-stacks"
rm -f "$out.perf"
cat "$out.summary"
