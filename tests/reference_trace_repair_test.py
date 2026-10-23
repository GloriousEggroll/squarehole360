import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('trace_repair', ROOT / 'tools/rendering/repair-reference-trace.py')
repair = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repair)


class RepairTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.trace = self.root / 'input.xtr'
        self.output = self.root / 'output.xtr'
        self.capture = self.root / 'capture'
        self.capture.mkdir()
        self.captured = {'events': [{'frame': 12, 'function': 'Present'}]}
        (self.capture / 'capture.json').write_text(json.dumps(self.captured))
        self.metadata = dict(version=1, frame=12, provenance='reference_gpu_frame_boundary', trace='input.xtr')
        Path(str(self.trace) + '.initial.json').write_text(json.dumps(self.metadata))
        header = struct.pack('<I40sI', 1, b'0' * 40, 0x4d5308ab)
        memory = struct.pack('<5I', 6, 0x1000, 0, 4, 4) + b'KEEP'
        packet = struct.pack('>6I', 0xc0046400, 0, 0x2000, 1, 1, 12)
        self.original = header + memory + struct.pack('<3I', 4, 0x3000, 6) + packet + struct.pack('<I', 5)
        self.trace.write_bytes(self.original)
        self.plan = dict(issues=[], operations=[])
        self.add_texture(b'KEEPdata')

    def add_texture(self, blob):
        digest = hashlib.sha256(blob).hexdigest()
        (self.capture / (digest + '.payload.bin')).write_bytes(blob)
        self.plan['operations'].append(dict(type='draw', event=len(self.plan['operations']), frame=12,
            textures=[dict(source='cpu_snapshot', slot=0, fetch=[0, 0x1000, 0, 0, 0, 0],
                           base_payload=digest, mip_payload=None)]))
        return digest

    def run_repair(self):
        with patch.object(repair.scene_plan, 'build', return_value=self.plan):
            return repair.repair(self.trace, self.capture, self.output)

    def test_only_absent_bytes_and_original_trace_preserved(self):
        report = self.run_repair()
        self.assertEqual(report['injected_bytes'], 4)
        self.assertEqual(report['injected_ranges'][0]['address'], 0x1004)
        self.assertEqual(report['injected_ranges'][0]['sha256'], hashlib.sha256(b'data').hexdigest())
        at, _, _, _ = repair.trace_layout(self.original)
        output = self.output.read_bytes()
        self.assertEqual(output[:at], self.original[:at])
        self.assertEqual(output[at + 24:], self.original[at:])
        self.assertEqual(report['original_trace_sha256'], hashlib.sha256(self.original).hexdigest())

    def test_repaired_trace_cannot_masquerade_as_initial_gpu_history(self):
        self.run_repair()
        metadata = json.loads(Path(str(self.output) + '.initial.json').read_text())
        with self.assertRaises(ValueError):
            repair.history.validate_pair(metadata, self.captured, self.output.name)

    def test_conflicting_missing_bytes_rejected(self):
        self.add_texture(b'KEEPelse')
        with self.assertRaisesRegex(ValueError, 'Conflicting CPU snapshots'):
            self.run_repair()
        self.assertFalse(self.output.exists())

    def test_gpu_produced_overlap_rejected(self):
        texture = dict(base='00001004', format=6, width=1, height=1, pitch=32,
                       tiled=False, endian=0, dimension=1, layers=1, packed=False)
        self.plan['operations'].append(dict(type='resolve', frame=12, destination=texture,
                                           destination_rectangle=[0, 0, 1, 1]))
        with self.assertRaisesRegex(ValueError, 'GPU-produced'):
            self.run_repair()
        self.assertFalse(self.output.exists())

    def test_frame_mismatch_rejected(self):
        self.metadata['frame'] = 13
        Path(str(self.trace) + '.initial.json').write_text(json.dumps(self.metadata))
        with self.assertRaises(ValueError):
            self.run_repair()

    def test_corrupt_payload_rejected(self):
        digest = self.plan['operations'][0]['textures'][0]['base_payload']
        (self.capture / (digest + '.payload.bin')).write_bytes(b'corrupt!')
        with self.assertRaisesRegex(ValueError, 'Invalid CPU asset'):
            self.run_repair()

    def test_existing_output_rejected(self):
        self.output.write_bytes(b'user file')
        with self.assertRaisesRegex(ValueError, 'must be new'):
            self.run_repair()
        self.assertEqual(self.output.read_bytes(), b'user file')

    def test_truncated_trace_rejected(self):
        for count in (1, 20, len(self.original) - 1):
            with self.assertRaises(ValueError):
                repair.trace_layout(self.original[:count])


if __name__ == '__main__':
    unittest.main()
