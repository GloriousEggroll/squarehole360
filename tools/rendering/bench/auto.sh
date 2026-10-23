#!/bin/bash
# Run the automated room scene with extra config; sample thread load during the held view.
# Usage: auto.sh OUTPUT_DIR [--config K=V ...]
S="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$S/../../.."
out=$1; shift
SEQ=${SEQ:-reports/rendering/native-renderer/performance-60fps/room-fix-20261006-02/fsi-original-composition-20261010/live-stage/live-on-03/input-sequence.json}
timeout 400 python3 tools/rendering/verify-native-boot.py --hardware --raster --copy-user-data --async-native-submissions --draw-batch-size 16 "$@" --input-sequence $SEQ --seconds ${RUN_SECONDS:-110} --output "$out" > "${TMPDIR:-/tmp}/gow3-auto-last.out" 2>&1 &
harness=$!
for i in $(seq 1 120); do pid=$(pgrep -x gow3) && break; sleep 0.5; done
sleep ${SAMPLE_DELAY:-70}
[ -n "$pid" ] && timeout 25 python3 -I "$S/load_sampler.py" "$pid" "$out/auto-load.jsonl"
wait $harness
grep -E '"(input_actions_complete|clean_ui_close|native_failure)"' "$out/result.json" | tr -d '\n '; echo
"$S/held.sh" "$out/game.log"
python3 - "$out/auto-load.jsonl" <<'PY'
import json,sys,statistics
S=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
if S:
    print(' gpu_busy',round(statistics.mean(s['gpu_busy'] for s in S)),'process_cpu',round(statistics.mean(s['process_cpu'] for s in S)))
    top={}
    for s in S:
        for p,t,c in s['threads'][:4]: top.setdefault(c,[]).append(p)
    for c,v in sorted(top.items(),key=lambda x:-sum(x[1]))[:4]: print('  ',c,round(sum(v)/len(S),1))
PY
