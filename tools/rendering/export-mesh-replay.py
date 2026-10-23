#!/usr/bin/env python3
"""Export an isolated captured draw for native Vulkan replay.

Material mode includes both shaders' constants, CPU texture snapshots and
raster state. Prior render passes and GPU-only resolves are not reproduced.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def fetch_requirements(blob):
    flags, virtual, physical, _, _, _, metadata, _, _ = struct.unpack_from('>9I', blob)
    if flags & 1 and flags & 0x41 != 0x41:
        raise ValueError('Vertex shader has not been linked')
    offset, size = struct.unpack_from('>II', blob, metadata)
    if virtual + physical != len(blob) or offset + size > physical or size % 12:
        raise ValueError('Invalid shader extent')
    code = blob[virtual + offset:virtual + offset + size]
    position, limit, slots, textures = 0, len(code), set(), set()
    while position < limit:
        x, y, z = struct.unpack_from('>3I', code, position)
        for cf in (x | (y & 65535) << 32, (y >> 16) | z << 16):
            if cf >> 44 not in (1, 2, 3, 4, 5, 6, 13, 14):
                continue
            address, count = cf & 4095, (cf >> 12) & 7
            if not address or (address + count) * 12 > len(code):
                raise ValueError('Invalid shader EXEC extent')
            limit = min(limit, address * 12)
            for i in range(count):
                if not (cf >> (16 + 2 * i)) & 1:
                    continue
                words = struct.unpack_from('>3I', code, (address + i) * 12)
                opcode = words[0] & 31
                if opcode == 0 and not (words[1] >> 30) & 1:
                    slots.add(((words[0] >> 20) & 31) * 3 + ((words[0] >> 25) & 3))
                elif opcode in (1, 16, 17, 19):
                    textures.add((words[0] >> 20) & 31)
        position += 12
    return slots, textures


def export(directory, event, material=False):
    def payload(digest, suffix='payload'):
        data = (directory / f'{digest}.{suffix}.bin').read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Capture payload hash mismatch')
        return data

    up = event['function'] in ('BeginVertices', 'BeginIndexedVertices') and event['phase'] == 2
    if not up and (event['function'] not in ('DrawPrimitive', 'DrawIndexedPrimitive') or event['phase'] != 1):
        raise ValueError('Requires a completed draw or finalized UP draw')
    shader = event['vertex_container']
    slots, textures = fetch_requirements(payload(shader, 'vertex'))
    if textures and not material:
        raise ValueError('Vertex textures are not included in this capture')
    args = [int(x, 16) for x in event['args']]
    draw_count = args[4] if event['function'] == 'BeginIndexedVertices' else args[2] if up else args[3]
    if not 0 < draw_count <= 4 * 1024 * 1024:
        raise ValueError('Draw count exceeds native replay limit')
    primitive = args[1]
    if primitive not in (4, 6, 13):
        raise ValueError('Mesh diagnostic supports triangle lists/strips and quad lists')
    geometry = event.get('geometry', [])
    by_kind = {g['kind']: g for g in geometry}
    streams = {g['fetch_slot']: g for g in geometry if 'fetch_slot' in g}
    stream_data = {}
    if up:
        if slots != {95}:
            raise ValueError('UP draw requires exactly the transient stream at slot 95')
        finalized = {g['kind']: g for g in event.get('up_geometry', [])}
        if 96 not in finalized:
            raise ValueError('Missing finalized UP vertex payload')
        stream_data[95] = (payload(finalized[96]['sha256']), 2)
        if event['function'] == 'BeginIndexedVertices':
            if args[2] != 0 or 97 not in finalized:
                raise ValueError('Missing UP index data or unsupported minimum index')
            vertex_count, stride = args[3], args[6]
            width, endian = (4, 2) if args[5] & 4 else (2, 1)
            count = args[4]
            raw = payload(finalized[97]['sha256'])
            if len(raw) != ((count * width + 3) // 4) * 4:
                raise ValueError('Finalized UP index allocation has the wrong length')
            indices = bytearray(raw[:count * width])
        else:
            vertex_count, stride = args[2:4]
            count, width, endian = vertex_count, 4, 0
            indices = bytearray(struct.pack('<' + 'I' * count, *range(count)))
        if not stride or len(stream_data[95][0]) != vertex_count * stride:
            raise ValueError('Finalized UP vertex allocation has the wrong length')
    else:
        if not slots <= streams.keys():
            raise ValueError('Missing vertex payload')
        stream_data = {slot: (payload(streams[slot]['sha256']), streams[slot]['endian']) for slot in slots}
        count = args[3]
        if event['function'] == 'DrawIndexedPrimitive':
            if not {30, 31} <= by_kind.keys():
                raise ValueError('Missing index payload')
            flags = struct.unpack_from('>I', payload(by_kind[30]['sha256']))[0]
            width, endian = (4 if flags & 0x80000000 else 2), (flags >> 29) & 3
            indices = bytearray(payload(by_kind[31]['sha256']))
        else:
            width, endian = 4, 0
            indices = bytearray(struct.pack('<' + 'I' * count, *range(args[2], args[2] + count)))
    if len(indices) != width * count:
        raise ValueError('Index count does not match payload')
    if width == 2 and endian > 1:
        raise ValueError('32-bit endian groups for 16-bit indices need aligned capture bounds')
    if endian in (1, 2):
        indices[0::2], indices[1::2] = indices[1::2], indices[0::2]
    if endian in (2, 3):
        for i in range(0, len(indices), 4):
            indices[i:i+4] = indices[i+2:i+4] + indices[i:i+2]
    # Native triangle lists preserve quad edge order. Triangle strips are also
    # expanded so Plume's implicit restart enable cannot change guest indices.
    if primitive in (6, 13):
        values = struct.unpack('<' + ('H' if width == 2 else 'I') * count, indices)
        if primitive == 13:
            if count % 4:
                raise ValueError('Incomplete quad list')
            triangles = [values[b + j] for b in range(0, count, 4) for j in (0, 1, 2, 0, 2, 3)]
        else:
            device_mode = struct.unpack_from('>I', payload(event['device_state']), 0x2948)[0]
            if device_mode & (1 << 21):
                raise ValueError('Triangle strip restart conversion not implemented')
            triangles = [values[j] for i in range(2, count) for j in ((i-2, i-1, i) if i % 2 == 0 else (i-1, i-2, i))]
        indices = struct.pack('<' + ('H' if width == 2 else 'I') * len(triangles), *triangles)
        primitive, count = 4, len(triangles)
    if count > 4 * 1024 * 1024:
        raise ValueError('Expanded draw count exceeds native replay limit')
    device = payload(event['device_state'])

    def host_words(offset, count):
        return struct.pack('<' + 'I' * count, *struct.unpack_from('>' + 'I' * count, device, offset))

    target_width, target_height = 1280, 720
    if material:
        surfaces = {s['kind']: s for s in event.get('surfaces', [])}
        if 6 not in surfaces:
            raise ValueError('Material preview requires a color target')
        dimensions = struct.unpack_from('>I', payload(surfaces[6]['sha256']), 0x24)[0]
        target_width, target_height = ((dimensions >> 18) & 0x3fff) + 1, ((dimensions >> 3) & 0x7fff) + 1
    out = (b'G3MESH04' if material else b'G3MESH01') + struct.pack('<6I', target_width, target_height, primitive, width, count, len(slots))
    if material:
        out += host_words(0x2880, (0x2a60 - 0x2880) // 4)
    out += host_words(0x780, 1024)
    if material:
        out += host_words(0x1780, 1024)
    out += host_words(0x2780, 8) + host_words(0x27A0, 32)
    for slot in sorted(slots):
        data, endian = stream_data[slot]
        out += struct.pack('<3I', slot, len(data), endian) + data
    out += indices
    if material:
        if not event.get('pixel_container'):
            raise ValueError('Material replay requires a pixel shader')
        _, pixel_textures = fetch_requirements(payload(event['pixel_container'], 'pixel'))
        textures |= pixel_textures
        bindings = {(t['slot'], t['region']): t for t in event.get('textures', [])}
        out += struct.pack('<I', len(textures))
        for slot in sorted(textures):
            fetch = struct.unpack_from('>6I', device, 0x480 + slot * 24)
            base, mips = bindings.get((slot, 'base')), bindings.get((slot, 'mips'))
            if fetch[0] & 3 != 2 or not (base or mips):
                raise ValueError(f'Missing texture snapshot for slot {slot}')
            base_data = payload(base['sha256']) if base else b''
            mip_data = payload(mips['sha256']) if mips else b''
            out += struct.pack('<9I', slot, *fetch, len(base_data), len(mip_data)) + base_data + mip_data
    return out, shader


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='Inspected capture directory')
    parser.add_argument('output', type=Path)
    parser.add_argument('--event', type=int, help='Event index; by default select the first eligible color draw')
    parser.add_argument('--material', action='store_true', help='Include actual pixel constants and CPU texture snapshots (capture v3)')
    args = parser.parse_args()
    report = json.loads((args.capture / 'capture.json').read_text())
    candidates = [(args.event, report['events'][args.event])] if args.event is not None else enumerate(report['events'])
    for index, event in candidates:
        if args.event is None and not event.get('pixel_container'):
            continue
        try:
            data, shader = export(args.capture, event, args.material)
        except (KeyError, ValueError) as error:
            if args.event is not None:
                raise ValueError(f'Event {index}: {error}') from error
            continue
        args.output.write_bytes(data)
        metadata = dict(event=index, vertex_shader=shader, sha256=hashlib.sha256(data).hexdigest(),
                        pixel_shader=event.get('pixel_container') if args.material else None,
                        scope=('Captured material/CPU textures/raster state; isolated draw with empty depth, RGBA8 color target; prior passes/GPU-only resolves not replayed'
                               if args.material else 'Geometry preview only; substitute constant-color pixel shader and native raster state'))
        args.output.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(json.dumps(metadata))
        return
    raise SystemExit('No eligible captured draw')


if __name__ == '__main__':
    main()
