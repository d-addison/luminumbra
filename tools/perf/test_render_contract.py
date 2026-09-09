#!/usr/bin/env python3
"""Measurement validity tests, run in the existing cross-platform Python CI lane."""
import argparse
import copy
import json
import math
from pathlib import Path
import subprocess
import sys
import unittest

import perf
import render_contract as contract
import validate_render_capture as capture

FIXTURES = Path(__file__).parent / 'fixtures'
QUALIFIED = 'NVIDIA GeForce RTX 5070 Ti'


def artifact():
    """Synthetic positive control; every number is an oracle input, not a capture."""
    rows = []
    for frame, gpu in enumerate([3.0, 4.0, 6.0, 12.0], 2):
        passes = {name: {'issued': False, 'ms': None} for name in contract.PASSES}
        passes['gbuffer'] = {'issued': True, 'ms': gpu - 1}
        rows.append(dict(frame=frame, gpu_frame=frame, frame_wall_ms=gpu + 2,
                         cpu_submit_ms=2.0, present_ms=gpu, gpu_frame_ms=gpu,
                         gpu_pass_sum_ms=gpu - 1, passes=passes))
    data = {'schema': contract.SCHEMA, 'frames': len(rows), 'warmup_frames': 2,
            'width': 1280, 'height': 720, 'render_scale': 1.0,
            'internal_width': 1280, 'internal_height': 720, 'gpu_timers_supported': True,
            'frame_samples': rows,
            'adapter': {'vendor': 'NVIDIA Corporation', 'renderer': QUALIFIED, 'version': '4.5 synthetic fixture'},
            'profile': {'name': 'quality', 'render_scale': 1.0, 'width': 1280, 'height': 720},
            'debug_overrides': {'active': {}, 'declared': {}},
            'excluded_windows': [{'first_frame': 0, 'last_frame': 1, 'reason': 'warmup'},
                                 {'first_frame': 6, 'last_frame': 6, 'reason': 'report and optional screenshot'}],
            'workload': {'kind': 'fixed_view', 'seed': 424242, 'preset': 'default',
                         'preset_revision': 6, 'expected_frames': len(rows)},
            'avg': {}, 'avg_ms': {}, 'distribution': {}}
    for name in contract.METRICS:
        summary = perf.summarize([row[name] for row in rows], 'ms')
        data['distribution'][name] = {key: summary[key] for key in contract.STATS}
        data['distribution'][name].update(unit='ms', sample_count=len(rows), unavailable_count=0)
        data['avg'][name] = sum(row[name] for row in rows) / len(rows)
    for name in contract.PASSES:
        data['avg_ms'][name] = sum(row['passes'][name]['ms'] or 0 for row in rows) / len(rows)
    data['avg_ms']['total'] = data['avg']['gpu_pass_sum_ms']
    data['avg_ms']['ssao_total'] = 0
    return data


def args(**overrides):
    result = dict(position=None, yaw=None, pitch=None, tod=None, fov=None,
                  qualified_renderer=QUALIFIED, qualified_vendor=None,
                  traversal=None, workload_manifest=None, require_controller=False,
                  require_distinct_controller=False, require_geometry=False,
                  require_settled=False, require_camera_chunk=False)
    result.update(overrides)
    return argparse.Namespace(**result)


class RenderContractTests(unittest.TestCase):
    def test_valid_capture(self):
        self.assertEqual(capture.validate(artifact(), args())['verdict'], 'PASS')

    def test_whole_frame_brackets_pass_sum(self):
        data = artifact()
        data['frame_samples'][0]['gpu_frame_ms'] = 1.0
        with self.assertRaisesRegex(ValueError, 'whole-frame GPU'):
            contract.validate(data, QUALIFIED)

    def test_incomplete_timer_coverage(self):
        for field in contract.METRICS:
            data = artifact()
            data['frame_samples'][0][field] = None
            with self.subTest(field=field), self.assertRaises(ValueError):
                contract.validate(data, QUALIFIED)
        data = artifact()
        data['frame_samples'][0]['passes']['gbuffer']['ms'] = None
        with self.assertRaises(ValueError):
            contract.validate(data, QUALIFIED)
        data = artifact()
        data['distribution']['gpu_frame_ms']['unavailable_count'] = 1
        with self.assertRaisesRegex(ValueError, 'incomplete timer'):
            contract.validate(data, QUALIFIED)

    def test_adapter_mismatch_committed_fixture(self):
        data = json.loads((FIXTURES / 'adapter-mismatch.v3.json').read_text())
        with self.assertRaisesRegex(ValueError, 'adapter does not match'):
            contract.validate(data, QUALIFIED)
        data['adapter']['renderer'] = QUALIFIED
        self.assertEqual(contract.validate(data, QUALIFIED)['verdict'], 'PASS')

    def test_adapter_mismatch_cli_fails(self):
        result = subprocess.run([sys.executable, str(Path(capture.__file__)),
                                 str(FIXTURES / 'adapter-mismatch.v3.json'),
                                 '--qualified-renderer', QUALIFIED], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn('adapter does not match', json.loads(result.stdout)['reason'])

    def test_adapter_identity_required(self):
        data = artifact()
        data['gpu_timers_supported'] = False
        with self.assertRaisesRegex(ValueError, 'GPU timers unavailable'):
            contract.validate(data, QUALIFIED)
        with self.assertRaisesRegex(ValueError, 'qualified-renderer'):
            contract.validate(artifact())
        data = artifact()
        data['adapter']['version'] = None
        with self.assertRaises(ValueError):
            contract.validate(data, QUALIFIED)

    def test_undeclared_render_debug_override(self):
        data = artifact()
        data['debug_overrides']['active']['env:LUMIN_RENDER_SCALE'] = '0.67'
        with self.assertRaisesRegex(ValueError, 'undeclared'):
            contract.validate(data, QUALIFIED)
        data['debug_overrides']['declared'] = copy.deepcopy(data['debug_overrides']['active'])
        self.assertEqual(contract.validate(data, QUALIFIED)['verdict'], 'PASS')

    def test_shared_cpp_percentile_fixture(self):
        tokens = (FIXTURES / 'distribution.txt').read_text().split()
        count = int(tokens[0])
        values = [float(v) for v in tokens[1:1 + count]]
        expected = [float(v) for v in tokens[1 + count:]]
        actual = perf.summarize(values, 'ms')
        self.assertEqual(len(expected), len(contract.STATS))
        for stat, value in zip(contract.STATS, expected):
            self.assertAlmostEqual(actual[stat], value, places=12)

    def test_forged_distribution_and_mean(self):
        for group, key in [('distribution', 'p95'), ('avg', 'gpu_frame_ms')]:
            data = artifact()
            if group == 'distribution':
                data[group]['gpu_frame_ms'][key] += 1
            else:
                data[group][key] += 1
            with self.subTest(group=group), self.assertRaises(ValueError):
                contract.validate(data, QUALIFIED)

    def test_late_query_frame_mismatch(self):
        data = artifact()
        data['frame_samples'][0]['gpu_frame'] += 1
        with self.assertRaisesRegex(ValueError, 'originating frame'):
            contract.validate(data, QUALIFIED)

    def test_profile_and_exclusions(self):
        mutations = [lambda d: d['profile'].update(render_scale=0.67),
                     lambda d: d.update(internal_width=100),
                     lambda d: d['excluded_windows'][0].update(reason=''),
                     lambda d: d['excluded_windows'][0].update(last_frame=2),
                     lambda d: d.update(excluded_windows=[])]
        for mutate in mutations:
            data = artifact()
            mutate(data)
            with self.assertRaises(ValueError):
                contract.validate(data, QUALIFIED)

    def test_corrupt_and_future_schema(self):
        for mutation in [lambda d: d.update(schema='luminumbra.render_benchmark.v4'),
                         lambda d: d['frame_samples'][0].update(gpu_frame_ms=float('nan')),
                         lambda d: d['frame_samples'][0].update(gpu_frame_ms=True),
                         lambda d: d['frame_samples'][0]['passes']['water'].update(ms=0)]:
            data = artifact()
            mutation(data)
            with self.assertRaises(ValueError):
                capture.validate(data, args())

    def test_workload_mismatch(self):
        with self.assertRaisesRegex(ValueError, 'workload manifest'):
            contract.validate(artifact(), QUALIFIED, workload={'seed': 1})

    def test_traversal_manifest_and_camera_oracle(self):
        manifest = contract.traversal_manifest((FIXTURES / 'surface-flight.traversal').read_text())
        first = [contract.camera_sample(manifest, tick) for tick in range(1801)]
        second = [contract.camera_sample(manifest, tick) for tick in range(1801)]
        self.assertEqual(first, second)
        self.assertEqual(first[0]['position'], [8, 80, 8])
        self.assertEqual(first[900]['position'], [248, 80, 8])
        self.assertEqual(first[1800]['position'], [488, 80, 8])
        with self.assertRaises(ValueError):
            contract.traversal_manifest((FIXTURES / 'surface-flight.traversal').read_text().replace('expected_ticks 1800', 'expected_ticks 1799'))

    def test_traversal_capture_and_refusals(self):
        script = FIXTURES / 'surface-flight.traversal'
        manifest = contract.traversal_manifest(script.read_text())
        data = artifact()
        data['workload'].update({key: value for key, value in manifest.items() if key != 'path'})
        data['workload'].update(kind='traversal', actual_ticks=1800, measured_duration_seconds=60,
                                script=script.read_text(), script_path=str(script),
                                camera_samples=[contract.camera_sample(manifest, tick) for tick in (0, 600, 1200, 1800)])
        for row in data['frame_samples']:
            row['frame_wall_ms'] = 15000
        summary = perf.summarize([15000] * 4, 'ms')
        data['distribution']['frame_wall_ms'].update({key: summary[key] for key in contract.STATS})
        data['avg']['frame_wall_ms'] = 15000
        self.assertEqual(contract.validate(data, QUALIFIED, traversal=script)['verdict'], 'PASS')
        for mutate in [lambda d: d['workload'].update(actual_ticks=1799),
                       lambda d: d['workload'].update(measured_duration_seconds=59),
                       lambda d: d['workload'].update(measured_duration_seconds=61),
                       lambda d: d['workload'].update(script='luminumbra.traversal.v2'),
                       lambda d: d['workload']['camera_samples'][1].update(position=[0, 0, 0]),
                       lambda d: d['workload']['camera_samples'][-1].update(tick=1799)]:
            candidate = copy.deepcopy(data)
            mutate(candidate)
            with self.assertRaises(ValueError):
                contract.validate(candidate, QUALIFIED, traversal=script)
        with self.assertRaisesRegex(ValueError, 'manifest does not match'):
            contract.validate(data, QUALIFIED)

    def test_v2_existing_report_checks_unchanged(self):
        # A complete synthetic v2 linked-uniform capture, without any v3 keys.
        data = {'schema': 'luminumbra.render_benchmark.v2', 'width': 1280, 'height': 720,
                'camera': {'position': [0, 0, 0], 'yaw': 0, 'pitch': 0, 'fov': 90},
                'capture_context': {'time_of_day_at_report': 0, 'last_world_streaming_position': [0, 0, 0]},
                'render_uniforms': {'matrix_layout': 'column_major', 'observation': 'report_time_shader_state'}}
        right, up, forward = capture.camera_basis(0, 0)
        view = [right[0], up[0], -forward[0], 0, right[1], up[1], -forward[1], 0,
                right[2], up[2], -forward[2], 0, 0, 0, 0, 1]
        projection = [0.0] * 16
        projection[0], projection[5] = 720 / 1280, 1.0
        for name in ('geometry', 'instanced_static_mesh'):
            data['render_uniforms'][name] = dict(available=True, view=view, projection=projection)
        data['render_uniforms']['lighting'] = {'available': True, 'u_viewPos': [0, 0, 0],
            'u_inverseView': right + [0] + up + [0] + [-v for v in forward] + [0] + [0, 0, 0, 1],
            'u_sun.direction': [0, -1, 0]}
        before = copy.deepcopy(data)
        result = capture.validate(data, args(position=[0, 0, 0], yaw=0, pitch=0, tod=0, qualified_renderer=None))
        self.assertEqual(result['verdict'], 'PASS')
        self.assertEqual(data, before)


if __name__ == '__main__':
    unittest.main()
