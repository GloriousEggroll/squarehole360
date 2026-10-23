#!/usr/bin/env python3
"""Validate a G3D3D001 API-boundary capture and summarize guest resource bindings.

This format records observations made before Xenia's GPU command translation.
Versions 2-6 add geometry, CPU textures, finalized UP writes, pass parameters and frontbuffer identity.
GPU-only resolves are not read back. Shader containers can be reconstructed only when both blocks
are present and their lengths agree with the container header.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

FUNCTIONS = {
    0x82AEFF00: "DrawPrimitive", 0x82AF0318: "DrawIndexedPrimitive",
    0x82AF0790: "SpecializedDraw", 0x82AF99B0: "InitializeDrawCommandBuffer",
    0x82AECC98: "SetRenderTarget", 0x82AED028: "SetDepthStencilSurface",
    0x82AF34A8: "Resolve", 0x82AF6B10: "Present",
    0x82AF1D50: "Clear",
    0x82AF16D8: "ClearRect",
    0x82AEF500: "BeginVertices", 0x82AEF9C0: "BeginIndexedVertices",
}


def inspect(data, payloads=None):
    if len(data) < 16 or data[:8] != b"G3D3D001":
        raise ValueError("Unknown capture header/ABI")
    version, device_size = struct.unpack_from('<II', data, 8)
    if version not in (1, 2, 3, 4, 5, 6) or device_size != 0x3600:
        raise ValueError("Unknown capture header/ABI")
    offset, events, shaders, counts = 16, [], {}, Counter()
    inline_blocks, references = {}, 0
    view = memoryview(data)
    missing = Counter()
    footer = None
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError("Truncated record header")
        size, function = struct.unpack_from("<II", data, offset)
        if size < 16 or size > len(data) - offset:
            raise ValueError("Truncated/invalid record length")
        end = offset + size
        if not function:
            if size != 16 or end != len(data):
                raise ValueError("Invalid capture footer")
            reason, count = struct.unpack_from("<II", data, offset + 8)
            if reason > 2 or count != len(events):
                raise ValueError("Footer count/reason mismatch")
            footer = {"reason": ["frames_complete", "size_limit", "shutdown"][reason], "events": count}
            offset = end
            break
        if size < 60 or function not in FUNCTIONS:
            raise ValueError("Invalid event header")
        phase, frame, lr, *rest = struct.unpack_from("<13I", data, offset + 8)
        args, result, block_count = rest[:8], rest[8], rest[9]
        if phase > (2 if version >= 4 else 1) or block_count not in {1: (10, 13), 2: (31,), 3: (95,), 4: (97,), 5: (102,), 6: (103,)}[version]:
            raise ValueError("Invalid event phase/block count")
        if phase == 2 and function not in (0x82AEF500, 0x82AEF9C0):
            raise ValueError("Finalized UP phase used by a non-UP function")
        cursor, blocks = offset + 60, {}
        for _ in range(block_count):
            if end - cursor < 12:
                raise ValueError("Truncated block header")
            kind, address, length = struct.unpack_from("<III", data, cursor)
            cursor += 12
            if kind in blocks or not 1 <= kind <= block_count:
                raise ValueError("Invalid capture block")
            if version >= 2 and length & 0x80000000:
                reference = length & 0x7FFFFFFF
                if reference not in inline_blocks:
                    raise ValueError("Invalid/forward block reference")
                block = inline_blocks[reference]
                references += 1
            else:
                if length > end - cursor:
                    raise ValueError("Invalid capture block")
                block = view[cursor:cursor + length]
                if length:
                    inline_blocks[cursor] = block
                cursor += length
            blocks[kind] = (address, block)
            if not length:
                missing[str(kind)] += 1
        if cursor != end:
            raise ValueError("Trailing event bytes")
        event = {"function": FUNCTIONS[function], "address": f"{function:08x}", "phase": phase,
                 "frame": frame, "lr": f"{lr:08x}", "args": [f"{x:08x}" for x in args],
                 "result": f"{result:08x}"}
        device = blocks[1][1]
        if device:
            if len(device) != 0x3600:
                raise ValueError("Device state size mismatch")
            fields = {"vertex_shader": 0x3290, "pixel_shader": 0x328C, "vertex_declaration": 0x2F58,
                      "index_buffer": 0x318C, "depth_surface": 0x31A0}
            event["device"] = {name: f"{struct.unpack_from('>I', device, pos)[0]:08x}" for name, pos in fields.items()}
        for stage, virtual_kind, physical_kind in [("vertex", 2, 3), ("pixel", 4, 5)]:
            virtual, physical = blocks[virtual_kind][1], blocks[physical_kind][1]
            if not virtual or not physical:
                continue
            if len(virtual) < 36:
                raise ValueError("Truncated shader container")
            flags, virtual_size, physical_size = struct.unpack_from(">III", virtual)
            # Match ShaderBlocks: bit 4 marks the observed memory-export vertex
            # variant, which can remain bound during non-draw state records.
            # This validates its captured container, not permission to execute
            # an arbitrary memory-export program in the native raster path.
            allowed = 0x71 if stage == "vertex" else 0
            if ((flags & ~allowed) != 0x102A1100 or bool(flags & 1) != (stage == "vertex") or
                    virtual_size != len(virtual) or physical_size != len(physical)):
                raise ValueError("Shader blocks disagree with container")
            blob = bytes(virtual) + bytes(physical)
            digest = hashlib.sha256(blob).hexdigest()
            shaders[digest] = (stage, blob)
            event[stage + "_container"] = digest
        if version >= 2:
            geometry = []
            for kind in (1, *range(14, 32)):
                address, block = blocks[kind]
                if not block:
                    continue
                digest = hashlib.sha256(block).hexdigest()
                if payloads is not None:
                    payloads[digest] = block
                if kind == 1:
                    event['device_state'] = digest
                else:
                    binding = dict(kind=kind, address=f'{address:08x}', bytes=len(block), sha256=digest)
                    if 14 <= kind <= 29:
                        binding['fetch_slot'] = 95 - (kind - 14)
                        if device:
                            binding['endian'] = struct.unpack_from('>I', device, 0x480 + binding['fetch_slot'] * 8 + 4)[0] & 3
                    geometry.append(binding)
            if geometry:
                event['geometry'] = geometry
        if version >= 3:
            resources, textures = [], []
            for kind in (*range(6, 11), *range(32, 96)):
                address, block = blocks[kind]
                if not block:
                    continue
                digest = hashlib.sha256(block).hexdigest()
                if payloads is not None:
                    payloads[digest] = block
                binding = dict(kind=kind, address=f'{address:08x}', bytes=len(block), sha256=digest)
                if kind < 32:
                    resources.append(binding)
                else:
                    binding.update(slot=(kind - 32) // 2, region='mips' if (kind - 32) % 2 else 'base',
                                   provenance='guest_cpu_memory')
                    if device:
                        binding['fetch'] = list(struct.unpack_from('>6I', device, 0x480 + binding['slot'] * 24))
                    textures.append(binding)
            if resources:
                event['surfaces'] = resources
            if textures:
                event['textures'] = textures
        if version >= 4 and phase == 2:
            event['up_geometry'] = []
            for kind in (96, 97):
                address, block = blocks[kind]
                if block:
                    digest = hashlib.sha256(block).hexdigest()
                    if payloads is not None:
                        payloads[digest] = block
                    event['up_geometry'].append(dict(kind=kind, address=f'{address:08x}', bytes=len(block), sha256=digest))
        if version >= 5:
            parameters = []
            for kind, size in ((98, 16), (99, 16), (100, 64), (101, 8), (102, 12), *(([(103, 64)]) if version >= 6 else [])):
                address, block = blocks[kind]
                if not block:
                    continue
                if len(block) != size:
                    raise ValueError('Invalid clear/resolve parameter size')
                digest = hashlib.sha256(block).hexdigest()
                if payloads is not None:
                    payloads[digest] = block
                parameters.append(dict(kind=kind, address=f'{address:08x}', bytes=size, sha256=digest))
            if parameters:
                event['pass_parameters'] = parameters
        counts[FUNCTIONS[function] + ("_finalized" if phase == 2 else "_after" if phase else "")] += 1
        events.append(event)
        offset = end
    if footer is None:
        raise ValueError("Capture is incomplete: footer missing")
    return {"format": "G3D3D001", "version": version, "replayable": False,
            "block_references": references,
            "limitation": ("Texture snapshots are CPU memory; GPU-only resolves are not captured" if version >= 4 else
                           "Texture snapshots are CPU memory; GPU-only resolves and deferred UP writes are not captured"
                           if version == 3 else "Textures and deferred UP writes are not captured" if version == 2
                           else "Texture/vertex payloads and deferred UP writes are not captured"),
            "footer": footer, "counts": dict(counts), "missing_blocks": dict(missing),
            "shaders": [{"sha256": h, "stage": s, "bytes": len(b)} for h, (s, b) in shaders.items()],
            "events": events}, shaders


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = args.capture.read_bytes()
    payloads = {}
    report, shaders = inspect(data, payloads)
    args.output.mkdir(parents=True, exist_ok=True)
    for digest, (stage, blob) in shaders.items():
        (args.output / f"{digest}.{stage}.bin").write_bytes(blob)
    for digest, blob in payloads.items():
        (args.output / f'{digest}.payload.bin').write_bytes(blob)
    manifest = dict(source=str(args.capture), source_sha256=hashlib.sha256(data).hexdigest(),
                    status='Captured runtime shader containers; compilation is not image equivalence',
                    shaders=[dict(s, path=f"{s['sha256']}.{s['stage']}.bin") for s in report['shaders']])
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (args.output / "capture.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("footer", "counts", "missing_blocks", "shaders")}, indent=2))


if __name__ == "__main__":
    main()
