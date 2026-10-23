#!/usr/bin/env python3
"""Small Xenos programs with independently predictable GPU results.

These exercise instruction semantics, not just generated HLSL text. They use
the same container reader and converter as the captured Gears shaders.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def words(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def alu(dst, src1, src2=None, *, export=False, constant=False, add=False, relative=False):
    if src2 is None:
        src2 = src1
    return words(dst | (int(export) << 15) | (15 << 16) | (50 << 26),
                 int(relative) << 30,
                 src2 << 8 | src1 << 16 | (0 if add else 2) << 24 |
                 (0 if constant else 1) << 31 | 1 << 29 | (0 if constant else 1) << 30)


def fetch(opcode, src, dst, *, dimension=1, computed=False, denormalized=False, predicated=False):
    return words(opcode | src << 5 | dst << 12 | 1 << 19 | int(denormalized) << 25 | 36 << 26,
                 0x688 | 0xF1FF << 12 | int(computed) << 28 | int(predicated) << 31,
                 dimension << 14 | int(predicated) << 31)


def program(instructions, *, vertex=False, interpolator=False, elements=(), linked=False):
    # Two CF words: ALLOC outputs, EXEC_END a contiguous instruction block.
    sequence = sum(int(is_fetch) << (2 * i) for i, (_, is_fetch) in enumerate(instructions))
    cf = [(12 << 44) | (2 << 41), (2 << 44) | 1 | len(instructions) << 12 | sequence << 16]
    return container(cf, [b for b, _ in instructions], vertex=vertex,
                     interpolator=interpolator, elements=elements, linked=linked)


def container(cf, instructions, *, vertex=False, interpolator=False, elements=(), linked=False):
    assert len(cf) % 2 == 0
    physical = b''
    for a, b in zip(cf[::2], cf[1::2]):
        physical += words(a & 0xFFFFFFFF, (a >> 32) | (b & 0xFFFF) << 16, b >> 16)
    physical += b''.join(instructions)
    # A high SV_Position register keeps r0 available for accumulators.
    metadata = words(0, len(physical), 0, 63 << 8, 0, int(interpolator) << 5)
    metadata += words(0, len(elements), 0) if vertex else words(0, 1)
    metadata += words(*elements)
    if interpolator:
        metadata += words(5 << 4)  # TEXCOORD0 -> r0 / export 0.
    virtual_size = 36 + len(metadata)
    return words(0x102A1100 | int(vertex) | (0x40 if linked else 0), virtual_size, len(physical), 0, 0, 0, 36, 0, 0) + metadata + physical


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixtures = {
        'gradients': program([(fetch(18, 0, 1), True), (alu(0, 1, export=True), False)], interpolator=True),
        'texture-1d': program([(alu(1, 7, constant=True), False), (fetch(1, 1, 2, dimension=0), True),
                               (alu(0, 2, export=True), False)]),
        'texture-2d': program([(alu(1, 7, constant=True), False), (fetch(1, 1, 2), True),
                               (alu(0, 2, export=True), False)]),
        'texture-stacked': program([(alu(1, 7, constant=True), False), (fetch(1, 1, 2, dimension=2), True),
                                    (alu(0, 2, export=True), False)]),
        'texture-2d-unnormalized': program([(alu(1, 7, constant=True), False), (fetch(1, 1, 2, denormalized=True), True),
                                            (alu(0, 2, export=True), False)]),
        'passthrough': program([(alu(0, 0, export=True), False)], interpolator=True),
    }
    # Vector KILL observes every component even if no destination is written.
    # c7 is compared to zero (c8); a surviving pixel exports fixed color c9.
    # Partial masks deliberately omit Y/Z/W so their kill conditions cannot be
    # accidentally reduced to the destination's live components.
    for operation, opcode in (('eq', 24), ('gt', 25), ('ge', 26), ('ne', 27)):
        for mask in (0, 1, 5, 15):
            kill = words(1 | mask << 16 | 50 << 26, 0,
                         8 << 8 | 7 << 16 | opcode << 24 | 1 << 29)
            fixtures[f'vector-kill-{operation}-mask{mask}'] = program([
                (kill, False), (alu(0, 9, export=True, constant=True), False)])
    # Register LOD uses c8.x; sample coordinate uses c7.xy.
    lod_fetch = bytearray(fetch(1, 1, 2))
    struct.pack_into('>I', lod_fetch, 4, struct.unpack_from('>I', lod_fetch, 4)[0] | 1 << 29)
    fixtures['texture-explicit-lod'] = program([
        (alu(1, 8, constant=True), False), (fetch(24, 1, 0), True),
        (alu(1, 7, constant=True), False), (bytes(lod_fetch), True), (alu(0, 2, export=True), False)])
    # Full float4 position fetch, with metadata pointing to instruction 1.
    vertex_fetch = words(1 << 19 | 31 << 20 | 2 << 25, 0x688 | 38 << 16 | 3 << 12, 4)
    fixtures['vertex-texture-1d'] = program([
        (vertex_fetch, True), (alu(62, 0, export=True), False),
        (alu(1, 7, constant=True), False), (fetch(1, 1, 2, dimension=0), True),
        (alu(0, 2, export=True), False)], vertex=True, interpolator=True, elements=(1,), linked=True)
    # Nested loops, repeated inner loop inherits the outer address register.
    cf = [12 << 44 | 2 << 41, 7 << 44 | 6,
          7 << 44 | 5 | 1 << 13 | 1 << 16, 1 << 44 | 4 | 1 << 12,
          8 << 44 | 3 | 1 << 16, 8 << 44 | 2,
          2 << 44 | 5 | 1 << 12, 0]
    # src1 = r0, src2 = c[7+aL]. const0Relative selects the first constant.
    accumulate = words(15 << 16 | 50 << 26, 1 << 31, 7 << 8 | 1 << 31 | 1 << 29)
    fixtures['loops'] = container(cf, [accumulate, alu(0, 0, export=True)])
    # Each ALU pairs a vector and scalar operation. Both read pre-instruction
    # registers. The scalar previous-result register changes only afterwards.
    # r2 becomes (a,3*a,4*a,3*a), where a=c7.x.
    def paired(scalar_opcode, scalar_dst, scalar_mask):
        return words(1 | scalar_dst << 8 | 1 << 16 | scalar_mask << 20 | scalar_opcode << 26,
                     0x40, 1 | 1 << 8 | 1 << 16 | 7 << 29)
    retain = words(2 | 2 << 8 | 4 << 16 | 8 << 20 | 50 << 26,
                   0x20 << 8 | 0x20 << 16, 1 << 8 | 1 << 16 | 2 << 24 | 7 << 29)
    fixtures['alu-paired'] = program([(alu(1, 7, constant=True), False),
        (paired(5, 2, 1), False), (paired(1, 2, 2), False),
        (retain, False), (alu(0, 2, export=True), False)])
    # Explicit expected values, rather than a second implementation of the GPU
    # decoder. Full fetch selects slot 94, mini fetch reads at word offset 1.
    vertex_cases = [
        ('u8-normalized', 6, 0, 0, [0xFF008040], [64/255, 128/255, 0, 1]),
        ('u8-integer', 6, 2, 0, [0xFF008040], [64, 128, 0, 255]),
        ('s8-normalized', 6, 1, 0, [0x7FFF0080], [-1, 0, -1/127, 1]),
        ('s8-nozero', 6, 5, 0, [0x7FFF0080], [-1, .5/127.5, -.5/127.5, 1]),
        ('snorm11', 16, 1, 0, [2047 | 1023 << 11 | 511 << 22], [-1/1023, 1, 1, 0]),
        ('s16-normalized', 25, 1, 0, [0x7FFF8000], [-1, 1, 0, 0]),
        ('half2', 31, 3, 0, [0xC0003E00], [1.5, -2, 0, 0]),
        ('half4', 32, 3, 0, [0xC0003E00, 0xFC007FFF], [1.5, -2, 131008, -65536]),
        ('float1', 36, 3, 0, [0x3FA00000], [1.25, 0, 0, 0]),
        ('float2', 37, 3, 0, [0x3FA00000, 0xC0000000], [1.25, -2, 0, 0]),
        ('float3', 57, 3, 0, [0x3FA00000, 0xC0000000, 0x40800000], [1.25, -2, 4, 0]),
        ('float4', 38, 3, 0, [0x3FA00000, 0xC0000000, 0x40800000, 0x3F000000], [1.25, -2, 4, .5]),
        ('float-exponent', 38, 3, -2, [0x3FA00000, 0xC0000000, 0x40800000, 0x3F000000], [.3125, -.5, 1, .125]),
    ]
    for name, fmt, flags, exponent, raw_words, expected in vertex_cases:
        # Full fetch writes nothing; the mini operation must still inherit it.
        select = words(1 << 19 | 31 << 20 | 1 << 25, 0xFFF | 6 << 16, 0)
        mini = words(2 << 12, 0x688 | fmt << 16 | flags << 12 | (exponent & 63) << 24 | 1 << 30, 1 << 8)
        fixtures['vertex-' + name] = program([
            (vertex_fetch, True), (alu(62, 0, export=True), False),
            (select, True), (mini, True), (alu(0, 2, export=True), False)],
            vertex=True, interpolator=True, elements=(1, 3 | 5 << 12, 4 | 5 << 12 | 1 << 16), linked=True)
        cases = []
        raw_words = [0xDEADBEEF, *raw_words]
        for endian in range(4):
            raw = bytearray()
            for word in raw_words:
                b = struct.pack('<I', word)
                if endian in (1, 2): b = bytes((b[1], b[0], b[3], b[2]))
                if endian in (2, 3): b = b[2:] + b[:2]
                raw += b
            raw += bytes(32 - len(raw))
            cases.append(bytes(raw) + struct.pack('<4fII', *expected, endian, len(raw_words) * 4))
        # No readable data: every loaded word is zero, including missing words
        # within a format. No-zero signed normalization maps 0 to a half step.
        zero_expected = [.5/127.5] * 4 if flags == 5 else [0] * 4
        cases.append(bytes(32) + struct.pack('<4fII', *zero_expected, 0, 0))
        (args.output / ('vertex-' + name + '.cases')).write_bytes(b'G3VF0001' + struct.pack('<I', len(cases)) + b''.join(cases))
    shaders = []
    for name, data in fixtures.items():
        digest = hashlib.sha256(data).hexdigest()
        stage = 'vertex' if data[3] & 1 else 'pixel'
        path = f'{digest}.{stage}.bin'
        (args.output / path).write_bytes(data)
        shaders.append(dict(name=name, sha256=digest, stage=stage, path=path))
    manifest = dict(source='Synthetic Xenos semantic fixtures',
                    source_sha256=hashlib.sha256(b''.join(fixtures.values())).hexdigest(), shaders=shaders)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Wrote {len(fixtures)} instruction-semantic fixtures')


if __name__ == '__main__':
    main()
