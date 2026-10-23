#!/usr/bin/env python3
"""Small offline corpus for the closed LLVM22 ELF identity reader; no compiler."""
import hashlib
from pathlib import Path
import struct
import tempfile
import zlib

import sdk_ir_pgo_elf as elf


def uleb(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def name_block(names, compressed):
    plain = b"\x01".join(name.encode() for name in names)
    packed = zlib.compress(plain) if compressed else plain
    return uleb(len(plain)) + uleb(len(packed) if compressed else 0) + packed


def record(name, counters=1, bitmap_bytes=0):
    reference = int.from_bytes(hashlib.md5(name.encode(), usedforsecurity=False).digest()[:8], "little")
    return struct.pack("<6QI2HI4x", reference, 0, 0, 0, 0, 0, counters, 0, 0, bitmap_bytes)


def container(names, data, counters):
    # Independent minimal ELF file with ordinary64-byte headers and three named
    # profile sections. Addresses/relocations are immaterial to NameRef identity.
    strings = b"\0.shstrtab\0__llvm_prf_names\0__llvm_prf_data\0__llvm_prf_cnts\0"
    payloads = [strings, names, data, counters]
    labels = [b".shstrtab", b"__llvm_prf_names", b"__llvm_prf_data", b"__llvm_prf_cnts"]
    body = bytearray()
    sections = [bytes(64)]
    offset = 64 + 5 * 64
    for index, (label, payload) in enumerate(zip(labels, payloads)):
        sections.append(struct.pack("<IIQQQQIIQQ", strings.index(label), 3 if index == 0 else 1,
                                    0, 0, offset, len(payload), 0, 0, 1, 0))
        body.extend(payload)
        offset += len(payload)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    return struct.pack("<16sHHIQQQIHHHHHH", ident, 3, 62, 1, 0, 0, 64, 0,
                       64, 0, 0, 64, 5, 1) + b"".join(sections) + body


def main():
    names = ["_ZN3rex6memory8BaseHeap16QueryRangeAccessEjj", "_ZN3rex6memory10RingBuffer4ReadEj"]
    good_names = name_block(names, True)
    good_data = record(names[0], 2) + record(names[1], 1)
    good = container(good_names, good_data, bytes(24))
    passed = 0
    with tempfile.TemporaryDirectory(prefix="sdk-pgo-elf-test-") as folder:
        path = Path(folder) / "fixture.elf"

        def check(data, expected):
            nonlocal passed
            path.write_bytes(data)
            if expected is None:
                try:
                    elf.profile_symbols(path)
                except RuntimeError:
                    passed += 1
                    return
                raise AssertionError("malformed identity unexpectedly accepted")
            assert elf.profile_symbols(path) == sorted(expected)
            passed += 1

        check(good, names)
        check(container(name_block(names, False), good_data, bytes(24)), names)
        check(container(name_block(names[:1], True) + name_block(names[1:], False), good_data, bytes(24)), names)
        check(container(good_names, record(names[0]) * 2, bytes(16)), names[:1])
        check(container(b"", b"", b""), [])
        for end in (0, 16, 63, 65, len(good) - 1):
            check(good[:end], None)
        for offset, value in ((4, 1), (5, 2), (18, 3), (58, 32), (60, 0), (62, 9)):
            bad = bytearray(good)
            bad[offset] = value
            check(bad, None)
        for block in (b"\x80", b"\x80" * 10, b"\x00\x00", b"\x01\x04x",
                      uleb(elf.MAX_SECTION_BYTES + 1) + b"\0", b"\x03\x03bad",
                      b"\x03\0a\0b", b"\x03\0a\x01\x01",
                      uleb(3) + uleb(len(zlib.compress(b"abcd"))) + zlib.compress(b"abcd")):
            check(container(block, good_data, bytes(24)), None)
        packed = zlib.compress(b"abc") + b"junk"
        check(container(uleb(3) + uleb(len(packed)) + packed, good_data, bytes(24)), None)
        for data, counters in ((good_data[:-1], bytes(24)), (good_data, bytes(23)),
                               (good_data, bytes(32)), (record(names[0], 0), bytes(8)),
                               (record(names[0], 1, 1), bytes(8)),
                               (record("unknown"), bytes(8)), (b"", bytes(8)),
                               (good_data, b"")):
            check(container(good_names, data, counters), None)
        check(container(b"", good_data, bytes(24)), None)
    print("PASS SDK LLVM22 ELF profile decoder: %d offline cases" % passed)


if __name__ == "__main__":
    main()
