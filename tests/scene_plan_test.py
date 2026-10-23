"""Render-pass dependencies: partial resolves and physical storage aliases."""
import importlib.util
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('scene', ROOT / 'tools/rendering/build-scene-plan.py')
scene = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scene)


class ScenePlanTest(unittest.TestCase):
    def test_resolve_destination_swap_uses_guest_fetch_selector(self):
        for selector in range(8):
            with self.subTest(selector=selector), tempfile.TemporaryDirectory() as temp:
                path = Path(temp)
                fetch = [2 | (1 << 22), 0x1000 | 6, 15 | (15 << 13), selector << 1, 0, 1 << 9]
                self.history_capture(path, fetch)
                with patch.object(scene.mesh_export, 'fetch_requirements', return_value=(set(), set())):
                    plan = scene.build(path)
                self.assertEqual(plan['operations'][0]['copy_dest_swap'], selector != 0)

    def history_capture(self, path, resolve_fetch=None, draw_fetch=None):
        def payload(data):
            digest = hashlib.sha256(data).hexdigest()
            (path / f'{digest}.payload.bin').write_bytes(data)
            return digest
        fetch = resolve_fetch or [2 | (1 << 22), 0x1000 | 6, 15 | (15 << 13), 0, 0, 1 << 9]
        device = bytearray(0x3600)
        struct.pack_into('>6I', device, 0x480, *fetch)
        state = payload(device)
        surface = bytearray(64)
        struct.pack_into('>2I', surface, 24, 32, 0)
        struct.pack_into('>I', surface, 36, (15 << 18) | (15 << 3))
        binding = dict(kind=6, address='00002000', sha256=payload(surface))
        texture = bytearray(64)
        struct.pack_into('>6I', texture, 28, *fetch)
        parameter = dict(kind=100, address='00003000', sha256=payload(texture))
        args = ['00000000'] * 8
        resolve = dict(function='Resolve', phase=0, frame=10, args=args, device_state=state,
                       surfaces=[binding], pass_parameters=[parameter])
        present = dict(function='Present', phase=0, frame=10, args=[args[0], '00003000', *args[2:]], device_state=state)
        shader = b'fixture shader'; digest = hashlib.sha256(shader).hexdigest()
        (path / f'{digest}.vertex.bin').write_bytes(shader)
        if draw_fetch:
            struct.pack_into('>6I', device, 0x480, *draw_fetch)
        draw_state = payload(device)
        draw = dict(function='DrawPrimitive', phase=1, frame=11, args=args, device_state=state,
                    surfaces=[binding], vertex_container=digest)
        draw['device_state'] = draw_state
        events = [resolve, present, draw, dict(present, frame=11)]
        report = dict(version=6, footer=dict(reason='frames_complete'), events=events)
        (path / 'capture.json').write_text(json.dumps(report))
        return report

    def test_previous_frame_producer_survives_present(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp); self.history_capture(path)
            with patch.object(scene.mesh_export, 'fetch_requirements', return_value=(set(), {0})):
                plan = scene.build(path)
            self.assertEqual(plan['frames'], [10, 11])
            self.assertEqual(plan['issues'], [])
            texture = plan['operations'][2]['textures'][0]
            self.assertEqual(texture['source'], 'gpu_resolve')
            self.assertEqual(texture['producers'], [dict(event=0, rectangle=[0, 0, 16, 16], full=True)])

    def test_frame_gap_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp); report = self.history_capture(path)
            report['events'][2]['frame'] = 12
            (path / 'capture.json').write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, 'consecutive'):
                scene.build(path)

    def test_operation_after_present_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp); report = self.history_capture(path)
            report['events'][2]['frame'] = 10
            (path / 'capture.json').write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, 'after the frame Present'):
                scene.build(path)

    def texture(self):
        return dict(base='10000000', format=6, width=1280, height=720, pitch=1280,
                    tiled=True, endian=2, dimension=1)

    def test_physical_aliases(self):
        self.assertEqual(scene.physical_address(0xf3aa2000), 0x13aa3000)
        self.assertEqual(scene.physical_address(0xb3370000), 0x13370000)
        # Captured depth resource and shader fetch must identify the same bytes.
        a = scene.texture_identity([0x8a000002, 0xf3aa2097, 0x59e4ff, 0xd11, 0, 0x200], True)
        b = scene.texture_identity([0x8a000002, 0x13aa3097, 0x59e4ff, 0xd11, 0, 0x200])
        self.assertEqual(a, b)

    def test_partial_writes_keep_prior_producers(self):
        h, t = scene.ResolveHistory(), self.texture()
        h.write(t, [0, 0, 1280, 720], 3)
        h.write(t, [10, 20, 30, 40], 5)
        h.write(t, [80, 90, 100, 110], 8)
        self.assertEqual([w['event'] for w in h.read(t)], [3, 5, 8])
        h.write(t, [0, 0, 1280, 720], 9)
        self.assertEqual([w['event'] for w in h.read(t)], [9])

    def test_alias_overwrite_invalidates_old_interpretation(self):
        h, a = scene.ResolveHistory(), self.texture()
        h.write(a, [0, 0, 1280, 720], 1)
        b = dict(a, format=23)
        h.write(b, [0, 0, 1280, 720], 2)
        self.assertEqual(h.read(a), [])
        self.assertEqual([w['event'] for w in h.read(b)], [2])

    def test_offset_alias_overwrite_invalidates_old_interpretation(self):
        h = scene.ResolveHistory()
        a = dict(self.texture(), base='00001000', width=64, height=64, pitch=64, tiled=False)
        b = dict(a, base='00003000', height=32)
        h.write(a, [0, 0, 64, 64], 1)
        h.write(b, [4, 6, 12, 9], 2)
        self.assertEqual(h.read(a), [])
        self.assertEqual([w['event'] for w in h.read(b)], [2])

    def test_offset_alias_and_adjacent_asset_classification(self):
        gpu = scene.GPURegions()
        a = dict(self.texture(), base='00001000', width=64, height=64, pitch=64, tiled=False)
        gpu.add(a)
        self.assertTrue(gpu.overlaps(dict(a, base='00003000', height=32)))
        self.assertFalse(gpu.overlaps(dict(a, base='00005000', height=32)))
        # Allocation pitch padding is not evidence of a GPU write.
        sparse = dict(a, width=1, height=2, pitch=2048)
        gpu = scene.GPURegions(); gpu.add(sparse)
        self.assertFalse(gpu.overlaps(dict(a, base='00002000', width=1, height=1)))
        self.assertTrue(gpu.overlaps(dict(a, base='00003000', width=1, height=1)))

    def test_plan_classifies_offset_gpu_alias(self):
        resolve_fetch = [2 | (2 << 22), 0x1000 | 6, 63 | (63 << 13), 0, 0, 1 << 9]
        draw_fetch = [2 | (2 << 22), 0x3000 | 6, 63 | (31 << 13), 0, 0, 1 << 9]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp); self.history_capture(path, resolve_fetch, draw_fetch)
            with patch.object(scene.mesh_export, 'fetch_requirements', return_value=(set(), {0})):
                plan = scene.build(path)
            texture = plan['operations'][2]['textures'][0]
            self.assertEqual(texture['source'], 'gpu_alias_or_previous_frame')
            self.assertEqual(texture['identity']['base'], '00003000')

    def test_partial_resolve_does_not_claim_unwritten_gpu_bytes(self):
        a = dict(self.texture(), base='00001000', width=64, height=64, pitch=64, tiled=False)
        gpu = scene.GPURegions(); gpu.add(a, [0, 0, 64, 16])
        self.assertFalse(gpu.overlaps(dict(a, base='00002000', height=16)))
        self.assertTrue(gpu.overlaps(dict(a, base='00001000', height=16)))

    def test_storage_endian_packing_and_stacked_stride(self):
        t = dict(self.texture(), base='00001000', width=1, height=1, pitch=32, tiled=False, format=24)
        self.assertEqual(scene.storage_ranges(t), ((0x1002, 0x1004),))
        self.assertEqual(scene.storage_ranges(dict(t, endian=0)), ((0x1000, 0x1002),))
        self.assertEqual(scene.storage_ranges(dict(t, layers=2)), ((0x1002, 0x1004), (0x2002, 0x2004)))
        packed = dict(t, format=6, endian=0, packed=True)
        self.assertEqual(scene.storage_ranges(packed), ((0x1040, 0x1044),))

    def test_tiled_ranges_match_known_microtiles(self):
        t = dict(self.texture(), base='00001000', width=8, height=1, pitch=32, endian=0)
        self.assertEqual(scene.storage_ranges(t), ((0x1000, 0x1010), (0x1020, 0x1030)))
        t['height'] = 2
        self.assertEqual(scene.storage_ranges(t), ((0x1000, 0x1040),))

    def test_actual_scene(self):
        path = ROOT / 'reports/rendering/native-renderer/captures/campaign-09'
        if not (path / 'capture.json').exists():
            self.skipTest('User-owned gameplay capture is not distributed')
        plan = scene.build(path)
        self.assertEqual(plan['counts']['draws'], 2172)
        self.assertEqual(plan['counts']['resolve'], 38)
        self.assertEqual(plan['counts']['gpu_resolve'], 449)
        self.assertFalse(plan['executable'])
        self.assertEqual(plan['version'], 3)
        self.assertEqual(plan['frames'], [5899])
        present = plan['operations'][-1]
        self.assertEqual(present['frontbuffer']['base'], '1f5f8000')
        self.assertEqual(present['provenance'], 'prior_resolve_resource')
        self.assertEqual(plan['issues'], [])
        rectangle = next(o for o in plan['operations'] if o['event'] == 2068)
        self.assertEqual(rectangle['vertex_input'], 'affine_float4_rectangle')
        self.assertEqual(rectangle['args'][1:4], [8, 3, 16])
        first = next(o for o in plan['operations'] if o['type'] == 'draw')
        self.assertTrue(first['vertex_slots'])
        self.assertIn('destination_fetch', next(o for o in plan['operations'] if o['type'] == 'resolve'))
        for operation in plan['operations']:
            for texture in operation.get('textures', []):
                for producer in texture.get('producers', []):
                    self.assertLess(producer['event'], operation['event'])


if __name__ == '__main__':
    unittest.main()
