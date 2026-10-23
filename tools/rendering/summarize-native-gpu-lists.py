#!/usr/bin/env python3
"""Rank sampled physical-list intervals without attributing mixed work to a shader."""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import statistics

COHORTS = {1: 'standalone', 2: 'packed depth', 3: 'readonly depth queries',
           4: 'packed color', 5: 'general draw group'}

def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)] if ordered else None

def summarize(report, first, end):
    pairs = {p['id']: {'vertex': p['vertex'], 'pixel': p['pixel']}
             for p in report.get('shader_pairs', [])}
    by_signature = collections.defaultdict(list)
    by_cohort = collections.defaultdict(list)
    frames = collections.defaultdict(list)
    coverage = collections.Counter()
    for sample in report.get('samples', []):
        selected = sample['selected_api_frame']
        if not first <= selected < end:
            continue
        coverage['retained_lists'] += 1
        children = sample['children']
        if not children:
            raise ValueError('Retained physical list has no logical children')
        coverage['logical_children'] += len(children)
        coverage['draw_children'] += sum(c['draw'] for c in children)
        # Include every physical list in a selected submission, but do not call
        # neighbouring/unknown frame work a part of the selected API frame.
        exact_frame = sample['all_frames_known'] and all(c['api_frame'] == selected for c in children)
        if not exact_frame:
            coverage['mixed_or_neighbour_frame_lists'] += 1
        value = sample.get('inclusive_gpu_ms')
        if value is None:
            coverage['unavailable_lists'] += 1
            continue
        if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError('Invalid GPU interval')
        if sample['phase'] != 2:
            raise ValueError('GPU interval is not from completed original-fence ownership')
        coverage['available_lists'] += 1
        if not exact_frame:
            continue
        frames[selected].append(value)
        kinds = tuple(sorted({c['kind'] for c in children}))
        pair_ids = tuple(sorted({c['pair'] for c in children if c['draw']}))
        variants = tuple(sorted({(c['vertex_variant'], c['pixel_variant'], c['fsi_state_specialized'])
                                 for c in children if c['draw']}))
        cohorts = tuple(sorted({c['cohort'] if c['cohort_known'] else 0 for c in children}))
        states = tuple(sorted({(c['depth_control'], c['color_mask'], c['blend_mask'])
                               for c in children if c['draw']}))
        signature = (kinds, pair_ids, variants, cohorts, states)
        by_signature[signature].append(value)
        cohort = ', '.join(COHORTS.get(c, 'unknown or transfer') for c in cohorts)
        by_cohort[cohort].append(value)
    total_frames = len(frames)

    def metrics(values):
        return {'physical_lists': len(values), 'inclusive_sum_ms': sum(values),
                'inclusive_sum_per_observed_frame_ms': sum(values) / total_frames if total_frames else None,
                'median_list_ms': statistics.median(values), 'p95_list_ms': percentile(values, .95),
                'maximum_list_ms': max(values)}

    ranked = []
    for (kinds, pair_ids, variants, cohorts, states), values in by_signature.items():
        ranked.append({'kinds': kinds, 'shader_pairs': [dict(id=p, **pairs.get(p, {})) for p in pair_ids],
                       'single_known_shader_pair': len(pair_ids) == 1 and pair_ids[0] in pairs,
                       'selected_variants': variants, 'cohorts': cohorts,
                       'depth_color_blend_states': states, **metrics(values)})
    ranked.sort(key=lambda r: r['inclusive_sum_ms'], reverse=True)
    return {'selected_frame_range': [first, end], 'coverage': dict(coverage),
            'observed_exact_frames': total_frames,
            'actual_frames': sorted(frames), 'ranked_physical_groups': ranked,
            'cohort_totals': sorted([{'cohort': c, **metrics(v)} for c, v in by_cohort.items()],
                                    key=lambda r: r['inclusive_sum_ms'], reverse=True),
            'limitations': [
                'GPU intervals include dependencies and may overlap. Sums are not exclusive GPU time or a frame budget.',
                'Only sampled unified-queue physical lists are measured; synchronous transfers and presentation are outside this scope.',
                'Neighbouring and mixed-frame lists remain counted in coverage but are excluded from exact-frame rankings.',
                'Multiple shader pairs within one physical list are retained together; its interval is never split between shaders.',
                'Missing timestamps and truncated retention are explicit. Ranking alone does not identify hardware stall reasons.'
            ]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--first-frame', type=int, default=0)
    parser.add_argument('--end-frame', type=int, default=2**32 - 1)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 0 <= args.first_frame < args.end_frame <= 2**32 - 1:
        parser.error('Expected 0 <= first-frame < end-frame <= UINT32_MAX')
    if args.output.exists():
        parser.error('Output already exists; preserve previous evidence')
    if args.report.stat().st_size > 512 * 1024 * 1024:
        parser.error('Input exceeds the bounded diagnostic report size')
    raw = args.report.read_bytes()
    report = json.loads(raw)
    if not report.get('supported') or not report.get('enabled'):
        parser.error('GPU physical-list timing was not active/supported')
    result = summarize(report, args.first_frame, args.end_frame)
    result['input'] = {'path': str(args.report), 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}
    result['retention'] = {k: report.get(k) for k in ('status', 'retention_complete', 'capacity_stopped',
        'records', 'available', 'unavailable', 'invalid', 'dropped_groups', 'unknown_frame_batches')}
    result['schema'] = 'native-gpu-physical-list-ranking-v1'
    with args.output.open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({'output': str(args.output), 'frames': result['observed_exact_frames'],
                      'coverage': result['coverage'], 'retention': result['retention']}))

if __name__ == '__main__':
    main()
