"""Read the files in an Xbox 360 STFS content package (LIVE, PIRS or CON).

Used for title updates, which the game needs unpacked (the executable patch
for the recompiler and the game's update: files). Read-only: the package is
not modified and nothing outside the destination folder is written.
"""
import hashlib
import os
from pathlib import Path
import struct

from .xdvdfs import AssetError, safe_path

BLOCK = 0x1000
PER_TABLE = 170           # data blocks covered by one level-0 hash table
LEVEL_BLOCKS = (170, 170 * 170, 170 * 170 * 170)


class Package:
    def __init__(self, data):
        if data[:4] not in (b"LIVE", b"PIRS", b"CON "):
            raise AssetError("Not an Xbox 360 content package")
        self.data = data
        header_size = struct.unpack(">I", data[0x340:0x344])[0]
        self.content_type = "%08X" % struct.unpack(">I", data[0x344:0x348])[0]
        self.title_id = "%08X" % struct.unpack(">I", data[0x360:0x364])[0]
        if data[0x3A9:0x3AD] != b"\x00\x00\x00\x00":  # descriptor type 0 = STFS
            raise AssetError("Unsupported content package (not STFS)")
        descriptor = data[0x379:0x379 + 0x24]
        # Read-only packages keep one copy of each hash table, others two.
        self.tables = 1 if descriptor[2] & 1 else 2
        self.first = (header_size + BLOCK - 1) & ~(BLOCK - 1)
        self.table_blocks = struct.unpack("<H", descriptor[3:5])[0]
        self.table_start = int.from_bytes(descriptor[5:8], "little")
        self.entries = self._file_table()

    def _backing(self, block):
        """Package block holding data block `block` (hash tables sit between data)."""
        backing = block
        for base in LEVEL_BLOCKS:
            backing += ((block + base) // base) * self.tables
            if block < base:
                break
        return backing

    def _block(self, block):
        offset = self.first + self._backing(block) * BLOCK
        if offset + BLOCK > len(self.data) + BLOCK - 1:
            raise AssetError("Content package is truncated")
        return self.data[offset:offset + BLOCK]

    def _next(self, block):
        """The next block of a fragmented file, from the level-0 hash table."""
        group_first = (block // PER_TABLE) * PER_TABLE
        table = self.first + (self._backing(group_first) - self.tables) * BLOCK
        entry = self.data[table + (block % PER_TABLE) * 0x18:table + (block % PER_TABLE + 1) * 0x18]
        return int.from_bytes(entry[0x15:0x18], "big")

    def _blocks(self, start, count, contiguous):
        block = start
        for _ in range(count):
            yield block
            block = block + 1 if contiguous else self._next(block)

    def _file_table(self):
        entries = []
        for block in self._blocks(self.table_start, self.table_blocks, False):
            raw = self._block(block)
            for k in range(BLOCK // 0x40):
                e = raw[k * 0x40:(k + 1) * 0x40]
                length = e[0x28] & 0x3F
                if not length:
                    continue
                entries.append(dict(
                    name=e[:length].decode("latin-1"),
                    directory=bool(e[0x28] & 0x80),
                    contiguous=bool(e[0x28] & 0x40),
                    blocks=int.from_bytes(e[0x29:0x2C], "little"),
                    start=int.from_bytes(e[0x2F:0x32], "little"),
                    parent=struct.unpack(">h", e[0x32:0x34])[0],
                    size=struct.unpack(">I", e[0x34:0x38])[0]))
        return entries

    def path(self, index, depth=0):
        entry = self.entries[index]
        if entry["parent"] == -1:
            return entry["name"]
        if depth > 64 or not 0 <= entry["parent"] < len(self.entries):
            raise AssetError("Content package has a broken folder table")
        return self.path(entry["parent"], depth + 1) + "/" + entry["name"]

    def files(self):
        """(relative path, size) of every file."""
        return [(self.path(i), e["size"]) for i, e in enumerate(self.entries) if not e["directory"]]

    def read(self, index):
        entry = self.entries[index]
        out = bytearray()
        for block in self._blocks(entry["start"], entry["blocks"], entry["contiguous"]):
            out += self._block(block)
        return bytes(out[:entry["size"]])

    def extract(self, destination):
        """Writes every file under destination; returns {path: sha256}."""
        destination = Path(destination)
        digests = {}
        for i, entry in enumerate(self.entries):
            if entry["directory"]:
                continue
            relative = self.path(i)
            safe_path(relative)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            data = self.read(i)
            temporary = target.with_name(target.name + ".part")
            temporary.write_bytes(data)
            os.replace(temporary, target)
            digests[relative] = hashlib.sha256(data).hexdigest()
        return digests
