"""CPU-only depth contract checks; fixtures are never visual acceptance evidence."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

import inspect_terrain_aovs as aovs


def projection(reversed_depth):
    near, far = 0.1, 3200.0
    matrix = np.zeros((4, 4))
    matrix[0, 0] = matrix[1, 1] = 1
    matrix[3, 2] = -1
    if reversed_depth:
        matrix[2, 2], matrix[2, 3] = near / (far - near), near * far / (far - near)
    else:
        matrix[2, 2], matrix[2, 3] = -(far + near) / (far - near), -2 * near * far / (far - near)
    return matrix.flatten(order='F').tolist()


class TerrainAovDepth(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def packet(self, name='packet', depths=(0, 0.00000001, 0.5, 1), reversed_depth=True, endian='<'):
        root = self.root / name
        root.mkdir()
        metadata = dict(schema='luminumbra.terrain_coverage_capture.v1', complete=True, enabled=True,
                        matrix_layout='column_major', render_frame=641, time_of_day=0.04,
                        capture_phase='same_rendered_frame_after_postprocessing_before_present_unmeasured',
                        color_dimensions=[2, 2], gbuffer_dimensions=[2, 2],
                        gbuffer_projection=projection(reversed_depth), view=np.eye(4).flatten().tolist(),
                        taau_enabled=False, jitter_ndc=[0.0, 0.0],
                        far=dict(wanted=1, resident_wanted=1, missing_after_eviction=0))
        metadata['attachments'] = {name: 'test fixture' for name in
            ('color.ppm', 'depth.pfm', 'position.pfm', 'normal.pfm', 'albedo.pfm', 'material.pgm')}
        metadata['attachments']['depth.pfm'] = aovs.DEPTH_DESCRIPTION if reversed_depth else aovs.LEGACY_DEPTH_DESCRIPTION
        if reversed_depth:
            metadata['depth_convention'] = dict(schema='luminumbra.depth_convention.v1', direction='reversed',
                clip_depth_range='zero_to_one', clear_value=0.0, near_plane_depth=1.0,
                far_plane_depth=0.0, near_plane_m=0.1, far_plane_m=3200.0,
                storage_format='GL_DEPTH_COMPONENT32F')
        (root / 'manifest.json').write_text(json.dumps(metadata))
        (root / 'color.ppm').write_bytes(b'P6\n2 2\n255\n' + bytes(range(12)))
        (root / 'material.pgm').write_bytes(b'P5\n2 2\n255\n' + bytes([255, 1, 1, 1]))
        for name, channels, values in [('depth.pfm', 1, depths), ('position.pfm', 3, [0] * 12),
                                       ('normal.pfm', 3, [0, 0, 1] * 4), ('albedo.pfm', 3, [0.5] * 12)]:
            magic = b'Pf' if channels == 1 else b'PF'
            scale = b'-1.0' if endian == '<' else b'1.0'
            (root / name).write_bytes(magic + b'\n2 2\n' + scale + b'\n' + np.array(values, dtype=endian + 'f4').tobytes())
        return root, metadata

    def test_reversed_clear_zero_and_near_one(self):
        root, _ = self.packet()
        before = {p.name: p.read_bytes() for p in root.iterdir()}
        _, arrays, report = aovs.inspect(root)
        self.assertEqual(report['covered_pixels'], 3)
        self.assertEqual(report['clear_depth_pixels'], 1)
        self.assertEqual(arrays['depth.pfm'][1, 0, 0], 0)
        self.assertEqual(before, {p.name: p.read_bytes() for p in root.iterdir()})

    def test_legacy_requires_opt_in_and_keeps_forward_depth(self):
        root, _ = self.packet(depths=(1, 0.999, 0.5, 0), reversed_depth=False)
        with self.assertRaisesRegex(ValueError, 'historical packets require'):
            aovs.inspect(root)
        report = aovs.inspect(root, legacy_forward=True)[2]
        self.assertEqual(report['covered_pixels'], 3)
        self.assertTrue(report['depth_convention']['legacy_explicit'])

    def test_projection_and_metadata_mismatch_refused(self):
        _, metadata = self.packet()
        for modify in ('projection', 'clear', 'storage', 'description', 'planes'):
            bad = copy.deepcopy(metadata)
            if modify == 'projection':
                bad['gbuffer_projection'] = projection(False)
            elif modify == 'clear':
                bad['depth_convention']['clear_value'] = 1
            elif modify == 'storage':
                bad['depth_convention']['storage_format'] = 'GL_DEPTH_COMPONENT24'
            elif modify == 'description':
                bad['attachments']['depth.pfm'] = aovs.LEGACY_DEPTH_DESCRIPTION
            else:
                bad['depth_convention']['near_plane_m'] = True
            with self.subTest(modify=modify), self.assertRaises(ValueError):
                aovs.depth_convention(bad)

    def test_legacy_flag_cannot_reinterpret_reversed_projection(self):
        _, metadata = self.packet(reversed_depth=False)
        metadata['gbuffer_projection'] = projection(True)
        with self.assertRaisesRegex(ValueError, 'Projection endpoints'):
            aovs.depth_convention(metadata, legacy_forward=True)

    def test_bad_payloads_refused(self):
        for index, depths in enumerate(((float('nan'), 0, 0, 0), (-0.1, 0, 0, 0), (1.1, 0, 0, 0))):
            root, _ = self.packet(str(index), depths=depths)
            with self.assertRaises(ValueError):
                aovs.inspect(root)
        for name, transform in [('short', lambda b: b[:-1]), ('trailing', lambda b: b + b'x')]:
            root, _ = self.packet(name)
            path = root / 'depth.pfm'
            path.write_bytes(transform(path.read_bytes()))
            with self.assertRaisesRegex(ValueError, 'Truncated or trailing'):
                aovs.inspect(root)

    def test_pfm_endian_and_row_order_preserved(self):
        little, _ = self.packet('little', endian='<')
        big, _ = self.packet('big', endian='>')
        a, b = aovs.inspect(little), aovs.inspect(big)
        np.testing.assert_array_equal(a[1]['depth.pfm'], b[1]['depth.pfm'])
        self.assertEqual(a[2]['coverage_fraction'], b[2]['coverage_fraction'])

    def test_comparison_counts_clear_transitions_and_rejects_mixed_contracts(self):
        first, _ = self.packet('first', depths=(0, 0.8, 0, 0))
        second, _ = self.packet('second', depths=(0.2, 0.9, 0, 0))
        report = aovs.compare(aovs.inspect(first), aovs.inspect(second))
        self.assertEqual(report['newly_covered_pixels'], 1)
        self.assertEqual(report['newly_clear_pixels'], 0)
        old, _ = self.packet('legacy', reversed_depth=False)
        with self.assertRaisesRegex(ValueError, 'different depth conventions'):
            aovs.compare(aovs.inspect(first), aovs.inspect(old, True))

    def test_benchmark_join_refuses_duplicate_or_mismatched_capture(self):
        _, metadata = self.packet()
        benchmark = dict(schema='luminumbra.render_benchmark.v2', width=2, height=2,
            internal_width=2, internal_height=2, terrain_coverage=dict(capture_complete=True,
                frame_observations=[dict(render_frame=641, benchmark_phase='capture', far=metadata['far'])]))
        self.assertEqual(aovs.validate_benchmark(metadata, benchmark)['capture_frame'], 641)
        bad = copy.deepcopy(benchmark)
        bad['terrain_coverage']['frame_observations'] *= 2
        with self.assertRaises(ValueError):
            aovs.validate_benchmark(metadata, bad)
        bad = copy.deepcopy(benchmark)
        bad['terrain_coverage']['frame_observations'][0]['far']['wanted'] = 9
        with self.assertRaises(ValueError):
            aovs.validate_benchmark(metadata, bad)


if __name__ == '__main__':
    unittest.main()
