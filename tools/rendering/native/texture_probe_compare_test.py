#!/usr/bin/env python3
"""CPU-only fixture checks for texture_probe_compare's evidence classification."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

BINARY = pathlib.Path(sys.argv.pop(1)).resolve()


class ProbeCompareTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gow3-probe-compare-")
        self.root = pathlib.Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def fixture(self, name, changes=(), address=0x1000, lod_bias=0):
        directory = self.root / name
        directory.mkdir()
        base = bytearray(4096)
        for offset, value in changes:
            base[offset] = value
        # Linear, endian-swap-8-in-16 BC1, 8x8 pixels, 32 texel pitch.
        # Consumed blocks begin at offsets 0, 8, 256, and 264.
        fetch = [2 | (1 << 22), address | 18 | (1 << 6), 7 | (7 << 13), 0, lod_bias, 1 << 9]
        metadata = dict(version=1, provenance="native_cpu_texture_after_gpu_overlay_before_decode",
                        source_base_payload="a" * 64, source_mip_payload=None,
                        fetch=fetch, frame=1, event=2, memory_version=3, overlaid_bytes=0)
        for key, data in (("base", base), ("mips", b"")):
            filename = f"texture-0-{key}.bin"
            (directory / filename).write_bytes(data)
            metadata[key] = dict(file=filename, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        (directory / "texture-0.json").write_text(json.dumps(metadata))
        return directory

    def run_compare(self, *directories, succeeds=True):
        output = self.root / "comparison.json"
        result = subprocess.run([str(BINARY), str(output), *map(str, directories)], text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, succeeds, result.stderr)
        if succeeds:
            return json.loads(output.read_text())
        self.assertFalse(output.exists(), "Invalid input must not create a completed report")

    def test_padding_only_and_address_normalization(self):
        report = self.run_compare(self.fixture("left"), self.fixture("right", [(1024, 255)], address=0x2000))
        self.assertEqual(len(report["groups"]), 1)
        comparison = report["groups"][0]["comparisons"][0]
        self.assertEqual(comparison["classification"], "padding_only")
        self.assertEqual(comparison["base_allocation_changed_bytes"], 1)
        self.assertEqual(comparison["consumed_level_changed_bytes"], 0)

    def test_consumed_block_endian_and_relaxed_sampler_match(self):
        report = self.run_compare(self.fixture("left"), self.fixture("right", [(0, 255)], lod_bias=0x10000))
        self.assertEqual(len(report["groups"]), 2)
        self.assertEqual(report["summary"]["comparisons"], 0)
        comparison = report["relaxed_latest_record_comparisons"][0]
        self.assertEqual(comparison["classification"], "consumed_blocks_differ")
        self.assertEqual(comparison["consumed_level_changed_bytes"], 1)
        self.assertEqual(comparison["consumed_changed_blocks"], 1)
        self.assertEqual(comparison["levels"][0]["first_changed_byte"], 1)

    def test_hash_mismatch_rejected(self):
        directory = self.fixture("bad")
        data = bytearray((directory / "texture-0-base.bin").read_bytes())
        data[0] = 1
        (directory / "texture-0-base.bin").write_bytes(data)
        self.run_compare(directory, succeeds=False)

    def test_extent_mismatch_rejected(self):
        directory = self.fixture("bad")
        (directory / "texture-0-base.bin").write_bytes(b"bad")
        self.run_compare(directory, succeeds=False)

    def test_output_not_overwritten(self):
        directory = self.fixture("valid")
        output = self.root / "comparison.json"
        output.write_text("sentinel")
        result = subprocess.run([str(BINARY), str(output), str(directory)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_text(), "sentinel")


if __name__ == "__main__":
    unittest.main()
