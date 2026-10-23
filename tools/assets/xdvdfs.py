"""Bounded, read-only Xbox disc filesystem reader (Python standard library).

Disc layout reference: the BSD-licensed ReXGlue/Xenia DiscImageDevice in
vendor/rexglue-sdk/src/filesystem/devices/disc_image_device.cpp. This reader
does not decrypt executables, modify the image, or require a platform binary.
"""
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import os
import re
import stat
import struct


SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = (0, 0xFB20, 0x20600, 0x2080000, 0xFD90000)
MAX_DIRECTORY = 32 * 1024 * 1024
MAX_ENTRIES = 100_000


class AssetError(ValueError):
    pass


def safe_path(name):
    """Require a relative path safe on both Windows and POSIX hosts."""
    if not isinstance(name, str) or not name or len(name) > 1024:
        raise AssetError("Invalid asset path")
    parts = name.split("/")
    for part in parts:
        if (not part or part in (".", "..") or len(part) > 255
                or part[-1] in " ." or re.search(r'[\\<>:"|?*\x00-\x1f\x7f]', part)
                or part.split(".")[0].upper() in
                {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
                 *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}):
            raise AssetError(f"Unsafe asset path: {name!r}")
    return PurePosixPath(name)


@dataclass(frozen=True)
class Entry:
    path: str
    offset: int
    size: int
    directory: bool


class DiscImage:
    def __init__(self, path):
        self.path = Path(path)
        self.file = self.path.open("rb")
        try:
            info = os.fstat(self.file.fileno())
            if not stat.S_ISREG(info.st_mode):
                raise AssetError("The ISO must be a regular file")
            self.signature = (info.st_size, info.st_mtime_ns)
            self.size = info.st_size
            for partition in PARTITIONS:
                offset = partition + 32 * SECTOR
                if offset + SECTOR <= self.size:
                    header = self.read(offset, SECTOR)
                    if header[:20] == MAGIC and header[-20:] == MAGIC:
                        self.partition = partition
                        sector, size = struct.unpack_from("<II", header, 20)
                        self.entries = self._read_tree(sector, size)
                        break
            else:
                raise AssetError("No Xbox disc filesystem found. Supply an uncompressed Xbox 360 ISO.")
        except BaseException:
            self.file.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.file.close()

    def unchanged(self):
        info = os.fstat(self.file.fileno())
        if (info.st_size, info.st_mtime_ns) != self.signature:
            raise AssetError("The ISO changed during import; no completed import was published")

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset > self.size or size > self.size - offset:
            raise AssetError("Disc entry extends beyond the ISO")
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise AssetError("Unexpected end of ISO")
        return data

    def chunks(self, offset, size, block_size=4 * 1024 * 1024):
        while size:
            count = min(size, block_size)
            yield self.read(offset, count)
            offset += count
            size -= count

    def _read_tree(self, root_sector, root_size):
        entries, paths, directories = [], set(), set()
        pending = [(root_sector, root_size, "", 0)]
        total_directory_bytes = 0
        while pending:
            sector, size, parent, depth = pending.pop()
            if depth > 64 or not 14 <= size <= MAX_DIRECTORY:
                raise AssetError("Invalid directory depth or size")
            if sector in directories:
                raise AssetError("Disc directory cycle or alias")
            directories.add(sector)
            total_directory_bytes += size
            if total_directory_bytes > 256 * 1024 * 1024:
                raise AssetError("Disc directory tables exceed the supported limit")
            data = self.read(self.partition + sector * SECTOR, size)
            nodes, visited, spans = [0], set(), []
            while nodes:
                offset = nodes.pop()
                if offset in visited:
                    raise AssetError("Disc directory node cycle or alias")
                visited.add(offset)
                if offset + 14 > len(data):
                    raise AssetError("Truncated disc directory entry")
                left, right, start, length, attributes, name_size = struct.unpack_from("<HHIIBB", data, offset)
                end = offset + 14 + name_size
                if not name_size or end > len(data):
                    raise AssetError("Truncated or empty disc filename")
                try:
                    name = data[offset + 14:end].decode("ascii")
                except UnicodeDecodeError as exc:
                    raise AssetError("Unsupported non-ASCII disc filename") from exc
                if "/" in name:
                    raise AssetError("Disc filename contains a path separator")
                path = f"{parent}/{name}" if parent else name
                safe_path(path)
                if path.casefold() in paths:
                    raise AssetError(f"Duplicate disc path: {path}")
                paths.add(path.casefold())
                directory = bool(attributes & 0x10)
                file_offset = self.partition + start * SECTOR
                if file_offset > self.size or length > self.size - file_offset:
                    raise AssetError(f"Disc entry extends beyond the ISO: {path}")
                entries.append(Entry(path, file_offset, length, directory))
                if len(entries) > MAX_ENTRIES:
                    raise AssetError("Disc contains too many entries")
                if directory and length:
                    pending.append((start, length, path, depth + 1))
                spans.append((offset, end))
                nodes.extend(node * 4 for node in (left, right) if node)
            spans.sort()
            if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
                raise AssetError("Overlapping directory nodes")
        return sorted(entries, key=lambda entry: entry.path)
