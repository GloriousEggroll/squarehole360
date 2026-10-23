#!/usr/bin/env python3
"""Recover missing immutable CPU texture bytes in a paired diagnostic GPU trace.

Existing trace bytes and GPU-produced ranges are never replaced. Conflicting
CPU snapshots are rejected. Output provenance deliberately prevents this trace
from being imported as an original GPU-history snapshot.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


history = module('repair_history', 'import-gpu-history.py')
scene_plan = module('repair_scene', 'build-scene-plan.py')
LIMIT = 512 * 1024 * 1024


def merge_ranges(ranges):
    merged = []
    for first, last in sorted(ranges):
        if not 0 <= first < last <= LIMIT:
            raise ValueError('Invalid physical range')
        if merged and first <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(last, merged[-1][1]))
        else:
            merged.append((first, last))
    return merged


def subtract(first, last, occupied):
    for start, end in occupied:
        if end <= first:
            continue
        if start >= last:
            break
        if first < start:
            yield first, min(start, last)
        first = max(first, end)
        if first >= last:
            return
    if first < last:
        yield first, last


def trace_layout(data):
    if len(data) < 48 or struct.unpack_from('<I', data)[0] != 1:
        raise ValueError('Unsupported trace header')
    at, insertion, reads, writes, swaps = 48, None, [], [], []
    while at < len(data):
        if at + 4 > len(data):
            raise ValueError('Truncated trace command')
        kind = struct.unpack_from('<I', data, at)[0]
        fixed = {0: 12, 1: 4, 2: 12, 3: 4, 4: 12, 5: 4, 6: 20,
                 7: 20, 8: 12, 9: 8, 10: 24, 11: 16}.get(kind)
        if fixed is None or at + fixed > len(data):
            raise ValueError('Invalid trace command')
        extra = 0
        if kind in (0, 2, 4):
            if insertion is None:
                insertion = at
            count = struct.unpack_from('<I', data, at + 8)[0]
            extra = count * 4
            if kind == 4 and count >= 6 and at + fixed + extra <= len(data):
                packet = struct.unpack_from('>6I', data, at + fixed)
                if packet[0] >> 30 == 3 and (packet[0] >> 8) & 127 == 0x64:
                    swaps.append(packet[5])
        elif kind in (6, 7):
            _, address, encoding, extra, length = struct.unpack_from('<5I', data, at)
            if encoding > 1 or not length or address + length > LIMIT:
                raise ValueError('Invalid traced memory range')
            (reads if kind == 6 else writes).append((address, address + length))
        elif kind in (8, 10, 11):
            extra = struct.unpack_from('<I', data, at + fixed - 4)[0]
        at += fixed + extra
        if at > len(data):
            raise ValueError('Truncated trace payload')
    if insertion is None:
        raise ValueError('Trace contains no commands')
    return insertion, merge_ranges(reads), merge_ranges(writes), swaps


def combine_snapshots(candidates):
    """Merge consistent byte observations; reject any ambiguous missing byte."""
    result = []
    for address, blob, source in sorted(candidates, key=lambda item: item[0]):
        if result and address <= result[-1][0] + len(result[-1][1]):
            start, prior, sources = result[-1]
            offset = address - start
            overlap = min(len(blob), len(prior) - offset)
            if prior[offset:offset + overlap] != blob[:overlap]:
                raise ValueError(f'Conflicting CPU snapshots at physical {address:08x}')
            prior.extend(blob[overlap:])
            sources.add(source)
        else:
            result.append((address, bytearray(blob), {source}))
    return result


def repair(trace, capture, output):
    if output.exists() or Path(str(output) + '.initial.json').exists() or Path(str(output) + '.repair.json').exists():
        raise ValueError('Diagnostic repair output must be new')
    data = trace.read_bytes()
    metadata = json.loads(Path(str(trace) + '.initial.json').read_text())
    capture_bytes = (capture / 'capture.json').read_bytes()
    captured = json.loads(capture_bytes)
    history.validate_pair(metadata, captured, trace.name)
    insertion, present, written, swaps = trace_layout(data)
    if swaps != [metadata['frame']]:
        raise ValueError('Repair requires exactly the paired API frame in the GPU trace')
    initial, _, _ = history.initial_state(data)
    produced = written + [(base, base + len(blob)) for base, blob in initial]
    plan = scene_plan.build(capture)
    if plan['issues']:
        raise ValueError('Capture has unsupported operations; GPU ownership cannot be established')
    for operation in plan['operations']:
        if operation['frame'] == metadata['frame'] and operation['type'] == 'resolve':
            produced.extend(scene_plan.storage_ranges(operation['destination'], operation['destination_rectangle']))
    produced = merge_ranges(produced)
    candidates, sources, seen = [], {}, set()
    for operation in plan['operations']:
        if operation['frame'] != metadata['frame'] or operation['type'] != 'draw':
            continue
        for texture in operation['textures']:
            if texture['source'] != 'cpu_snapshot':
                continue
            for field, word in (('base_payload', 1), ('mip_payload', 5)):
                digest = texture[field]
                if not digest:
                    continue
                address = texture['fetch'][word] & 0xfffff000
                if (address, digest) in seen:
                    continue
                seen.add((address, digest))
                blob = (capture / (digest + '.payload.bin')).read_bytes()
                if hashlib.sha256(blob).hexdigest() != digest or not address or address + len(blob) > LIMIT:
                    raise ValueError('Invalid CPU asset payload')
                absent = list(subtract(address, address + len(blob), present))
                for first, last in absent:
                    if scene_plan.ranges_overlap([(first, last)], produced):
                        raise ValueError(f'CPU repair overlaps GPU-produced memory at {first:08x}')
                    source = f'{address:08x}:{digest}'
                    sources[source] = dict(address=address, sha256=digest, bytes=len(blob),
                                           event=operation['event'], slot=texture['slot'], region=field)
                    candidates.append((first, blob[first - address:last - address], source))
    merged = combine_snapshots(candidates)
    if not merged:
        raise ValueError('No missing immutable CPU texture bytes to repair')
    records, injected = [], []
    for address, blob, provenance in merged:
        # Raw trace memory records preserve byte order and need no codec dependency.
        records.append(struct.pack('<5I', 6, address, 0, len(blob), len(blob)) + blob)
        injected.append(dict(address=address, bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest(),
                             sources=sorted(provenance)))
    repaired = data[:insertion] + b''.join(records) + data[insertion:]
    report = dict(version=1, provenance='diagnostic_cpu_asset_repaired_trace', frame=metadata['frame'],
                  original_trace_sha256=hashlib.sha256(data).hexdigest(),
                  capture_json_sha256=hashlib.sha256(capture_bytes).hexdigest(),
                  output_trace_sha256=hashlib.sha256(repaired).hexdigest(),
                  original_bytes_preserved=True, injected_bytes=sum(item['bytes'] for item in injected),
                  injected_ranges=injected, source_payloads=sources,
                  warning='CPU asset repair from paired capture; not an original GPU snapshot')
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('xb') as file:
        file.write(repaired)
    Path(str(output) + '.repair.json').write_text(json.dumps(report, indent=2) + '\n')
    Path(str(output) + '.initial.json').write_text(json.dumps(dict(
        version=1, frame=metadata['frame'], trace=output.name,
        provenance=report['provenance'], repair_report=output.name + '.repair.json'), indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    report = repair(args.trace, args.capture, args.output)
    print(json.dumps({key: report[key] for key in ('frame', 'injected_bytes', 'output_trace_sha256')}, indent=2))


if __name__ == '__main__':
    main()
