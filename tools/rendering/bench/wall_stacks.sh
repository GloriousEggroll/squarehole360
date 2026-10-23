#!/bin/bash
# Wall-clock stack samples of one named thread (default "GPU Commands"):
# includes blocked time that cycle sampling cannot see.
# Usage: wall_stacks.sh OUTPUT_DIR [auto.sh args ...]
# Env: SEQ, RUN_SECONDS, SAMPLE_DELAY, PROFILE_AT (default 76), SAMPLES (default 120), THREAD
S="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$S/../../.."
out=$1; shift
P=reports/rendering/native-renderer/performance-60fps/pan-20261019
(SEQ=${SEQ:-$P/pan-sequence.json} "$S/auto.sh" "$out" --camera-input "$@" > "$out.summary" 2>&1 &)
for i in $(seq 1 120); do pid=$(pgrep -x gow3) && break; sleep 0.5; done
sleep ${PROFILE_AT:-76}
tid=$(ps -L -o tid=,comm= -p $pid | grep "${THREAD:-GPU Commands}" | head -1 | awk '{print $1}')
: > "$out.wall-stacks"
for i in $(seq 1 ${SAMPLES:-120}); do
  eu-stack -p $pid 2>/dev/null | awk -v t="TID $tid:" '$0 ~ "^TID" {on=($0==t)} on' >> "$out.wall-stacks"
  echo >> "$out.wall-stacks"
  sleep 0.07
done
while pgrep -x gow3 >/dev/null; do sleep 2; done; sleep 2
cat "$out.summary"
