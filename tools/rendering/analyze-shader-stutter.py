#!/usr/bin/env python3
"""Correlate command-processor frame intervals with shader timing logs."""
import argparse
import bisect
import collections
import datetime
import json
from pathlib import Path
import re
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('run', type=Path)
args = parser.parse_args()
frames, events = [], []
previous = None
for line in (args.run / 'game.log').read_text().splitlines():
    try:
        timestamp = datetime.datetime.fromisoformat(line[1:24]).timestamp()
    except ValueError:
        continue
    thread_match = re.search(r'\[t(\d+)\]', line)
    if not thread_match:
        continue
    thread = int(thread_match[1])
    if 'GPU frame ' in line:
        frame = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)(?= |$)', line)}
        frame.update(time=line[1:24], t=timestamp, thread=thread,
                     interval_ms=round((timestamp - previous) * 1000, 3) if previous else 0,
                     gpu_translation_ms=0.0, worker_translations=0, pipelines=0)
        frames.append(frame)
        previous = timestamp
    timing = re.search(r'Vulkan (shader translation|pipeline creation): ms=([\d.]+)', line)
    if timing:
        duration = float(timing[2])
        events.append(dict(kind=timing[1], ms=duration, start=timestamp - duration / 1000,
                           end=timestamp, thread=thread))
if not frames:
    raise SystemExit('No GPU frame summaries recorded yet.')
gpu_thread = collections.Counter(f['thread'] for f in frames).most_common(1)[0][0]
times = [f['t'] for f in frames]
for event in events:
    first = bisect.bisect_right(times, event['start'])
    last = min(len(frames), bisect.bisect_left(times, event['end']) + 1)
    for frame in frames[first:last]:
        overlap = max(0, min(frame['t'], event['end']) -
                      max(frame['t'] - frame['interval_ms'] / 1000, event['start']))
        if not overlap:
            continue
        if event['kind'] == 'pipeline creation':
            frame['pipelines'] += 1
        elif event['thread'] == gpu_thread:
            frame['gpu_translation_ms'] += overlap * 1000
        else:
            frame['worker_translations'] += 1
eligible = [f for f in frames if f.get('draw_packets', 0) >= 500 and f.get('queries', 0) >= 10]

def summary(selected):
    if not selected:
        return None
    intervals = sorted(f['interval_ms'] for f in selected)
    return dict(frames=len(selected), median_ms=statistics.median(intervals),
                p95_ms=intervals[int((len(intervals) - 1) * .95)],
                p99_ms=intervals[int((len(intervals) - 1) * .99)], max_ms=max(intervals),
                above_25ms=sum(v > 25 for v in intervals), above_50ms=sum(v > 50 for v in intervals),
                gpu_translation_overlap_ms=round(sum(f['gpu_translation_ms'] for f in selected), 3))

translation_events = [e for e in events if e['kind'] == 'shader translation']
results = dict(
    caveat='3D workload filter includes pause screens; log intervals are not exact presentation times. '
           'Different play routes are not a controlled FPS comparison. Millisecond timestamps make overlap approximate.',
    gpu_thread=gpu_thread,
    translation_count=len(translation_events),
    gpu_translation_count=sum(e['thread'] == gpu_thread for e in translation_events),
    gpu_translation_total_ms=round(sum(e['ms'] for e in translation_events if e['thread'] == gpu_thread), 3),
    pipeline_count=sum(e['kind'] == 'pipeline creation' for e in events),
    eligible=summary(eligible),
    first_3d_frame=eligible[0] if eligible else None,
    first_60s_after_initial_frame=summary([f for f in eligible[1:] if f['t'] < eligible[0]['t'] + 60]) if eligible else None,
    worst_frames=sorted(eligible, key=lambda f: f['interval_ms'], reverse=True)[:20],
)
(args.run / 'shader-stutter-summary.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps({k: v for k, v in results.items() if k not in ('worst_frames', 'first_3d_frame')}, indent=2))
