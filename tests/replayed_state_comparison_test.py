"""Known diagnostic storage encodings and strict unknown-byte rejection."""
import importlib.util
from pathlib import Path
import struct
import unittest

import numpy as np

spec = importlib.util.spec_from_file_location('comparison', Path(__file__).resolve().parents[1] / 'tools/rendering/compare-replayed-state.py')
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class ComparisonTest(unittest.TestCase):
    def decode(self, blob, fmt=6, endian=0, fetch=None):
        texture = dict(base='00001000', format=fmt, width=1, height=1, pitch=32,
                       tiled=False, endian=endian, dimension=1)
        return comparison.decode([(4096, blob)], texture, fetch or [0] * 6)[0, 0]

    def test_rgba8_endian_modes(self):
        channels = (17, 39, 123, 255)
        for endian, swap in enumerate((0, 1, 3, 2)):
            with self.subTest(endian=endian):
                value = self.decode(bytes(channels[i ^ swap] for i in range(4)), endian=endian)
                np.testing.assert_allclose(value, np.array(channels) / 255, atol=1e-7)

    def test_packed10_components(self):
        blob = struct.pack('<I', 17 | (511 << 10) | (1023 << 20) | (2 << 30))
        np.testing.assert_allclose(self.decode(blob, 7), [17 / 1023, 511 / 1023, 1, 2 / 3], atol=1e-7)

    def test_tiled_microblock_address(self):
        texture = dict(base='00001000', format=6, width=16, height=4, pitch=32,
                       tiled=True, endian=0, dimension=1)
        blob = bytearray(4096)
        blob[340:344] = bytes([17, 39, 123, 255])
        image = comparison.decode([(4096, blob)], texture, [0] * 6)
        np.testing.assert_allclose(image[3, 9], [17 / 255, 39 / 255, 123 / 255, 1], atol=1e-7)

    def test_mixed_signed16_and_half(self):
        value = self.decode(struct.pack('<4H', 0x8000, 0xffff, 0x7fff, 0), 26,
                            fetch=[(1 | (1 << 4)) << 2, 0, 0, 0, 0, 0])
        np.testing.assert_equal(value, [-1, 1, 1, 0])
        np.testing.assert_equal(self.decode(struct.pack('<4e', -2, .5, 3, 1), 32), [-2, .5, 3, 1])
        np.testing.assert_equal(self.decode(struct.pack('<2e', -2, .5), 31), [-2, .5, 0, 1])

    def test_missing_bytes_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'missing sampled'):
            self.decode(b'\0\0\0')


if __name__ == '__main__':
    unittest.main()
