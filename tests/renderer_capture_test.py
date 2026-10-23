"""Validate recorded Gears ABI data and rejection of damaged captures."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("capture", ROOT / "tools/rendering/inspect-gears-capture.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def synthetic_capture(version=2, linked=True):
    """A linked shader plus a second event sharing the first event's bytes."""
    data = bytearray(b'G3D3D001' + struct.pack('<II', version, 0x3600))
    blocks = {1: bytes(0x3600),
              2: struct.pack('>9I', 0x102A1141 if linked else 0x102A1101, 36, 12, 0, 0, 0, 0, 0, 0),
              3: bytes(12), 14: bytes(range(16)), 32: b'base texture', 33: b'mip texture',
              96: b'final vertex bytes', 97: b'final indices', 98: bytes(16), 99: bytes(16),
              100: bytes(64), 101: bytes(8), 102: struct.pack('>dI', .5, 0)}
    positions = {}
    block_headers = {}
    for event in range(2):
        start = len(data)
        count = {1: 13, 2: 31, 3: 95, 4: 97, 5: 102, 6: 103}[version]
        function, phase = (0x82AF6B10, 0) if version == 6 else (0x82AF34A8, 0) if version == 5 else (0x82AEF9C0, 2) if version == 4 else (0x82AF0318, 1)
        data += struct.pack('<15I', 0, function, phase, 5, 0x1234, *([0] * 8), 0, count)
        for kind in range(1, count + 1):
            block = blocks.get(kind, b'')
            block_headers[event, kind] = len(data)
            if event and block and version >= 2:
                data += struct.pack('<III', kind, 0x2000, 0x80000000 | positions[kind])
            else:
                data += struct.pack('<III', kind, 0x1000, len(block))
                if block:
                    positions[kind] = len(data)
                data += block
        struct.pack_into('<I', data, start, len(data) - start)
    data += struct.pack('<4I', 16, 0, 0, 2)
    return data, positions, block_headers


class CaptureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = bytes(synthetic_capture(version=1)[0])

    def local_capture(self, name):
        path = ROOT / "reports/rendering/native-renderer/captures" / name
        if not path.is_file():
            self.skipTest("Optional local gameplay capture is not distributed")
        return path.read_bytes()

    def test_real_startup(self):
        report, shaders = capture.inspect(self.local_capture("startup-01.g3d"))
        self.assertEqual(report["footer"], {"reason": "frames_complete", "events": 21})
        self.assertEqual(report["counts"]["Present"], 3)
        self.assertEqual(len(shaders), 2)
        self.assertFalse(report["replayable"])
        for event in report["events"]:
            self.assertEqual(event["args"][0], "40005d80")

    def test_short_headers(self):
        for length in (0, 7, 15, 17, 20):
            with self.subTest(length=length), self.assertRaises(ValueError):
                capture.inspect(self.data[:length])

    def test_missing_footer(self):
        with self.assertRaisesRegex(ValueError, "footer missing"):
            capture.inspect(self.data[:-16])

    def test_footer_event_count(self):
        data = bytearray(self.data)
        struct.pack_into("<I", data, len(data) - 4, 0)
        with self.assertRaisesRegex(ValueError, "count/reason"):
            capture.inspect(data)

    def test_record_length_overflow(self):
        data = bytearray(self.data)
        struct.pack_into("<I", data, 16, 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "record length"):
            capture.inspect(data)

    def test_block_length_overflow(self):
        data = bytearray(self.data)
        struct.pack_into("<I", data, 16 + 60 + 8, 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "capture block"):
            capture.inspect(data)

    def test_unknown_abi(self):
        data = bytearray(self.data)
        struct.pack_into("<I", data, 8, 99)
        with self.assertRaisesRegex(ValueError, "Unknown capture"):
            capture.inspect(data)

    def test_trailing_data(self):
        with self.assertRaisesRegex(ValueError, "footer"):
            capture.inspect(self.data + b"unexpected")

    def test_linked_vertex_container(self):
        for version in (1, 2, 3, 4, 5, 6):
            data, _, _ = synthetic_capture(version)
            report, shaders = capture.inspect(data)
            self.assertEqual(len(shaders), 1)
            self.assertEqual(next(iter(shaders.values()))[0], 'vertex')
            self.assertEqual(report['footer']['events'], 2)

    def test_shader_stage_mismatch(self):
        data, positions, _ = synthetic_capture()
        struct.pack_into('>I', data, positions[2], 0x102A1100)
        with self.assertRaisesRegex(ValueError, 'Shader blocks'):
            capture.inspect(data)

    def test_memory_export_vertex_container(self):
        data, positions, _ = synthetic_capture(6)
        struct.pack_into('>I', data, positions[2], 0x102A1111)
        report, shaders = capture.inspect(data)
        self.assertEqual(report['footer'], dict(reason='frames_complete', events=2))
        self.assertEqual(len(shaders), 1)
        stage, blob = next(iter(shaders.values()))
        self.assertEqual(stage, 'vertex')
        self.assertEqual(struct.unpack_from('>I', blob)[0], 0x102A1111)
        self.assertFalse(report['replayable'])

    def test_memory_export_container_keeps_header_and_size_checks(self):
        # The bridge-known flag does not admit another stage, unknown flags,
        # a different signature, or disagreeing virtual/physical lengths.
        for word, value in ((0, 0x102A1110), (0, 0x102A1191), (0, 0x102B1111),
                            (4, 32), (4, 40), (8, 8), (8, 16)):
            with self.subTest(word=word, value=value):
                data, positions, _ = synthetic_capture(6)
                struct.pack_into('>I', data, positions[2], 0x102A1111)
                struct.pack_into('>I', data, positions[2] + word, value)
                with self.assertRaisesRegex(ValueError, 'Shader blocks'):
                    capture.inspect(data)

    def test_memory_export_flag_is_not_a_pixel_flag(self):
        data, positions, headers = synthetic_capture(6)
        struct.pack_into('>I', data, positions[2], 0x102A1111)
        # A valid backward reference to the vertex bytes must still be rejected
        # when placed in the pixel container, including its physical section.
        struct.pack_into('<I', data, headers[0, 4] + 8, 0x80000000 | positions[2])
        struct.pack_into('<I', data, headers[0, 5] + 8, 0x80000000 | positions[3])
        with self.assertRaisesRegex(ValueError, 'Shader blocks'):
            capture.inspect(data)

    def test_unknown_shader_flags(self):
        data, positions, _ = synthetic_capture()
        struct.pack_into('>I', data, positions[2], 0x102A1181)
        with self.assertRaisesRegex(ValueError, 'Shader blocks'):
            capture.inspect(data)

    def test_deduplicated_payloads(self):
        data, _, _ = synthetic_capture()
        payloads = {}
        report, _ = capture.inspect(data, payloads)
        self.assertEqual(report['version'], 2)
        self.assertEqual(report['block_references'], 4)
        first, second = [e['geometry'][0] for e in report['events']]
        self.assertEqual(first['sha256'], second['sha256'])
        self.assertNotEqual(first['address'], second['address'])
        self.assertEqual(first['fetch_slot'], 95)
        self.assertEqual(bytes(payloads[first['sha256']]), bytes(range(16)))

    def test_invalid_references(self):
        original, positions, headers = synthetic_capture()
        for address in (0, positions[14] + 1, len(original), 0x7FFFFFFF):
            data = bytearray(original)
            struct.pack_into('<I', data, headers[1, 14] + 8, 0x80000000 | address)
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, 'reference'):
                capture.inspect(data)

    def test_texture_snapshots(self):
        payloads = {}
        report, _ = capture.inspect(synthetic_capture(3)[0], payloads)
        textures = report['events'][0]['textures']
        self.assertEqual([t['region'] for t in textures], ['base', 'mips'])
        self.assertEqual(bytes(payloads[textures[0]['sha256']]), b'base texture')
        self.assertTrue(all(t['provenance'] == 'guest_cpu_memory' for t in textures))
        self.assertFalse(report['replayable'])

    def test_finalized_up_geometry(self):
        payloads = {}
        report, _ = capture.inspect(synthetic_capture(4)[0], payloads)
        self.assertEqual(report['counts']['BeginIndexedVertices_finalized'], 2)
        geometry = report['events'][0]['up_geometry']
        self.assertEqual([g['kind'] for g in geometry], [96, 97])
        self.assertEqual(bytes(payloads[geometry[0]['sha256']]), b'final vertex bytes')

    def test_invalid_finalized_phase(self):
        for version in (2, 3, 4):
            data = synthetic_capture(version)[0]
            struct.pack_into('<II', data, 20, 0x82AF0318, 2)
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, 'phase'):
                capture.inspect(data)

    def test_pass_parameters(self):
        payloads = {}
        report, _ = capture.inspect(synthetic_capture(5)[0], payloads)
        params = report['events'][0]['pass_parameters']
        self.assertEqual([p['kind'] for p in params], list(range(98, 103)))
        self.assertEqual(struct.unpack('>dI', payloads[params[-1]['sha256']]), (.5, 0))

    def test_invalid_pass_parameter_size(self):
        data, positions, headers = synthetic_capture(5)
        # Redirect the 16-byte rectangle to a valid but differently sized blob.
        struct.pack_into('<I', data, headers[1, 98] + 8, 0x80000000 | positions[100])
        with self.assertRaisesRegex(ValueError, 'parameter size'):
            capture.inspect(data)

    def test_real_finalized_capture(self):
        report, _ = capture.inspect(self.local_capture('campaign-08.g3d'))
        self.assertEqual(report['footer'], dict(reason='frames_complete', events=2284))
        for name in ('BeginVertices', 'BeginIndexedVertices'):
            self.assertEqual(report['counts'][name + '_after'], report['counts'][name + '_finalized'])
        for event in report['events']:
            if event['phase'] != 2:
                continue
            blocks = {g['kind']: g for g in event['up_geometry']}
            args = [int(a, 16) for a in event['args']]
            if event['function'] == 'BeginVertices':
                self.assertEqual(blocks[96]['bytes'], args[2] * args[3])
            else:
                self.assertEqual(blocks[96]['bytes'], args[3] * args[6])
                self.assertEqual(blocks[97]['bytes'], ((args[4] * (4 if args[5] & 4 else 2) + 3) // 4) * 4)

    def test_forward_reference(self):
        data, positions, headers = synthetic_capture()
        struct.pack_into('<I', data, headers[0, 1] + 8, 0x80000000 | positions[2])
        with self.assertRaisesRegex(ValueError, 'reference'):
            capture.inspect(data)

    def test_runtime_gameplay(self):
        data = self.local_capture('campaign-05.g3d')
        report, shaders = capture.inspect(data)
        self.assertEqual(report['footer'], dict(reason='frames_complete', events=2469))
        self.assertEqual(len(shaders), 313)
        self.assertEqual(sum(stage == 'vertex' for stage, _ in shaders.values()), 119)

    def test_real_geometry_capture(self):
        data = self.local_capture('campaign-06.g3d')
        payloads = {}
        report, shaders = capture.inspect(data, payloads)
        self.assertEqual(report['footer'], dict(reason='frames_complete', events=1686))
        self.assertEqual(report['block_references'], 17550)
        self.assertEqual(len(shaders), 276)
        self.assertEqual(report['counts']['DrawIndexedPrimitive_after'], 1133)
        for event in report['events']:
            if event['function'] not in ('DrawPrimitive', 'DrawIndexedPrimitive'):
                continue
            self.assertTrue(any('fetch_slot' in g for g in event['geometry']))
            if event['function'] == 'DrawIndexedPrimitive':
                blocks = {g['kind']: g for g in event['geometry']}
                flags = struct.unpack_from('>I', payloads[blocks[30]['sha256']])[0]
                width = 4 if flags & 0x80000000 else 2
                self.assertEqual(blocks[31]['bytes'], int(event['args'][3], 16) * width)


if __name__ == "__main__":
    unittest.main()
