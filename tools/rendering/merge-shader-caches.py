#!/usr/bin/env python3
"""Merge this build's GOW3 shader records, validating hashes before any write.

Run only with the game closed. Sources and timestamped destination backups are
retained. These are discovered shaders, not a complete whole-game cache.
"""
import argparse
import ctypes
import ctypes.util
import datetime
import json
from pathlib import Path
import shutil
import struct

SHADER_HEADER = bytes.fromhex('5845534820201219')
PIPELINE_HEADER = bytes.fromhex('584550530100000020260228')
# Packed PipelineStoredDescription: 8-byte hash + 58-byte description.
PIPELINE_RECORD_SIZE = 66
TITLE = '4D5308AB'


def merge(sources, destination):
    library = ctypes.CDLL(ctypes.util.find_library('xxhash'))
    hash64 = library.XXH3_64bits
    hash64.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    hash64.restype = ctypes.c_uint64
    shaders, pipelines, report = {}, {}, []
    for directory in sources:
        counts = {'source': str(directory), 'shaders': 0, 'pipelines': 0}
        for suffix, header in [('xsh', SHADER_HEADER), ('fsi.vk.xpso', PIPELINE_HEADER)]:
            path = directory / f'{TITLE}.{suffix}'
            if not path.exists():
                continue
            data = path.read_bytes()
            if not data.startswith(header):
                raise ValueError(f'{path}: incompatible header')
            offset = len(header)
            while offset < len(data):
                start = offset
                if suffix == 'xsh':
                    if len(data) - offset < 12:
                        raise ValueError(f'{path}: truncated shader header at {offset}')
                    key, count_type = struct.unpack_from('<QI', data, offset)
                    payload_size = (count_type & 0x7fffffff) * 4
                    offset += 12
                    records, kind = shaders, 'shaders'
                else:
                    if len(data) - offset < PIPELINE_RECORD_SIZE:
                        raise ValueError(f'{path}: truncated pipeline at {offset}')
                    key = struct.unpack_from('<Q', data, offset)[0]
                    payload_size = PIPELINE_RECORD_SIZE - 8
                    offset += 8
                    records, kind = pipelines, 'pipelines'
                payload = data[offset:offset + payload_size]
                if len(payload) != payload_size or hash64(payload, len(payload)) != key:
                    raise ValueError(f'{path}: corrupt record at {start}')
                offset += payload_size
                record = data[start:offset]
                if key in records and records[key] != record:
                    raise ValueError(f'{path}: conflicting record hash {key:016X}')
                records[key] = record
                counts[kind] += 1
        report.append(counts)
    for record in pipelines.values():
        vs, _, ps = struct.unpack_from('<QQQ', record, 8)
        if vs not in shaders or (ps and ps not in shaders):
            raise ValueError('Pipeline references a missing shader; refusing incomplete merge')
    destination.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    for suffix, header, records in [('xsh', SHADER_HEADER, shaders),
                                     ('fsi.vk.xpso', PIPELINE_HEADER, pipelines)]:
        target = destination / f'{TITLE}.{suffix}'
        if target.exists():
            shutil.copy2(target, target.with_name(target.name + '.' + stamp + '.bak'))
        temporary = target.with_name(target.name + '.merge-tmp')
        temporary.write_bytes(header + b''.join(records[key] for key in sorted(records)))
        temporary.replace(target)
    result = {'sources': report, 'destination': str(destination),
              'unique_shaders': len(shaders), 'unique_pipelines': len(pipelines),
              'coverage': 'Only shaders/pipelines discovered in the supplied play sessions'}
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('sources', nargs='+', type=Path)
    args = parser.parse_args()
    result = merge(args.sources, args.destination)
    text = json.dumps(result, indent=2) + '\n'
    args.report.write_text(text)
    print(text, end='')
