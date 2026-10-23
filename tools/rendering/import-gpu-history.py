#!/usr/bin/env python3
"""Import the initial GPU state from a frame-aligned reference trace.

Only memory downloaded before the first command packet is accepted. Later
CPU snapshots and resolves from the traced frame cannot seed its history.
The trace ABI is documented by ReXGlue's graphics/trace_protocol.h.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

MAX_MEMORY = 512 * 1024 * 1024
EDRAM_BYTES = 2048 * 5120


def snappy(data, expected):
    """Decode a bounded raw Snappy block; no optional Python packages needed."""
    at, size, shift = 0, 0, 0
    while True:
        if at >= len(data) or shift > 28:
            raise ValueError('Invalid Snappy length')
        byte = data[at]; at += 1
        size |= (byte & 127) << shift
        if not byte & 128:
            break
        shift += 7
    if size != expected or size > MAX_MEMORY:
        raise ValueError('Snappy output length mismatch')
    output = bytearray()
    while at < len(data):
        tag = data[at]; at += 1
        kind = tag & 3
        if kind == 0:
            length = tag >> 2
            if length >= 60:
                count = length - 59
                if at + count > len(data):
                    raise ValueError('Truncated Snappy literal length')
                length = int.from_bytes(data[at:at + count], 'little'); at += count
            length += 1
            if at + length > len(data) or len(output) + length > size:
                raise ValueError('Invalid Snappy literal')
            output.extend(data[at:at + length]); at += length
        else:
            count = 1 if kind == 1 else 2 if kind == 2 else 4
            if at + count > len(data):
                raise ValueError('Truncated Snappy copy')
            distance = int.from_bytes(data[at:at + count], 'little'); at += count
            if kind == 1:
                distance |= (tag & 224) << 3
                length = 4 + ((tag >> 2) & 7)
            else:
                length = 1 + (tag >> 2)
            if not distance or distance > len(output) or len(output) + length > size:
                raise ValueError('Invalid Snappy copy distance/length')
            # Copies may overlap the destination, repeating the source period.
            start = len(output) - distance
            period = output[start:start + min(distance, length)]
            output.extend((period * ((length + distance - 1) // distance))[:length])
    if len(output) != size:
        raise ValueError('Truncated Snappy output')
    return bytes(output)


def decode(encoding, data, length):
    if length > MAX_MEMORY:
        raise ValueError('Oversized GPU snapshot')
    if encoding == 0:
        if len(data) != length:
            raise ValueError('Raw snapshot length mismatch')
        return bytes(data)
    if encoding == 1:
        return snappy(data, length)
    raise ValueError('Unknown trace compression')


def initial_state(data, *, state_only=False):
    if len(data) < 48 or struct.unpack_from('<I', data)[0] != 1:
        raise ValueError('Unsupported GPU trace header')
    if struct.unpack_from('<I', data, 44)[0] != 0x4d5308ab:
        raise ValueError('GPU trace is not the supported Gears title')
    at, ranges, edram, gamma, stopped = 48, [], None, None, False
    view = memoryview(data)
    while at < len(data):
        if at + 4 > len(data):
            raise ValueError('Truncated GPU trace command')
        kind = struct.unpack_from('<I', data, at)[0]
        if kind in (0, 2, 4):
            if state_only:
                raise ValueError('Diagnostic state-only export contains commands')
            stopped = True
            break
        if kind in (1, 3, 5):
            at += 4
            continue
        sizes = {6: 20, 8: 12, 10: 24, 11: 16}
        if kind not in sizes or at + sizes[kind] > len(data):
            raise ValueError('Unexpected/truncated initial GPU trace command')
        if kind == 6:
            _, address, encoding, encoded, decoded = struct.unpack_from('<5I', data, at)
            if address % 4 or not decoded or decoded % 4 or address + decoded > MAX_MEMORY:
                raise ValueError('Invalid initial GPU memory range')
        elif kind == 8:
            _, encoding, encoded = struct.unpack_from('<3I', data, at)
            decoded = EDRAM_BYTES
        elif kind == 10:
            _, first, count, _, encoding, encoded = struct.unpack_from('<6I', data, at)
            if first + count > 0x5003:
                raise ValueError('Invalid register snapshot')
            decoded = count * 4
        else:
            _, _, encoding, encoded = struct.unpack_from('<4I', data, at)
            decoded = 4 * (256 + 3 * 128)
        at += sizes[kind]
        if encoded > len(data) - at:
            raise ValueError('Truncated GPU trace payload')
        payload = decode(encoding, view[at:at + encoded], decoded); at += encoded
        if kind == 6:
            if any(address < base + len(blob) and base < address + len(payload) for base, blob in ranges):
                raise ValueError('Overlapping initial GPU memory ranges')
            ranges.append((address, payload))
        elif kind == 8:
            if edram is not None:
                raise ValueError('Duplicate initial EDRAM snapshot')
            edram = payload
        elif kind == 11:
            gamma = payload
    if not stopped and not state_only:
        raise ValueError('GPU trace has no command stream after its initial state')
    return ranges, edram, gamma


def validate_pair(metadata, capture, trace_name, reference_frame=None):
    expected_frame = capture['events'][0]['frame']
    if reference_frame is not None:
        if not any(e['frame'] == reference_frame and e['function'] == 'Present' for e in capture['events']):
            raise ValueError('Reference frame is not a completed captured API frame')
        expected_frame = reference_frame + 1
    if (metadata.get('version') != 1 or metadata.get('provenance') != 'reference_gpu_frame_boundary'
            or metadata['trace'] != trace_name or metadata['frame'] != expected_frame):
        raise ValueError('GPU initial state and API capture frame do not match')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('capture', type=Path, help='Inspected Gears API capture directory')
    parser.add_argument('--reference-frame', type=int, help='Import the next frame boundary as the completed reference for this API frame')
    args = parser.parse_args()
    metadata = json.loads(Path(str(args.trace) + '.initial.json').read_text())
    capture = json.loads((args.capture / 'capture.json').read_text())
    validate_pair(metadata, capture, args.trace.name, args.reference_frame)
    if args.trace.stat().st_size > 2 * MAX_MEMORY:
        raise ValueError('GPU trace exceeds the import size limit')
    data = args.trace.read_bytes()
    ranges, edram, gamma = initial_state(data)

    def save(blob):
        digest = hashlib.sha256(blob).hexdigest()
        (args.capture / f'{digest}.gpu.bin').write_bytes(blob)
        return dict(sha256=digest, bytes=len(blob))
    report = dict(version=1, frame=metadata['frame'], provenance=metadata['provenance'],
                  trace_sha256=hashlib.sha256(data).hexdigest(),
                  ranges=[dict(save(blob), address=address) for address, blob in ranges],
                  edram=save(edram) if edram is not None else None, gamma=save(gamma) if gamma else None)
    filename = 'gpu-history.json'
    if args.reference_frame is not None:
        report['reference_frame'] = args.reference_frame
        filename = f'gpu-reference-{args.reference_frame}.json'
    (args.capture / filename).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(frame=report['frame'], ranges=len(ranges), gpu_bytes=sum(len(b) for _, b in ranges), edram=edram is not None)))


if __name__ == '__main__':
    main()
