#!/usr/bin/env python3
"""Sample GPU busy % and per-thread CPU % of a process every 250 ms until it exits.

Writes one JSON object per line: wall time, GPU busy, and the busiest threads.
Reads only /proc and sysfs, so it adds no work to the game itself.
"""
import json, os, sys, time
from datetime import datetime
from pathlib import Path

pid, out_path = int(sys.argv[1]), Path(sys.argv[2])
gpu = next(Path('/sys/class/drm').glob('card*/device/gpu_busy_percent'))
ticks = os.sysconf('SC_CLK_TCK')


def threads():
    result = {}
    for task in Path(f'/proc/{pid}/task').iterdir():
        try:
            stat = (task / 'stat').read_text()
        except OSError:
            continue
        # comm may contain spaces; it is wrapped in the last parentheses.
        comm = stat[stat.index('(') + 1:stat.rindex(')')]
        fields = stat[stat.rindex(')') + 2:].split()
        result[task.name] = (comm, int(fields[11]) + int(fields[12]))
    return result


previous, previous_time = threads(), time.monotonic()
with out_path.open('w') as out:
    while Path(f'/proc/{pid}').exists():
        time.sleep(0.25)
        try:
            current, now = threads(), time.monotonic()
            busy = int(gpu.read_text())
        except (OSError, StopIteration):
            break
        elapsed = now - previous_time
        usage = []
        for tid, (comm, total) in current.items():
            if tid in previous:
                percent = 100.0 * (total - previous[tid][1]) / ticks / elapsed
                if percent >= 5:
                    usage.append((round(percent, 1), tid, comm))
        usage.sort(reverse=True)
        out.write(json.dumps({'wall': datetime.now().strftime('%H:%M:%S.%f')[:-3], 'gpu_busy': busy,
                              'process_cpu': round(sum(u[0] for u in usage), 1),
                              'threads': usage[:12]}) + '\n')
        out.flush()
        previous, previous_time = current, now
