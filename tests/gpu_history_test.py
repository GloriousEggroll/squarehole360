"""GPU history import must never consume later CPU/resolve trace records."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('history', Path(__file__).resolve().parents[1] / 'tools/rendering/import-gpu-history.py')
history = importlib.util.module_from_spec(spec)
spec.loader.exec_module(history)


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


class HistoryTest(unittest.TestCase):
    def test_diagnostic_state_only_requires_explicit_mode(self):
        header = words(1) + bytes(40) + words(0x4d5308ab)
        record = words(6, 4096, 0, 4, 4) + b'GPU!'
        with self.assertRaisesRegex(ValueError, 'no command stream'):
            history.initial_state(header + record)
        self.assertEqual(history.initial_state(header + record, state_only=True)[0], [(4096, b'GPU!')])
        with self.assertRaisesRegex(ValueError, 'contains commands'):
            history.initial_state(header + record + words(4, 0, 0), state_only=True)

    def test_reference_is_next_boundary_not_initial_history(self):
        metadata = dict(version=1, provenance='reference_gpu_frame_boundary', trace='test.xtr', frame=11)
        capture = dict(events=[dict(frame=10, function='DrawPrimitive'), dict(frame=10, function='Present')])
        history.validate_pair(metadata, capture, 'test.xtr', 10)
        for frame in (None, 9, 11):
            with self.subTest(frame=frame), self.assertRaises(ValueError):
                history.validate_pair(metadata, capture, 'test.xtr', frame)
        metadata['frame'] = 10
        history.validate_pair(metadata, capture, 'test.xtr')
        with self.assertRaises(ValueError):
            history.validate_pair(metadata, capture, 'test.xtr', 10)

    def test_snappy_literal_and_copy_modes(self):
        self.assertEqual(history.snappy(bytes([4, 12]) + b'abcd', 4), b'abcd')
        # Overlap: copy five bytes from distance one, using the two-byte tag.
        self.assertEqual(history.snappy(bytes([6, 0, 65, 18, 1, 0]), 6), b'AAAAAA')
        self.assertEqual(history.snappy(bytes([8, 12]) + b'abcd' + bytes([1, 4]), 8), b'abcdabcd')
        self.assertEqual(history.snappy(bytes([8, 12]) + b'abcd' + bytes([15, 4, 0, 0, 0]), 8), b'abcdabcd')
        self.assertEqual(history.snappy(bytes([61, 240, 60]) + b'x' * 61, 61), b'x' * 61)

    def test_snappy_bad_input(self):
        for data, size in [(b'', 0), (bytes([5, 0, 65]), 5), (bytes([4, 14, 0, 0]), 4),
                           (bytes([2, 0, 65, 2, 2, 0]), 2), (bytes([1, 0, 65]), 2),
                           (bytes([1, 4, 65, 66]), 1)]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                history.snappy(data, size)

    def test_initial_gpu_ranges_only(self):
        header = words(1) + bytes(40) + words(0x4d5308ab)
        first = words(6, 4096, 0, 4, 4) + b'GPU!'
        later = words(6, 4096, 0, 4, 4) + b'CPU!'
        ranges, edram, gamma = history.initial_state(header + first + words(4, 0, 0) + later)
        self.assertEqual(ranges, [(4096, b'GPU!')])
        self.assertIsNone(edram)
        self.assertIsNone(gamma)

    def test_register_and_gamma_layout(self):
        header = words(1) + bytes(40) + words(0x4d5308ab)
        registers = words(10, 0, 2, 0, 0, 8) + bytes(8)
        gamma = words(11, 2, 0, 2560) + bytes(range(256)) * 10
        result = history.initial_state(header + registers + gamma + words(0))
        self.assertEqual(result[2], bytes(range(256)) * 10)

    def test_rejects_unbounded_and_incomplete_ranges(self):
        header = words(1) + bytes(40) + words(0x4d5308ab)
        good = words(6, 4096, 0, 4, 4) + b'abcd'
        for body in [good, good + good + words(4), words(6, 512 * 1024 * 1024, 0, 4, 4) + b'abcd' + words(4),
                     words(6, 3, 0, 4, 4) + b'abcd' + words(4), words(6, 0, 0, 8, 8) + b'abcd',
                     words(6, 0, 0, 0, 0) + words(4)]:
            with self.subTest(body=body), self.assertRaises(ValueError):
                history.initial_state(header + body)


if __name__ == '__main__':
    unittest.main()
