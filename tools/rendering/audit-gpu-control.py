#!/usr/bin/env python3
"""Count GPU control dependencies in a user-owned reference trace.

This is a coverage audit, not a renderer or proof that commands can be skipped.
Packet payloads stay in guest byte order; trace command headers are little endian.
"""
import argparse
from collections import Counter
import json
import importlib.util
from pathlib import Path
import struct
import hashlib

_spec = importlib.util.spec_from_file_location('gpu_history', Path(__file__).with_name('import-gpu-history.py'))
_history = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_history)

OPCODES = {
    0x22: 'DRAW_INDX', 0x36: 'DRAW_INDX_2', 0x34: 'DRAW_INDX_BIN', 0x35: 'DRAW_INDX_2_BIN',
    0x23: 'VIZ_QUERY', 0x5b: 'EVENT_WRITE_ZPD', 0x3c: 'WAIT_REG_MEM', 0x64: 'XE_SWAP',
    0x46: 'EVENT_WRITE', 0x58: 'EVENT_WRITE_SHD', 0x5a: 'EVENT_WRITE_EXT',
}


def shader_exports(raw):
    """Conservatively inspect every ALU clause, including conditional paths."""
    if len(raw) % 12:
        raise ValueError('Shader microcode is not a whole number of instructions')
    instructions = list(struct.iter_unpack('>3I', raw))
    limit, cursor, exports, memory_allocations = len(instructions), 0, set(), 0
    while cursor < limit:
        a, b, c = instructions[cursor]
        for cf in (a | ((b & 65535) << 32), (b >> 16) | (c << 16)):
            opcode = cf >> 44
            if opcode == 12 and (cf >> 41) & 3 == 3:
                memory_allocations += 1
            if opcode not in (1, 2, 3, 4, 5, 6, 13, 14):
                continue
            address, count = cf & 4095, (cf >> 12) & 7
            if address + count > len(instructions):
                raise ValueError('Shader EXEC exceeds code')
            if address:
                limit = min(limit, address)
            for index in range(count):
                if (cf >> (16 + index * 2)) & 1:
                    continue  # Fetch, which cannot memory-export.
                word = instructions[address + index][0]
                if word & (1 << 15):
                    exports.add(word & 63)
        cursor += 1
    return dict(sha256=hashlib.sha256(raw).hexdigest(), bytes=len(raw),
                memory_allocations=memory_allocations,
                export_registers=sorted(exports),
                memory_export_registers=sorted(exports & set(range(32, 38))))


def audit(data):
    if len(data) < 48 or struct.unpack_from('<I', data)[0] != 1:
        raise ValueError('Unsupported trace header')
    counts, primitives, conditional_draws = Counter(), Counter(), 0
    draw_modes = Counter()
    point_states = Counter()
    registers = {}
    register_loads = {}
    register_memory = {}
    shader_loads, shader_memory, shaders = {}, {}, {}
    active_shaders = [None, None]
    point_shaders = Counter()
    point_queries = Counter()
    active_query = None
    at, commands, swaps, draws, zpd = 48, 0, [], [], []

    def words(offset, count):
        if offset + count * 4 > len(data):
            raise ValueError('Truncated trace command')
        return struct.unpack_from('<' + 'I' * count, data, offset)

    while at < len(data):
        kind, = words(at, 1)
        commands += 1
        if kind in (0, 2, 4):
            _, address, count = words(at, 3)
            end = at + 12 + count * 4
            if end > len(data):
                raise ValueError('Truncated packet payload')
            if kind == 4 and count:
                packet = struct.unpack_from('>' + 'I' * count, data, at + 12)
                packet_type = packet[0] >> 30
                if packet_type == 3:
                    opcode = (packet[0] >> 8) & 127
                    name = OPCODES.get(opcode, f'OPCODE_{opcode:02x}')
                    counts[name] += 1
                    if opcode in (0x22, 0x36, 0x34, 0x35):
                        # DRAW_INDX_2 has no explicit visibility-condition word.
                        index = 1 if opcode == 0x36 else 2
                        if len(packet) <= index:
                            raise ValueError('Truncated draw initiator')
                        initiator = packet[index]
                        primitives[str(initiator & 63)] += 1
                        conditional_draws += bool(opcode != 0x36 and packet[1] & 0x100)
                        mode = registers.get(0x2208, 0) & 7
                        export_mode = (registers.get(0x2180, 0) >> 24) & 7
                        draw_modes[f'primitive={initiator & 63},edram={mode},vs_export={export_mode}'] += 1
                        if initiator & 63 == 1:
                            point_shaders[tuple(active_shaders)] += 1
                            point_queries['active' if active_query is not None else 'inactive'] += 1
                            point_states[','.join(f'{name}={registers.get(reg, 0):08x}' for name, reg in (
                                ('color_mask', 0x2104), ('depth_control', 0x2200), ('scissor_tl', 0x2081),
                                ('scissor_br', 0x2082), ('point_size', 0x2280), ('surface', 0x2000)))] += 1
                        draws.append(dict(command=commands, primitive=initiator & 63,
                                          indices=initiator >> 16, opcode=name,
                                          edram_mode=mode, vertex_export_mode=export_mode))
                    elif opcode == 0x5b:
                        address = registers.get(0x2325, 0)
                        active_query = address if address & 32 else None
                        zpd.append(dict(command=commands, draws_before=len(draws), address=f'{address:08x}',
                                        role='begin' if address & 32 else 'end'))
                    elif opcode == 0x64:
                        swaps.append(dict(command=commands, draws_before=len(draws),
                                          api_frame=packet[5] if len(packet) >= 6 else None))
                    elif opcode == 0x27 and len(packet) >= 3:
                        address, stage, length = packet[1] & ~3, packet[1] & 3, (packet[2] & 65535)*4
                        if stage > 1:
                            raise ValueError('Unknown shader stage')
                        shader_loads[address] = (stage, length)
                        active_shaders[stage] = shader_memory.get((address, length))
                    elif opcode == 0x2b and len(packet) >= 3:
                        stage, length = packet[1], (packet[2] & 65535)*4
                        if stage > 1 or length > (len(packet)-3)*4:
                            raise ValueError('Invalid immediate shader')
                        shader = shader_exports(data[at+24:at+24+length])
                        shaders[shader['sha256']] = shader
                        active_shaders[stage] = shader['sha256']
                    elif opcode in (0x2d, 0x55, 0x56) and len(packet) >= 2:
                        # SET_CONSTANT, SET_CONSTANT2 and SET_SHADER_CONSTANTS.
                        offset = packet[1]
                        if opcode == 0x2d:
                            base = (offset & 2047) + 0x2000 if offset >> 16 == 4 else None
                        else:
                            base = offset & 65535
                        if base is not None:
                            for index, value in enumerate(packet[2:]): registers[base+index] = value
                    elif opcode == 0x2f and len(packet) >= 4 and (packet[2] >> 16) & 255 == 4:
                        address, base, length = packet[1] & 0x3fffffff, (packet[2] & 2047) + 0x2000, (packet[3] & 4095)*4
                        register_loads[address] = (base, length)
                        if (address, length) in register_memory:
                            raw = register_memory[address, length]
                            for index, value in enumerate(struct.unpack('>' + 'I'*(length//4), raw)): registers[base+index] = value
                else:
                    counts[f'TYPE_{packet_type}'] += 1
                    if packet_type == 0:
                        base, repeat = packet[0] & 0x7fff, bool(packet[0] & 0x8000)
                        for index, value in enumerate(packet[1:]): registers[base+(0 if repeat else index)] = value
                    elif packet_type == 1 and len(packet) >= 3:
                        registers[packet[0] & 2047] = packet[1]
                        registers[(packet[0] >> 11) & 2047] = packet[2]
            at = end
        elif kind in (1, 3, 5):
            at += 4
        elif kind in (6, 7):
            _, address, encoding, encoded, decoded = words(at, 5)
            if kind == 6 and address in shader_loads and decoded == shader_loads[address][1]:
                stage, length = shader_loads[address]
                shader = shader_exports(_history.decode(encoding, data[at+20:at+20+encoded], decoded))
                shaders[shader['sha256']] = shader
                active_shaders[stage] = shader['sha256']
                shader_memory[address, length] = shader['sha256']
            if kind == 6 and address in register_loads and decoded == register_loads[address][1]:
                base, length = register_loads[address]
                raw = _history.decode(encoding, data[at+20:at+20+encoded], decoded)
                register_memory[address, length] = raw
                for index, value in enumerate(struct.unpack('>' + 'I'*(length//4), raw)): registers[base+index] = value
            at += 20 + encoded
        elif kind == 8:
            _, _, encoded = words(at, 3)
            at += 12 + encoded
        elif kind == 9:
            _, event = words(at, 2)
            counts[f'TRACE_EVENT_{event}'] += 1
            at += 8
        elif kind == 10:
            _, first, count, _, encoding, encoded = words(at, 6)
            raw = _history.decode(encoding, data[at+24:at+24+encoded], count*4)
            for index, value in enumerate(struct.unpack('<' + 'I'*count, raw)): registers[first+index] = value
            at += 24 + encoded
        elif kind == 11:
            _, _, _, encoded = words(at, 4)
            at += 16 + encoded
        else:
            raise ValueError(f'Unknown trace command {kind} at {at}')
        if at > len(data):
            raise ValueError('Trace command exceeds file')
    return dict(version=1, command_count=commands, packets=dict(counts),
                primitive_counts=dict(primitives), draw_mode_counts=dict(draw_modes), conditional_draws=conditional_draws,
                point_draw_index_counts=dict(Counter(str(d['indices']) for d in draws if d['primitive'] == 1)),
                point_draw_states=dict(point_states),
                point_draw_shaders=[dict(count=count, vertex=shaders.get(vs), pixel=shaders.get(ps))
                                    for (vs, ps), count in point_shaders.items()],
                point_draw_occlusion_queries=dict(point_queries),
                swaps=swaps, depth_sample_events=zpd,
                safe_to_disable_reference_draws=False,
                limitations=['Counts do not replace occlusion results, shader memory exports, or CPU readbacks',
                             'Draw packets include reference clear and resolve operations',
                             'Absent operations in one trace do not establish whole-game coverage'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    report = audit(args.trace.read_bytes())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('packets', 'primitive_counts', 'conditional_draws', 'swaps')}))


if __name__ == '__main__':
    main()
