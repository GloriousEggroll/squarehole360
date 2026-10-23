"""Bounded ELF64-LE LLVM22 IR instrumentation identity reader.

Clang22 may discard private __profc_ symbol names at object emission/linking.
Read the actual profile data records, resolving their NameRef through LLVM's
ULEB128/zlib name blocks instead. This is a deliberately closed Linux x86-64
qualification helper, not a general ELF or future LLVM profile parser.

The caller pins the compiler/profile runtime to LLVM22. Its installed
profile/InstrProfData.inc defines the 64-byte per-function record on this ABI:
six uint64 fields, uint32 NumCounters, two uint16 value-site counts, uint32
NumBitmapBytes, then four padding bytes. NameRef is the low64 MD5 name hash.
"""
import hashlib
from pathlib import Path
import os
import stat
import struct
import zlib


MAX_SECTIONS = 65535
MAX_SECTION_BYTES = 64 * 1024 * 1024
MAX_TOTAL_PROFILE_BYTES = 128 * 1024 * 1024
MAX_NAMES = 1000000
MAX_NAME_BYTES = 16384
PROFILE_SECTIONS = ("__llvm_prf_names", "__llvm_prf_data", "__llvm_prf_cnts")


def _need(value, reason):
    if not value:
        raise RuntimeError("LLVM22 ELF profile identity: " + reason)


def _uleb(data, offset):
    value = 0
    for index in range(10):
        _need(offset < len(data), "truncated ULEB128")
        byte = data[offset]
        offset += 1
        _need(index != 9 or byte <= 1, "ULEB128 overflow")
        value |= (byte & 127) << (index * 7)
        if not byte & 128:
            return value, offset
    raise RuntimeError("LLVM22 ELF profile identity: unterminated ULEB128")


def _names(blocks):
    names = {}
    decoded_bytes = 0
    for data in blocks:
        offset = 0
        while offset < len(data):
            unpacked, offset = _uleb(data, offset)
            packed, offset = _uleb(data, offset)
            _need(0 < unpacked <= MAX_SECTION_BYTES, "unsupported name-block size")
            size = packed or unpacked
            _need(size <= MAX_SECTION_BYTES and size <= len(data) - offset,
                  "truncated or oversized name block")
            encoded = data[offset:offset + size]
            offset += size
            decoded_bytes += unpacked
            _need(decoded_bytes <= MAX_TOTAL_PROFILE_BYTES, "decoded name byte bound")
            if packed:
                inflater = zlib.decompressobj()
                try:
                    decoded = inflater.decompress(encoded, unpacked + 1)
                except zlib.error as error:
                    raise RuntimeError("LLVM22 ELF profile identity: invalid compressed names") from error
                _need(len(decoded) == unpacked and inflater.eof and not inflater.unused_data
                      and not inflater.unconsumed_tail, "compressed name size/stream mismatch")
            else:
                decoded = encoded
            for raw in decoded.split(b"\x01"):
                _need(0 < len(raw) <= MAX_NAME_BYTES and b"\0" not in raw, "invalid function name")
                try:
                    name = raw.decode("utf-8")
                except UnicodeDecodeError as error:
                    raise RuntimeError("LLVM22 ELF profile identity: non-UTF8 function name") from error
                reference = int.from_bytes(hashlib.md5(raw, usedforsecurity=False).digest()[:8], "little")
                _need(reference not in names or names[reference] == name, "ambiguous NameRef collision")
                names[reference] = name
                _need(len(names) <= MAX_NAMES, "function name count bound")
    return names


def _read_sections(path):
    with Path(path).open("rb") as stream:
        before = os.fstat(stream.fileno())
        _need(stat.S_ISREG(before.st_mode), "input is not a regular file")
        length = before.st_size

        def read_at(offset, size):
            _need(0 <= offset <= length and 0 <= size <= length - offset, "ELF extent outside file")
            _need(size <= MAX_SECTION_BYTES, "ELF read bound")
            stream.seek(offset)
            data = stream.read(size)
            _need(len(data) == size, "short ELF read")
            return data

        header = read_at(0, 64)
        _need(header[:7] == b"\x7fELF\x02\x01\x01", "requires ELF64 little-endian version1")
        fields = struct.unpack("<16sHHIQQQIHHHHHH", header)
        _, kind, machine, version, _, _, section_offset, _, header_size, _, _, entry_size, count, strings = fields
        _need(kind in (1, 2, 3) and machine == 62 and version == 1, "requires x86-64 REL/EXEC/DYN ELF")
        _need(header_size == 64 and entry_size == 64 and 0 < count < MAX_SECTIONS,
              "unsupported ELF section layout or extended section numbering")
        _need(0 < strings < count, "invalid section-name table index")
        table = read_at(section_offset, count * entry_size)
        sections = [struct.unpack_from("<IIQQQQIIQQ", table, index * entry_size)
                    for index in range(count)]
        string_section = sections[strings]
        _need(string_section[1] == 3, "section-name table is not STRTAB")
        text = read_at(string_section[4], string_section[5])
        result = {name: [] for name in PROFILE_SECTIONS}
        total = 0
        for section in sections:
            name_offset, section_type, _, _, offset, size, _, _, alignment, _ = section
            _need(name_offset < len(text), "section name offset outside STRTAB")
            end = text.find(b"\0", name_offset)
            _need(end >= 0, "unterminated ELF section name")
            raw_name = text[name_offset:end]
            if not raw_name.startswith(b"__llvm_prf_"):
                continue
            name = raw_name.decode("ascii", errors="strict")
            if name == "__llvm_prf_bits":
                _need(not size, "bitmap/MC-DC instrumentation is outside qualified scope")
            if name not in result:
                # Value-profile auxiliary sections do not identify instrumented
                # functions; NameRef-bearing __llvm_prf_data is authoritative.
                continue
            _need(section_type == 1, "profile section is not PROGBITS")
            _need(alignment in (0, 1, 2, 4, 8, 16, 32, 64), "unsupported profile alignment")
            total += size
            _need(total <= MAX_TOTAL_PROFILE_BYTES, "profile section byte bound")
            result[name].append(read_at(offset, size))
        after = os.fstat(stream.fileno())
        signature = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
        _need(signature(before) == signature(after), "ELF changed while reading")
        return result


def profile_symbols(path, nm=None):
    """Return sorted names with actual LLVM22 data/counter records.

    nm is retained only for compatibility with the original census call sites;
    no subprocess or compilation occurs. Empty profile sections mean no IR
    counters. Partial, unresolved or unsupported instrumentation raises.
    """
    del nm
    sections = _read_sections(path)
    names_bytes = sum(map(len, sections["__llvm_prf_names"]))
    data_bytes = sum(map(len, sections["__llvm_prf_data"]))
    counter_bytes = sum(map(len, sections["__llvm_prf_cnts"]))
    if not (names_bytes or data_bytes or counter_bytes):
        return []
    _need(names_bytes and data_bytes and counter_bytes, "incomplete name/data/counter sections")
    _need(all(len(data) % 64 == 0 for data in sections["__llvm_prf_data"]),
          "unsupported LLVM per-function data record size")
    _need(all(len(data) % 8 == 0 for data in sections["__llvm_prf_cnts"]),
          "unsupported counter width or padding")
    names = _names(sections["__llvm_prf_names"])
    observed = set()
    counters = 0
    for data in sections["__llvm_prf_data"]:
        for offset in range(0, len(data), 64):
            reference = struct.unpack_from("<Q", data, offset)[0]
            count = struct.unpack_from("<I", data, offset + 48)[0]
            bitmap_bytes = struct.unpack_from("<I", data, offset + 56)[0]
            _need(reference in names, "data NameRef is absent from name blocks")
            _need(0 < count <= MAX_SECTION_BYTES // 8, "invalid per-function counter count")
            _need(bitmap_bytes == 0, "bitmap instrumentation is outside qualified scope")
            counters += count
            observed.add(names[reference])
    _need(counters * 8 == counter_bytes, "data/counter section totals differ")
    return sorted(observed)
