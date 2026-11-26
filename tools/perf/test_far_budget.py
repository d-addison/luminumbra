#!/usr/bin/env python3
"""Synthetic controls for matrix completeness and frame-attributed measurements."""
import copy
import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import far_budget as budget
import render_contract
import test_render_contract as controls
from test_perf import far_capture, update_far_summary
from test_render_contract import budget_artifact


def synthetic_capture(directory):
    data = budget_artifact()
    data['far_world'] = far_capture()['far_world']
    text = '''luminumbra.traversal.v1
 duration_seconds 0.13333333333333333
 tick_rate 30
 speed_mps 8
 seed 424242
 preset default
 preset_revision 8
 preset_identity 56ffbe1771cddfa6
 content_identity 7583622726659997707
 expected_ticks 4
 cold_cache true
 points 2
 point 8 80 8 0 -6
 point 488 80 8 0 -6
'''
    path = directory / 'workload.traversal'
    path.write_bytes(text.encode('utf-8'))
    manifest = render_contract.traversal_manifest(text)
    data['workload'].update({key: value for key, value in manifest.items() if key != 'path'})
    data['workload'].update(kind='traversal', script=text, actual_ticks=4,
        measured_duration_seconds=0.001,
        camera_samples=[render_contract.camera_sample(manifest, tick) for tick in range(1, 5)])
    matrix = budget.load_matrix()
    matrix.update(warmup_frames=2, measured_frames=4)
    cell = budget.cells(budget.load_matrix())[1]
    world = matrix['worlds'][0]
    width, height = map(int, cell['display'].split('x'))
    data.update(width=width, height=height, internal_width=width, internal_height=height)
    data['profile'].update(width=width, height=height)
    return data, matrix, cell, world, path


class FarBudgetTests(unittest.TestCase):
    def test_matrix_requires_every_world_profile_display(self):
        matrix = budget.load_matrix()
        cells = budget.cells(matrix)
        self.assertEqual(len(cells), 32)
        self.assertEqual(len({(c['world'], c['profile'], c['display']) for c in cells}), 16)
        self.assertEqual(len({c['id'] for c in cells}), 32)
        for key in ('worlds', 'profiles', 'displays', 'workloads'):
            for position in range(len(matrix[key])):
                broken = copy.deepcopy(matrix)
                broken[key].pop(position)
                with self.subTest(key=key, position=position), self.assertRaises(ValueError):
                    budget.validate_matrix(broken)
        broken = copy.deepcopy(matrix)
        broken['worlds'][1] = broken['worlds'][0]
        with self.assertRaises(ValueError):
            budget.validate_matrix(broken)

    def test_committed_dependencies_are_pinned_or_explicitly_incomplete(self):
        matrix = budget.load_matrix()
        for cell in budget.cells(matrix):
            world = next(w for w in matrix['worlds'] if w['id'] == cell['world'])
            reasons = budget.dependencies(world, cell)
            ready = cell['world'] == 'default' and cell['workload'] == 'traversal'
            self.assertEqual(not reasons, ready, (cell['id'], reasons))
            self.assertEqual(len(budget.fixture_identity(world)), 64)

    def test_fixture_hash_covers_dependencies_and_metadata(self):
        world = budget.load_matrix()['worlds'][0]
        identity = budget.fixture_identity(world)
        for key, value in [('seed', 1), ('pose', {'x': 1}), ('preset_identity', '1' * 16),
                           ('inputs', {'data/common/biomes.json': 'a' * 64}), ('script_sha256', 'b' * 64)]:
            changed = copy.deepcopy(world)
            changed[key] = value
            self.assertNotEqual(identity, budget.fixture_identity(changed))

    def test_matrix_refuses_unknown_partial_and_invalid_values(self):
        matrix = budget.load_matrix()
        changes = [('schema', 'unknown'), ('worlds', []), ('warmup_frames', True),
                   ('renderer', 'other'), ('measured_frames', 0), ('extra', None)]
        for key, value in changes:
            broken = copy.deepcopy(matrix)
            broken[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                budget.validate_matrix(broken)
        for key, value in [('seed', 1e38), ('content_identity', '18446744073709551616'),
                           ('script', 'C:fixture'), ('script_sha256', None), ('preset', 'other'),
                           ('inputs', {}), ('pose', {'x': 0}), ('blocked_by', '')]:
            broken = copy.deepcopy(matrix)
            broken['worlds'][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                budget.validate_matrix(broken)

    def test_missing_inputs_incomplete_but_changed_or_escaping_inputs_refused(self):
        matrix = budget.load_matrix()
        world, cell = matrix['worlds'][0], budget.cells(matrix)[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertIn('missing fixture input:', '\n'.join(budget.dependencies(world, cell, root)))
            changed = copy.deepcopy(world)
            changed['inputs'] = {'../escape': '0' * 64}
            with self.assertRaises(ValueError):
                budget.dependencies(changed, cell, root)
        changed = copy.deepcopy(world)
        changed['inputs']['data/common/biomes.json'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'identity differs'):
            budget.dependencies(changed, cell)
        changed = copy.deepcopy(world)
        changed['script_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'traversal identity'):
            budget.dependencies(changed, cell)
        changed = copy.deepcopy(world)
        changed['seed'] = 1
        with self.assertRaisesRegex(ValueError, 'traversal fixture'):
            budget.dependencies(changed, cell)

    def test_json_bounds_duplicates_nonfinite_partial_and_depth(self):
        for payload in (b'{', b'{"a":1,"a":2}', b'{"a":NaN}', b'[' * 65 + b']' * 65, b'\xff'):
            with self.subTest(payload=payload[:25]), self.assertRaises(ValueError):
                budget.decode(payload)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'large').write_bytes(b'12345')
            with self.assertRaisesRegex(ValueError, 'oversized'):
                budget.read(root, 'large', 4)
            (root / 'folder').mkdir()
            with self.assertRaises(ValueError):
                budget.read(root, 'folder', 4)
        self.assertEqual(budget.decode(b'\xef\xbb\xbf{\r\n"a":1\r\n}'), {'a': 1})

    def test_hitch_join_uses_originating_frame(self):
        data = budget_artifact()
        # Simulate delayed timing attribution. Several render rows may identify the
        # same originating sample; none may be joined by their render-frame index.
        data['frame_samples'][0]['frame_wall_ms'] = 40
        data['far_budget']['frames'][2]['tiers'][0]['work']['build_attempts'] = 7
        data['far_budget']['frames'][3]['tiers'][0]['work']['build_attempts'] = 13
        result = budget.measurements(data)
        self.assertEqual(len(result['hitches']), 1)
        hitch = result['hitches'][0]
        self.assertEqual((hitch['timing_frame'], hitch['render_frame']), (2, 3))
        self.assertEqual(hitch['tiers'][0]['work']['build_attempts'], 7)
        self.assertEqual(result['phases']['cold_or_settling']['frames'], 2)
        self.assertEqual(result['phases']['measured']['frames'], 4)
        self.assertEqual(result['fills'][0]['elapsed_from_origin_ms'], 20)
        self.assertEqual(result['fills'][0]['elapsed_from_acceptance_ms'], 10)
        self.assertEqual(result['fills'][0]['tier_elapsed_ms'], [20, 10, 10, 10, 10])
        data['far_budget']['frames'][1].update(timing_frame=2, phase='measured')
        joined = budget.measurements(data)['hitches']
        self.assertEqual([row['render_frame'] for row in joined], [2, 3])
        self.assertTrue(all(row['timing_frame'] == 2 for row in joined))

    def test_summaries_preserve_tier_peaks_attempts_failures_and_draws(self):
        data = budget_artifact()
        tier = data['far_budget']['frames'][0]['tiers'][0]
        tier['memory'].update(cpu_bytes=80, gpu_bytes=20, total_bytes=100, peak_bytes=120)
        tier['work'].update(build_attempts=9, rejected=2, upload_bytes=60, successful_upload_bytes=20)
        tier.update(draws=4, indices=12)
        result = budget.measurements(data)['phases']['cold_or_settling']
        self.assertEqual(result['combined_resident_peak_bytes'], 220)
        first = result['tiers'][0]
        self.assertEqual(first['memory_peaks']['total_bytes'], 100)
        self.assertEqual(first['memory_peaks']['peak_bytes'], 120)
        self.assertEqual(first['work']['build_attempts']['max'], 9)
        self.assertEqual(first['work']['successful_upload_bytes']['max'], 20)
        self.assertEqual(first['draws']['draws']['max'], 4)
        self.assertEqual(result['resident_peaks'], dict(cpu_bytes=120, gpu_bytes=100, total_bytes=220))

    def test_hitch_boundary_and_missing_cold_phase(self):
        data = budget_artifact()
        data['frame_samples'][0]['frame_wall_ms'] = 16.67
        self.assertEqual(budget.measurements(data)['hitches'], [])
        data['frame_samples'][0]['frame_wall_ms'] = 16.670001
        self.assertEqual(len(budget.measurements(data)['hitches']), 1)
        with tempfile.TemporaryDirectory() as directory:
            data, matrix, cell, world, script = synthetic_capture(Path(directory))
            for row in data['far_budget']['frames'][:2]:
                row.update(phase='measured', timing_frame=2)
            with self.assertRaisesRegex(ValueError, 'cold/settling phase is missing'):
                budget.evaluate(data, matrix, cell, world, script)

    def test_missing_bake_or_legacy_fallback_cannot_qualify(self):
        with tempfile.TemporaryDirectory() as directory:
            data, matrix, cell, world, script = synthetic_capture(Path(directory))
            self.assertEqual(budget.evaluate(data, matrix, cell, world, script)['state'], 'complete')
            edited = dict(world, id='edited')
            with self.assertRaisesRegex(ValueError, 'commit/all-tier bake'):
                budget.evaluate(data, matrix, cell, edited, script)
            for changes in (dict(effective_mode='legacy'), dict(requested_mode='legacy'),
                            dict(content_identity='00'), dict(refusal='ImmutableAuthorityUnavailable'),
                            dict(suppressed=True, submission_refusal='DrawRejected')):
                broken = copy.deepcopy(data)
                broken['far_world']['frames'][0].update(changes)
                update_far_summary(broken)
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    budget.evaluate(broken, matrix, cell, world, script)
            world_edited = budget.load_matrix()['worlds'][-1]
            self.assertTrue(any('commit' in r for r in budget.dependencies(world_edited, cell)))

    def test_capture_binding_and_malformed_inputs_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            data, matrix, cell, world, script = synthetic_capture(Path(directory))
            mutations = [lambda d: d.update(width=3440), lambda d: d.update(warmup_frames=3),
                lambda d: d['profile'].update(name='performance'),
                lambda d: d['workload'].update(seed=1), lambda d: d.pop('far_world'),
                lambda d: d.pop('far_budget'), lambda d: d['frame_samples'].pop(),
                lambda d: d['far_budget']['frames'][0]['tiers'][0]['memory'].update(total_bytes=99),
                lambda d: d['far_budget']['frames'][0]['tiers'][0]['memory'].update(peak_bytes=1)]
            for mutate in mutations:
                broken = copy.deepcopy(data)
                mutate(broken)
                with self.subTest(mutation=mutations.index(mutate)), self.assertRaises(ValueError):
                    budget.evaluate(broken, matrix, cell, world, script)
            missing_identity = dict(world, preset_identity=None)
            with self.assertRaisesRegex(ValueError, 'unavailable'):
                budget.evaluate(data, matrix, cell, missing_identity, script)

    def test_floor_failure_stays_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            data, matrix, cell, world, script = synthetic_capture(Path(directory))
            # Keep the full render validity check while changing the actual measured
            # frame input and recomputing its recorded distribution and mean.
            data['frame_samples'][0]['frame_wall_ms'] = 40
            values = [row['frame_wall_ms'] for row in data['frame_samples']]
            data['distribution']['frame_wall_ms'] = dict(budget.distribution(values, 'ms'), unavailable_count=0)
            data['avg']['frame_wall_ms'] = sum(values) / len(values)
            result = budget.evaluate(data, matrix, cell, world, script)
            self.assertEqual(result['state'], 'fail')
            self.assertEqual(result['verdict']['floor']['verdict'], 'fail')

    def test_settled_capture_keeps_pose_geometry_and_queue_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            data, matrix, cell, world, _ = synthetic_capture(Path(directory))
            cell = dict(cell, workload='settled')
            world = dict(world, pose=dict(x=0.0, y=0.0, z=0.0, yaw=0.0, pitch=0.0))
            data['workload'] = dict(kind='fixed_view', expected_frames=4,
                **{key: world[key] for key in ('preset', 'preset_revision', 'preset_identity', 'content_identity', 'seed')})
            report = controls.RenderContractTests().report_capture()
            report.update(data)
            report['camera']['fov'] = 45
            report['capture_context']['time_of_day_at_report'] = 0.04
            for name in ('geometry', 'instanced_static_mesh'):
                projection = report['render_uniforms'][name]['projection']
                projection[5] = 1 / math.tan(math.radians(45) / 2)
                projection[0] = projection[5] * report['height'] / report['width']
            report['render_uniforms']['lighting']['u_sun.direction'] = [math.sin(2 * math.pi * 0.04), -math.cos(2 * math.pi * 0.04), 0]
            report['streaming'] = dict(terrain_draws=1, generation_pending=0, meshing_pending=0, terrain_uploads_pending=0)
            report['camera_chunk'] = dict(resident=True)
            self.assertEqual(budget.evaluate(report, matrix, cell, world)['state'], 'complete')
            mutations = [lambda d: d['camera'].update(position=[1, 0, 0]),
                         lambda d: d['streaming'].update(terrain_draws=0),
                         lambda d: d['streaming'].update(generation_pending=1),
                         lambda d: d['camera_chunk'].update(resident=False),
                         lambda d: d['render_uniforms']['geometry'].update(available=False)]
            for mutate in mutations:
                broken = copy.deepcopy(report)
                mutate(broken)
                with self.subTest(mutation=mutations.index(mutate)), self.assertRaises(ValueError):
                    budget.evaluate(broken, matrix, cell, world)

    def test_incomplete_cold_and_refill_evidence_refused(self):
        data = budget_artifact()
        data['far_budget']['fills'][0]['status'] = 'timeout'
        with self.assertRaises(ValueError):
            budget.measurements(data)
        with tempfile.TemporaryDirectory() as directory:
            data, matrix, cell, world, script = synthetic_capture(Path(directory))
            last = data['far_budget']['frames'][-1]
            last.update(event=1, current_coverage=False)
            last['tiers'][0].update(current_coverage=False, covering=0, settled=0, pending=1)
            data['far_budget']['fills'].append(dict(event=1, kind='refill', origin_ms=50,
                accepted_ms=60, completed_ms=None, status='incomplete',
                tier_completed_ms=[None, 60, 60, 60, 60]))
            result = budget.evaluate(data, matrix, cell, world, script)
            self.assertEqual(result['state'], 'incomplete')
            self.assertIsNone(result['metrics']['fills'][-1]['completed_ms'])
            self.assertIn('incomplete or timed out fill', result['reasons'])

    def test_incomplete_cold_fill_retains_measured_peaks_without_qualifying(self):
        with tempfile.TemporaryDirectory() as directory:
            data, matrix, cell, world, script = synthetic_capture(Path(directory))
            for row in data['far_budget']['frames']:
                row['current_coverage'] = False
                for tier in row['tiers']:
                    tier.update(current_coverage=False, covering=0, settled=0, pending=1)
            data['far_budget']['fills'][0].update(completed_ms=None, status='incomplete',
                                                tier_completed_ms=[None] * 5)
            result = budget.evaluate(data, matrix, cell, world, script)
            self.assertEqual(result['state'], 'incomplete')
            self.assertEqual(result['metrics']['phases']['measured']['combined_resident_peak_bytes'], 150)
            self.assertIsNone(result['metrics']['fills'][0]['elapsed_from_origin_ms'])
        data = budget_artifact()
        data['far_budget']['frames'][2]['timing_frame'] = 999
        with self.assertRaises(ValueError):
            budget.measurements(data)


if __name__ == '__main__':
    unittest.main()
