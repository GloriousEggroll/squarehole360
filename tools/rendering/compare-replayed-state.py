#!/usr/bin/env python3
"""Compare native raw resolve images to explicit diagnostic SDK replay state.

This never imports replayed state as original captured history. Resources must
still contain the selected resolve at the inspected replay endpoint.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

import numpy as np
from PIL import Image


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


history = module('comparison_history', 'import-gpu-history.py')
scene = module('comparison_scene', 'build-scene-plan.py')


def decode(ranges, texture, fetch):
    if texture.get('layers', 1) != 1 or texture['dimension'] != 1:
        raise ValueError('Diagnostic comparison requires a single 2D level')
    block, size, pitch, rows, px, py = scene.storage_geometry(texture)
    if block != 1 or texture['format'] not in (6, 7, 26, 31, 32):
        raise ValueError('Unsupported diagnostic comparison format')
    width, height = texture['width'], texture['height']
    x, y = np.meshgrid(np.arange(width, dtype=np.int64) + px,
                       np.arange(height, dtype=np.int64) + py)
    offsets = scene.tiled_offset(x, y, pitch, size) if texture['tiled'] else (y * pitch + x) * size
    addresses = int(texture['base'], 16) + ((offsets[:, :, None] + np.arange(size, dtype=np.int64)) ^ (0, 1, 3, 2)[texture['endian']])
    raw, defined = np.zeros(addresses.shape, np.uint8), np.zeros(addresses.shape, bool)
    lowest, highest = int(addresses.min()), int(addresses.max())
    for start, data in ranges:
        if start > highest or start + len(data) <= lowest:
            continue
        mask = (addresses >= start) & (addresses < start + len(data))
        if np.any(mask):
            raw[mask] = np.frombuffer(data, np.uint8)[(addresses[mask] - start).astype(np.int64)]
            defined[mask] = True
    if not defined.all():
        raise ValueError('Diagnostic snapshot is missing sampled resource bytes')
    fmt = texture['format']
    if fmt in (31, 32):
        value = raw.copy().view('<f2').reshape(height, width, size // 2).astype(np.float32)
        result = np.zeros((height, width, 4), np.float32)
        result[:, :, 3] = 1
        result[:, :, :size // 2] = value
        return result
    bits = np.zeros((height, width), np.uint64)
    for i in range(size):
        bits |= raw[:, :, i].astype(np.uint64) << (i * 8)
    output = []
    for channel in range(4):
        count = (2 if channel == 3 else 10) if fmt == 7 else 16 if fmt == 26 else 8
        shift = channel * (10 if fmt == 7 else count)
        values = ((bits >> shift) & ((1 << count) - 1)).astype(np.float32)
        if (fetch[0] >> (2 + channel * 2)) & 3 == 1:
            half = 1 << (count - 1)
            values = np.maximum(-1, np.where(values >= half, values - 2 * half, values) / (half - 1))
        else:
            values /= (1 << count) - 1
        output.append(values)
    return np.stack(output, axis=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('native', type=Path, help='Native --debug-raw-images output')
    parser.add_argument('reference', type=Path, help='replay-reference.py --export-state output')
    parser.add_argument('output', type=Path)
    parser.add_argument('events', type=int, nargs='+')
    args = parser.parse_args()
    metadata = json.loads((args.reference / 'gpu-state/state.json').read_text())
    replay = json.loads((args.reference / 'result.json').read_text())
    if metadata.get('provenance') != 'diagnostic_replayed_gpu_state' or metadata.get('has_command_stream') is not False:
        raise ValueError('Reference is not an explicit diagnostic replay snapshot')
    data = (args.reference / 'gpu-state/state.xtr').read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if replay.get('diagnostic_state_sha256') != digest or replay.get('diagnostic_state_viewer_command') != -1:
        raise ValueError('Snapshot hash or complete-frame endpoint is unverified')
    ranges, _, _ = history.initial_state(data, state_only=True)
    plan = json.loads((args.capture / 'scene.json').read_text())
    report = dict(provenance='diagnostic_replayed_gpu_state_comparison', reference_sha256=digest,
                  source_trace_sha256=replay['trace_sha256'], comparisons=[])
    for event in args.events:
        operation = next(o for o in plan['operations'] if o['event'] == event and o['type'] == 'resolve')
        if operation['frame'] != metadata['last_present_api_frame']:
            raise ValueError('Resolve does not belong to the exported reference frame')
        texture = operation['destination']
        # Reject later physical overwrites, regardless of resource identity.
        target_ranges = scene.storage_ranges(texture)
        for later in plan['operations']:
            if later['frame'] == operation['frame'] and later['event'] > event and later['type'] == 'resolve':
                if scene.ranges_overlap(target_ranges, scene.storage_ranges(later['destination'], later['destination_rectangle'])):
                    raise ValueError(f'Resolve {event} was overwritten before the reference endpoint')
        expected = decode(ranges, texture, operation['destination_fetch'])
        raw_path = args.native / f'resolve-{event}.rgba32f'
        native_bytes = raw_path.read_bytes()
        if len(native_bytes) != expected.size * 4:
            raise ValueError('Native raw image extent differs')
        actual = np.frombuffer(native_bytes, '<f4').reshape(expected.shape)
        if not np.isfinite(actual).all() or not np.isfinite(expected).all():
            raise ValueError('Comparison image contains non-finite values')
        delta = np.abs(actual - expected)
        report['comparisons'].append(dict(event=event, frame=operation['frame'], format=texture['format'],
            mean_absolute_rgba=delta.mean((0, 1)).tolist(), max_rgba=delta.max((0, 1)).tolist(),
            rms_rgb=float(np.sqrt(np.mean(delta[:, :, :3] ** 2))),
            changed_pixels=int(np.any(delta > 0, axis=2).sum()), pixels=texture['width'] * texture['height']))
        image = np.rint(np.clip(expected[:, :, :3], 0, 1) * 255).astype(np.uint8)
        Image.fromarray(image).save(args.native / f'reference-resolve-{event}.png')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
