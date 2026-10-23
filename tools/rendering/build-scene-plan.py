#!/usr/bin/env python3
"""Recover ordered render-pass dependencies across consecutive Gears frames.

This validates the capture-to-renderer contract. It does not claim that the
result is executable or that CPU texture snapshots contain GPU resolve output.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
from functools import lru_cache
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('mesh_export', Path(__file__).with_name('export-mesh-replay.py'))
mesh_export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mesh_export)

# This built-in vertex template is affine in one float4 POSITION input. Its
# three UP vertices describe a rectangle; the fourth can be interpolated before
# the shader without changing the result. Other unlinked programs stay rejected.
RECTANGLE_TEMPLATE = '9d8adccbaa369d9fa2106a03a54a997ec417e12fcbdb4bed7e67c36dfadf8a01'


def physical_address(address):
    """Supported XEX's D3D physical resource alias conversion (82AF34A8)."""
    return (address & 0x1fffffff) + (((address >> 20) + 0x200) & 0x1000)


def texture_identity(fetch, resource=False):
    f = list(fetch)
    if resource:
        f[1] = (f[1] & 0xfff) | physical_address(f[1] & 0xfffff000)
    dimension = (f[5] >> 9) & 3
    if dimension != 1:
        raise ValueError('Scene resolve identity currently supports 2D textures only')
    format_id = f[1] & 63
    # _EXPAND resources are resolved into their floating texture equivalents.
    format_id = {27: 30, 28: 31, 29: 32, 54: 7, 55: 16, 56: 17}.get(format_id, format_id)
    return dict(base=f'{f[1] & 0xfffff000:08x}', format=format_id,
                width=(f[2] & 8191) + 1, height=((f[2] >> 13) & 8191) + 1,
                pitch=((f[0] >> 22) & 511) * 32, tiled=bool(f[0] >> 31),
                endian=(f[1] >> 6) & 3, dimension=dimension,
                packed=bool(f[5] & 2048),
                layers=(f[2] >> 26) + 1 if f[1] & 1024 else 1)


def identity_key(texture):
    return tuple(texture[k] for k in ('base', 'format', 'width', 'height', 'pitch', 'tiled', 'endian', 'dimension')) + (texture.get('layers', 1), texture.get('packed', False))


def storage_geometry(texture):
    """Base-level layout matching native/texture_data.cpp, in storage blocks."""
    formats = {2: (1, 1), 8: (1, 1), 9: (1, 1), 10: (1, 2),
               6: (1, 4), 7: (1, 4), 14: (1, 4), 50: (1, 4),
               18: (4, 8), 19: (4, 16), 20: (4, 16), 49: (4, 16),
               51: (4, 8), 52: (4, 16), 53: (4, 16), 59: (4, 8),
               22: (1, 4), 23: (1, 4), 24: (1, 2), 25: (1, 4),
               26: (1, 8), 30: (1, 2), 31: (1, 4), 32: (1, 8),
               36: (1, 4), 37: (1, 8), 38: (1, 16)}
    block, size = formats[texture['format']]
    width, height = texture['width'], texture['height']
    pitch = ((texture['pitch'] + block - 1) // block + 31) & ~31
    rows = ((height + block - 1) // block + 31) & ~31
    if not pitch or not width or not height or width > 8192 or height > 8192:
        raise ValueError('Invalid texture storage dimensions')
    px = py = 0
    if texture.get('packed') and min((width - 1).bit_length(), (height - 1).bit_length()) <= 4:
        if (width - 1).bit_length() > (height - 1).bit_length(): py = 16 // block
        else: px = 16 // block
    return block, size, pitch, rows, px, py


def tiled_offset(x, y, pitch, size):
    # Xenos addressing; see native/texture_data.LICENSE for attribution.
    log_bytes = size.bit_length() - 1
    macro = ((x >> 5) + (y >> 5) * ((pitch + 31) >> 5)) << (log_bytes + 7)
    micro = ((x & 7) + ((y & 14) << 2)) << log_bytes
    offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 1) << 4)
    return ((offset & ~511) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) + \
        (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63)


@lru_cache(maxsize=2048)
def _storage_ranges(key, rectangle):
    base, fmt, width, height, pitch, tiled, endian, dimension, layers, packed = key
    texture = dict(base=base, format=fmt, width=width, height=height, pitch=pitch,
                   tiled=tiled, endian=endian, dimension=dimension, layers=layers, packed=packed)
    block, size, pitch, rows, px, py = storage_geometry(texture)
    left, top, right, bottom = rectangle
    if left < 0 or top < 0 or right > width or bottom > height or left >= right or top >= bottom:
        raise ValueError('Invalid texture storage rectangle')
    left, top, right, bottom = left // block + px, top // block + py, \
        (right + block - 1) // block + px, (bottom + block - 1) // block + py
    stride = (pitch * rows * size + 4095) & ~4095
    base = int(base, 16)
    ranges = []
    for layer in range(layers):
        start = base + layer * stride
        for y in range(top, bottom):
            # Endian swizzles exchange bytes only within each dword. Split
            # sub-dword partial runs precisely, including scalar formats.
            if not tiled and size >= 4:
                ranges.append((start + (y * pitch + left) * size, start + (y * pitch + right) * size))
                continue
            x = left
            while x < right:
                run = min(8, 16 // size) if tiled else max(1, 4 // size)
                count = min(right - x, run - x % run)
                offset = tiled_offset(x, y, pitch, size) if tiled else (y * pitch + x) * size
                first, last = start + offset, start + offset + count * size
                if size < 4 and (first % 4 or last % 4):
                    swap = (0, 1, 3, 2)[endian]
                    ranges.extend((at ^ swap, (at ^ swap) + 1) for at in range(first, last))
                else:
                    ranges.append((first, last))
                x += count
    merged = []
    for first, last in sorted(ranges):
        if first < 0 or last > 512 * 1024 * 1024:
            raise ValueError('Texture storage exceeds physical memory')
        if merged and first <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(last, merged[-1][1]))
        else: merged.append((first, last))
    return tuple(merged)


def storage_ranges(texture, rectangle=None):
    return _storage_ranges(identity_key(texture), tuple(rectangle or (0, 0, texture['width'], texture['height'])))


def ranges_overlap(a, b):
    i = j = 0
    while i < len(a) and j < len(b):
        if a[i][0] < b[j][1] and b[j][0] < a[i][1]: return True
        if a[i][1] <= b[j][1]: i += 1
        else: j += 1
    return False


class GPURegions:
    """Written bytes, excluding allocation padding and unrelated CPU assets."""
    def __init__(self): self.regions = []

    def add(self, texture, rectangle=None):
        self.regions.append(storage_ranges(texture, rectangle))

    def overlaps(self, texture):
        # A cheap conservative envelope avoids enumerating large CPU assets
        # whose addresses are nowhere near any render-target resolve.
        try: block, size, pitch, rows, px, py = storage_geometry(texture)
        except KeyError: return False  # Unsupported CPU formats stay CPU inputs.
        first = int(texture['base'], 16)
        stride = (pitch * rows * size + 4095) & ~4095
        columns = (texture['width'] + block - 1) // block + px
        end_row = (texture['height'] + block - 1) // block + py
        if texture['tiled']:
            # Include a whole final macro tile and its swizzled microtiles;
            # unlike width*pitch, this remains conservative when width>pitch.
            extent = (((end_row - 1) // 32) * (pitch // 32) + (columns - 1) // 32 + 2) * 1024 * size + 4096
        else:
            extent = ((end_row - 1) * pitch + columns) * size
        last = first + stride * (texture.get('layers', 1) - 1) + extent
        candidates = [r for r in self.regions if r and first < r[-1][1] and r[0][0] < last]
        return bool(candidates) and any(ranges_overlap(storage_ranges(texture), r) for r in candidates)


class ResolveHistory:
    """Retains partial writes; a full overwrite removes prior dependencies."""
    def __init__(self):
        self.writes = {}
        self.views = {}

    def write(self, texture, rectangle, event):
        key = identity_key(texture)
        full = rectangle == [0, 0, texture['width'], texture['height']]
        written = storage_ranges(texture, rectangle)
        # A differently based image may alias any of the exact written bytes.
        for old in tuple(self.writes):
            if old != key and ranges_overlap(written, self.views[old]):
                del self.writes[old]
                del self.views[old]
        prior = [] if full else self.writes.get(key, [])
        self.writes[key] = prior + [dict(event=event, rectangle=rectangle, full=full)]
        self.views[key] = storage_ranges(texture)

    def read(self, texture):
        return list(self.writes.get(identity_key(texture), []))


def build(directory):
    report = json.loads((directory / 'capture.json').read_text())
    if report['version'] < 5 or report['footer']['reason'] != 'frames_complete':
        raise ValueError('Requires a completed version-5 capture')
    frames = []
    presented = set()
    for event in report['events']:
        frame = event['frame']
        if not frames or frames[-1] != frame:
            if frames and (frame != frames[-1] + 1 or frames[-1] not in presented):
                raise ValueError('Capture frames must be consecutive and end with Present')
            frames.append(frame)
        if frame in presented:
            raise ValueError('Operation recorded after the frame Present')
        if event['function'] == 'Present' and event['phase'] == 0:
            presented.add(frame)
    if not frames or frames[-1] not in presented:
        raise ValueError('Capture must end with Present')
    cache = {}

    def payload(digest, suffix='payload'):
        key = digest, suffix
        if key not in cache:
            if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
                raise ValueError('Invalid payload digest')
            data = (directory / f'{digest}.{suffix}.bin').read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError('Capture payload hash mismatch')
            cache[key] = data
        return cache[key]

    def surface(binding):
        b = payload(binding['sha256'])
        surface_info, format_info = struct.unpack_from('>2I', b, 0x18)
        dimensions = struct.unpack_from('>I', b, 0x24)[0]
        return dict(address=binding['address'], depth=binding['kind'] == 10,
                    width=((dimensions >> 18) & 0x3fff) + 1, height=((dimensions >> 3) & 0x7fff) + 1,
                    surface_info=surface_info, format_info=format_info,
                    edram_base=format_info & 2047, format=(format_info >> 16) & (1 if binding['kind'] == 10 else 15))

    operations, issues, counts, destinations = [], [], Counter(), {}
    resource_headers = {}
    for index, event in enumerate(report['events']):
        if event['function'] not in ('ClearRect', 'Resolve'):
            continue
        params = {p['kind']: payload(p['sha256']) for p in event.get('pass_parameters', [])}
        if event['function'] == 'Resolve':
            if 100 not in params:
                raise ValueError(f'Event {index}: missing resolve destination')
            destinations[index] = texture_identity(struct.unpack_from('>6I', params[100], 28), True)
    gpu_regions = GPURegions()
    for index, texture in destinations.items():
        event = report['events'][index]
        params = {p['kind']: payload(p['sha256']) for p in event.get('pass_parameters', [])}
        rectangle = struct.unpack('>4i', params[98]) if 98 in params else (0, 0, texture['width'], texture['height'])
        x, y = struct.unpack('>2i', params[101]) if 101 in params else (0, 0)
        gpu_regions.add(texture, (x, y, x + rectangle[2] - rectangle[0], y + rectangle[3] - rectangle[1]))
    history = ResolveHistory()
    for index, event in enumerate(report['events']):
        name, phase = event['function'], event['phase']
        draw = (name in ('DrawPrimitive', 'DrawIndexedPrimitive') and phase == 1) or phase == 2
        if not draw and name not in ('ClearRect', 'Resolve', 'Present'):
            continue
        args = [int(a, 16) for a in event['args']]
        surfaces = {s['kind']: surface(s) for s in event.get('surfaces', [])}
        state = payload(event['device_state'])
        operation = dict(event=index, frame=event['frame'], type='draw' if draw else name.lower(),
                         surfaces={str(k): v for k, v in surfaces.items()})
        params = {p['kind']: payload(p['sha256']) for p in event.get('pass_parameters', [])}
        if name == 'ClearRect':
            if 98 not in params or 102 not in params:
                raise ValueError(f'Event {index}: incomplete clear parameters')
            rectangle = list(struct.unpack('>4i', params[98]))
            vx, vy, vw, vh = struct.unpack_from('>4f', state, 0x3260)
            if not all(math.isfinite(v) for v in (vx, vy, vw, vh)):
                raise ValueError('Non-finite clear viewport')
            rectangle = [max(rectangle[0], int(vx)), max(rectangle[1], int(vy)),
                         min(rectangle[2], int(vx) + int(vw)), min(rectangle[3], int(vy) + int(vh))]
            if struct.unpack_from('>I', state, 0x2f80)[0]:
                scissor = struct.unpack_from('>4i', state, 0x327c)
                rectangle = [max(rectangle[0], scissor[0]), max(rectangle[1], scissor[1]),
                             min(rectangle[2], scissor[2]), min(rectangle[3], scissor[3])]
            operation.update(flags=args[1], rectangle=rectangle, depth=struct.unpack_from('>d', params[102])[0],
                             stencil=args[4],
                             color=list(struct.unpack('>4f', params[99])) if 99 in params else None)
            if args[1] & 0xc0:
                issues.append(dict(event=index, reason='Special stencil clear mask requires native implementation'))
        elif name == 'Resolve':
            texture = destinations[index]
            rectangle = list(struct.unpack('>4i', params[98])) if 98 in params else [0, 0, texture['width'], texture['height']]
            point = list(struct.unpack('>2i', params[101])) if 101 in params else [0, 0]
            slot = args[1] & 7
            if slot > 4 or 6 + slot not in surfaces:
                raise ValueError(f'Event {index}: resolve has no source surface')
            dest_rect = [*point, point[0] + rectangle[2] - rectangle[0], point[1] + rectangle[3] - rectangle[1]]
            operation.update(source_slot=slot, flags=args[1], source_rectangle=rectangle,
                             destination_rectangle=dest_rect, destination=texture, level=args[5],
                             destination_fetch=list(struct.unpack_from('>6I', params[100], 28)),
                             exponent_bias=((args[1] >> 26) ^ 32) - 32)
            # Guest Resolve 82AF36D4/82AF3804 packs the first destination
            # fetch selector's nonzero test into COPY_DEST_INFO bit 24.
            # Depth copies ignore the swap bit.
            operation['copy_dest_swap'] = slot != 4 and bool(operation['destination_fetch'][3] & 0xe)
            if args[5] or args[1] & 0x2300:
                issues.append(dict(event=index, reason='Resolve mip/raw/clear flags require native implementation'))
            history.write(texture, dest_rect, index)
            resource_headers[next(p['address'] for p in event['pass_parameters'] if p['kind'] == 100)] = params[100]
        elif name == 'Present':
            header = params.get(103)
            provenance = 'captured_present_resource'
            if header is None:
                header = resource_headers.get(event['args'][1])
                provenance = 'prior_resolve_resource'
            if header is None:
                issues.append(dict(event=index, reason='Present frontbuffer resource was not captured'))
            else:
                fetch = list(struct.unpack_from('>6I', header, 28))
                texture = texture_identity(fetch, True)
                fetch[1] = (fetch[1] & 0xfff) | int(texture['base'], 16)
                operation.update(frontbuffer=texture, frontbuffer_fetch=fetch, provenance=provenance)
        elif draw:
            operation.update(vertex_shader=event.get('vertex_container'), pixel_shader=event.get('pixel_container'),
                             device_state=event['device_state'], geometry=event.get('geometry', []),
                             up_geometry=event.get('up_geometry', []), function=name, args=args, textures=[])
            slots, vertices = set(), set()
            for stage in ('vertex', 'pixel'):
                digest = event.get(stage + '_container')
                if not digest:
                    if stage == 'vertex': issues.append(dict(event=index, reason='Missing vertex shader'))
                    continue
                try:
                    vertex_slots, required = mesh_export.fetch_requirements(payload(digest, stage))
                    vertices |= vertex_slots
                    slots |= required
                except ValueError as error:
                    if (stage == 'vertex' and digest == RECTANGLE_TEMPLATE and
                            name == 'BeginVertices' and phase == 2 and args[1:4] == [8, 3, 16]):
                        operation['vertex_input'] = 'affine_float4_rectangle'
                    else:
                        issues.append(dict(event=index, reason=str(error)))
            operation['vertex_slots'] = sorted(vertices)
            snapshots = {(t['slot'], t['region']): t['sha256'] for t in event.get('textures', [])}
            for slot in sorted(slots):
                fetch = struct.unpack_from('>6I', state, 0x480 + slot * 24)
                binding = dict(slot=slot, fetch=list(fetch),
                               base_payload=snapshots.get((slot, 'base')), mip_payload=snapshots.get((slot, 'mips')))
                if fetch[0] & 3 != 2:
                    binding['source'] = 'invalid_binding'
                    issues.append(dict(event=index, slot=slot, reason='Required texture has an invalid fetch constant'))
                elif (fetch[5] >> 9) & 3 != 1:
                    binding['source'] = 'cpu_snapshot'
                else:
                    texture = texture_identity(fetch)
                    writes = history.read(texture)
                    binding.update(identity=texture, producers=writes)
                    if writes:
                        binding['source'] = 'gpu_resolve'
                        if not any(w['full'] for w in writes):
                            binding['needs_prior_contents'] = True
                    elif gpu_regions.overlaps(texture):
                        binding['source'] = 'gpu_alias_or_previous_frame'
                    else:
                        binding['source'] = 'cpu_snapshot'
                counts[binding['source']] += 1
                operation['textures'].append(binding)
            counts['draws'] += 1
        if not draw:
            counts[operation['type']] += 1
        operations.append(operation)
    return dict(version=3, executable=False, frames=frames,
                scope='Ordered consecutive-frame dependency plan; initial GPU history and scene equivalence must be verified',
                counts=dict(counts), issues=issues, operations=operations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    plan = build(args.capture)
    args.output.write_text(json.dumps(plan, indent=2) + '\n')
    print(json.dumps(dict(counts=plan['counts'], issues=len(plan['issues']), executable=plan['executable'])))


if __name__ == '__main__':
    main()
