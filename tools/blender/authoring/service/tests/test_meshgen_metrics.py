"""Bounded exported geometry, exact UV overlap, shared-frame guard and retries."""
import struct
import unittest
from unittest.mock import patch
import zlib
from copy import deepcopy
from meshgen_e4_fixture import arguments, glb, png
from luminumbra_author.contracts import Refusal
from luminumbra_author.meshgen_backend import bounded_json
from luminumbra_author.meshgen_metrics import (inspect_artifact, guard_decision, retry_decimation, uv_overlap, validate_png, area_weighted_samples, retry_finish_chain)


class PortableMetrics(unittest.TestCase):
    def setUp(self): self.args = arguments(); self.effective = self.args['resolved'].effective

    def refusal(self, rule, call):
        with self.assertRaises(Refusal) as caught: call()
        self.assertEqual(caught.exception.finding['rule_id'], rule)

    def mutate(self, change):
        data = self.args['artifact']; size = struct.unpack_from('<I', data, 12)[0]
        doc, blob = bounded_json(data[20:20+size]), bytearray(data[28+size:])
        change(doc, blob)
        return glb(doc, bytes(blob))

    def test_exported_accessor_counts_normals_uv_bounds_and_split_seam_bodies(self):
        observed = inspect_artifact(self.args['artifact'], self.effective)
        self.assertEqual((observed['triangles'], observed['vertices'], observed['bodies']), (12, 36, 1))
        self.assertEqual(observed['bounds_m'], [1, 1, 1])
        for key in ('has_normals', 'has_uv', 'watertight', 'budget_ok', 'uv_overlap_zero'): self.assertTrue(observed[key], key)

    def test_scene_selection_requires_an_unambiguous_valid_default(self):
        for count, default, rule in ((1, 'absent', None), (2, 'absent', 'scene.ambiguous'),
                                     (2, 0, None), (2, 1, None), (2, 2, 'glb.bounds'),
                                     (2, -1, 'glb.bounds'), (2, False, 'glb.bounds'),
                                     (2, None, 'glb.bounds')):
            with self.subTest(count=count, default=default):
                def change(doc, blob):
                    doc['scenes'] = [{} for _ in range(count)]
                    doc['scenes'][default if type(default) is int and 0 <= default < count else 0] = {'nodes': [0]}
                    if default == 'absent': doc.pop('scene')
                    else: doc['scene'] = default
                data = self.mutate(change)
                if rule:
                    self.refusal(rule, lambda: inspect_artifact(data, self.effective))
                else:
                    observed = inspect_artifact(data, self.effective)
                    self.assertEqual((observed['triangles'], observed['vertices']), (12, 36))

    def test_malformed_bounds_indices_attributes_expansion_and_encodings(self):
        cases = [
            ('glb.bounds', lambda d, b: d['accessors'][1].update(byteOffset=10000)),
            ('glb.encoding', lambda d, b: d['accessors'][0].update(sparse={})),
            ('glb.nonfinite', lambda d, b: struct.pack_into('<f', b, 432, float('nan'))),
            ('finish.gate_failed', lambda d, b: struct.pack_into('<3f', b, 432, 0, 0, 0)),
            ('glb.bounds', lambda d, b: d['meshes'][0]['primitives'][0]['attributes'].update(TEXCOORD_0=200)),
            ('glb.encoding', lambda d, b: d['nodes'][0].update(scale=[2, 2, 2])),
            ('glb.encoding', lambda d, b: d.update(extensionsUsed=['KHR_draco_mesh_compression'])),
            ('glb.encoding', lambda d, b: d.update(textures=[{}])),
        ]
        for rule, mutation in cases:
            with self.subTest(rule=rule): self.refusal(rule, lambda: inspect_artifact(self.mutate(mutation), self.effective))
        effective = deepcopy(self.effective); effective['limits']['glb_accessor_elements'] = 50
        self.refusal('glb.limit', lambda: inspect_artifact(self.args['artifact'], effective))

    def test_exact_uv_positive_area_overlap_shared_edges_and_bounded_adversarial_pairs(self):
        a = ((0, 0), (1, 0), (0, 1)); b = ((1, 0), (1, 1), (0, 1))
        self.assertTrue(uv_overlap([a, b]))
        self.assertFalse(uv_overlap([a, a]))
        self.assertFalse(uv_overlap([a, ((.2, .2), (.4, .2), (.2, .4))]))
        self.assertFalse(uv_overlap([((0, 0), (1, 0), (2, 0))]))
        # All X intervals intersect, Y intervals do not: finite broad-phase work.
        triangles = [((0, i*2), (1, i*2), (0, i*2+1)) for i in range(100)]
        self.assertIsNone(uv_overlap(triangles, pair_limit=20))

    def test_closed_shell_and_matched_open_sheet_guard_modes(self):
        config = self.effective['finish']['decimate']
        obs = self.args['observations']['guard']
        self.assertTrue(guard_decision(obs, config)['pass'])
        obs = {**obs, 'p10_after': .1}; self.assertFalse(guard_decision(obs, config)['pass'])
        obs = {'hit_rate_before': 0, 'hit_rate_after': 0, 'raw_component_ids': ['leaf'], 'components': [
            {'raw_id': 'leaf', 'matched_id': 'finished_leaf', 'raw_projected_xyz': [10, 1, 2], 'matched_projected_xyz': [9, 0, 1]}]}
        self.assertEqual(guard_decision(obs, config)['mode'], 'open_sheet')
        self.assertTrue(guard_decision(obs, config)['pass'])
        obs['components'][0]['matched_projected_xyz'] = [1, 100, 100]
        self.assertFalse(guard_decision(obs, config)['pass'])
        self.assertFalse(guard_decision({}, config)['pass'])

    def test_under_budget_guard_runs_retry_starts_same_baseline_restores_last_accepted(self):
        measured, collapsed, restored = [], [], []
        def measure(value): measured.append(value); return value
        baseline = {'triangles': 20, 'guard_pass': False}
        result, attempts = retry_decimation(baseline, 64, 3, lambda b, r: collapsed.append(b), measure, restored.append)
        self.assertIs(result, baseline); self.assertEqual(measured, [baseline]); self.assertFalse(collapsed)
        baseline = {'triangles': 100, 'guard_pass': True}
        results = iter([{'triangles': 70, 'guard_pass': True}, {'triangles': 50, 'guard_pass': False}])
        def collapse(b, r): collapsed.append((b, r)); return next(results)
        result, attempts = retry_decimation(baseline, 64, 3, collapse, measure, restored.append)
        self.assertTrue(all(b is baseline for b, r in collapsed))
        self.assertEqual(len(attempts), 2); self.assertEqual(restored, [result]); self.assertEqual(result['triangles'], 70)
        self.assertAlmostEqual(collapsed[1][1], .64*64/70)

    def test_preview_parser_bounded_chunks_and_dimensions(self):
        self.assertEqual(validate_png(png(), self.effective['limits']), (1, 1))
        self.refusal('worker.protocol', lambda: validate_png(png()[:-1], self.effective['limits']))
        self.refusal('worker.protocol', lambda: validate_png(png()+b'extra', self.effective['limits']))

    def test_preview_dimension_cap_checked_before_inflate_and_inclusive(self):
        def image(width, height):
            def chunk(kind, body):
                return struct.pack('>I', len(body))+kind+body+struct.pack('>I', zlib.crc32(kind+body)&0xffffffff)
            header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
            scanlines = (b'\0'+b'\x80'*(width*3))*height
            return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', header)
                    +chunk(b'IDAT', zlib.compress(scanlines))+chunk(b'IEND', b''))

        for cap in (self.effective['limits']['image_dimension'], 32):
            limits = {**self.effective['limits'], 'image_dimension': cap}
            for width, height in ((cap*2, 1), (1, cap*2)):
                with self.subTest(cap=cap, width=width, height=height):
                    data = image(width, height)
                    self.assertLessEqual(width*height, limits['image_pixels'])
                    with patch.object(zlib, 'decompressobj', side_effect=AssertionError('Unexpected decompression')):
                        self.refusal('worker.protocol', lambda: validate_png(data, limits))
            for width, height in ((cap, 1), (1, cap)):
                with self.subTest(cap=cap, width=width, height=height):
                    self.assertEqual(validate_png(image(width, height), limits), (width, height))

    def test_area_weighted_sampling_deterministic_in_common_frame_and_whole_chain_retry(self):
        points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        faces = [(0, 1, 2)]
        raw = area_weighted_samples(points, faces, count=20)
        self.assertEqual(raw, area_weighted_samples(points, faces, count=20))
        translated = area_weighted_samples([(x+10, y, z) for x, y, z in points], faces, count=20)
        self.assertTrue(all(abs(a[0]+10-b[0]) < 1e-12 for a, b in zip(raw, translated)))
        calls, measured = [], []
        def chain(raw, destructive_repairs): calls.append((raw, destructive_repairs)); return len(calls)
        def measure(raw, final): measured.append((raw, final)); return {'guard_pass': final == 2}
        result, reports = retry_finish_chain(points, chain, measure)
        self.assertEqual(result, 2); self.assertEqual(len(reports), 2)
        self.assertEqual([flag for raw, flag in calls], [True, False])
        self.assertTrue(all(raw is points for raw, final in measured))

    def test_uv_overlap_translated_thin_and_nearly_touching(self):
        self.refusal('finish.gate_failed', lambda: uv_overlap([((0, 0), (10**10000, 0), (0, 1))]))
        for offset in (0, 1e9, -1e9):
            with self.subTest(offset=offset):
                a = ((offset, offset), (offset+1, offset), (offset, offset+1))
                b = ((offset+1, offset), (offset+1, offset+1), (offset, offset+1))
                self.assertFalse(uv_overlap([a, a]))
                self.assertTrue(uv_overlap([a, b]))
                epsilon = 2**-20
                overlapping = tuple((x-epsilon, y-epsilon) for x, y in b)
                separated = tuple((x+epsilon, y+epsilon) for x, y in b)
                self.assertFalse(uv_overlap([a, overlapping]))
                self.assertTrue(uv_overlap([a, separated]))
        for height in (2**-100, 2**-500):
            a = ((0., 0.), (1., 0.), (0., height))
            self.assertFalse(uv_overlap([a, a]))
            b = ((0., height), (1., height), (0., 2*height))
            self.assertTrue(uv_overlap([a, b]))
