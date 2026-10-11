#!/usr/bin/env python3
"""Tests for the qualification execution runner, its aggregate and exit codes.

The client and checker are fake scripts written into a temporary directory, so no
GPU is needed. One test runs the real validate_render_capture.py checker.
"""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import sys
import tempfile
from unittest import mock
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import plan_qualification as planner  # noqa: E402
import qualification_matrix as matrix_lib  # noqa: E402
import run_qualification as rq  # noqa: E402

MATRIX_PATH = planner.DEFAULT_MATRIX
FIXTURE_PATH = Path(__file__).resolve().parent / 'fixtures' / 'adapter-mismatch.v3.json'
RENDERER = 'NVIDIA GeForce RTX 5070 Ti'
VENDOR = 'NVIDIA Corporation'
POSE = {'x': 1.0, 'y': 2.0, 'z': 3.0, 'yaw': 4.0, 'pitch': 5.0}
SURFACE_CELL = 'surface-flight.quality.3840x1600'
FOREST_CELL = 'forest-settled.quality.3840x1600'
CAVE_BLOCKED = 'blocked: traversal v2 events (fw #132); spec only'

FAKE_CLIENT = '''#!/usr/bin/env python3
"""Fake render benchmark client; behaviour chosen by FAKE_CLIENT_MODE."""
import json
import os
from pathlib import Path
import sys
import time

mode = os.environ.get('FAKE_CLIENT_MODE', 'ok')
argv = sys.argv[1:]
if mode == 'exit3':
    sys.exit(3)
if mode == 'hang':
    time.sleep(30)
    sys.exit(0)
artifact = Path(argv[argv.index('--render-benchmark') + 1])
if mode == 'noartifact':
    sys.exit(0)
if mode == 'copyfixture':
    artifact.write_bytes(Path(os.environ['FAKE_CLIENT_FIXTURE']).read_bytes())
    sys.exit(0)
renderer = 'Mesa llvmpipe' if mode == 'wrongadapter' else 'NVIDIA GeForce RTX 5070 Ti'
payload = {'schema': 'luminumbra.render_benchmark.v3',
           'adapter': {'vendor': 'NVIDIA Corporation', 'renderer': renderer,
                       'version': '4.6'}}
if mode == 'dumpenv':
    payload['saw_lumin_env'] = any(key.startswith(('LUMIN_', 'LUMINUMBRA_'))
                                   for key in os.environ)
artifact.write_text(json.dumps(payload), encoding='utf-8')
sys.exit(0)
'''

FAKE_CHECKER = '''#!/usr/bin/env python3
"""Fake checker: writes a minimal floor verdict and exits with FAKE_CHECKER_EXIT."""
import json
import os
from pathlib import Path
import sys

argv = sys.argv[1:]
code = int(os.environ.get('FAKE_CHECKER_EXIT', '0'))
verdict = 'pass' if code == 0 else 'fail'
Path(argv[argv.index('--verdict-out') + 1]).write_text(
    json.dumps({'schema': 'luminumbra.render_verdict.v1', 'floor': {'verdict': verdict}}),
    encoding='utf-8')
sys.exit(code)
'''


def adapter(renderer=RENDERER):
    return dict(vendor=VENDOR, renderer=renderer, version='4.6')


def passing_results(matrix):
    return [dict(id=cell['id'], state='pass', reason=None, adapter=adapter(),
                 artifact=None, verdict=None) for cell in matrix_lib.cells(matrix)]


class RunnerTestBase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.tmp = Path(self.directory.name)
        self.fake_client = self.tmp / 'fake_client.py'
        self.fake_client.write_text(FAKE_CLIENT, encoding='utf-8')
        self.fake_checker = self.tmp / 'fake_checker.py'
        self.fake_checker.write_text(FAKE_CHECKER, encoding='utf-8')
        self.out = self.tmp / 'out'
        self.matrix_path = self.write_matrix(with_poses=True)

    def write_matrix(self, with_poses):
        matrix = json.loads(MATRIX_PATH.read_text(encoding='utf-8'))
        if with_poses:
            for workload in matrix['workloads']:
                if workload['kind'] == 'fixed_view':
                    workload['pose'] = dict(POSE)
        path = self.tmp / 'matrix.json'
        path.write_text(json.dumps(matrix, indent=2), encoding='utf-8')
        return path

    def argv(self, *extra, matrix=None, checker=True, out=None):
        args = ['--client', str(self.fake_client), '--out-dir', str(out or self.out),
                '--matrix', str(matrix or self.matrix_path)]
        if checker:
            args += ['--checker', str(self.fake_checker)]
        return args + list(extra)

    def run_main(self, args, env=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        base = {'FAKE_CLIENT_MODE': 'ok', 'FAKE_CHECKER_EXIT': '0'}
        base.update(env or {})
        with mock.patch.dict(os.environ, base), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = rq.main(args)
        return code, stdout.getvalue(), stderr.getvalue()

    def load_qualification(self, out=None):
        path = (out or self.out) / 'qualification.json'
        return json.loads(path.read_text(encoding='utf-8'))

    def cell_of(self, document, cell_id):
        return next(cell for cell in document['cells'] if cell['id'] == cell_id)


class DryRunTests(RunnerTestBase):
    def test_dry_run_writes_plan_and_launches_nothing(self):
        code, out, _ = self.run_main(self.argv('--dry-run'))
        self.assertEqual(code, 0)
        self.assertEqual([path.name for path in self.out.iterdir()],
                         [planner.PLAN_NAME])
        plan = json.loads((self.out / planner.PLAN_NAME).read_text(encoding='utf-8'))
        self.assertEqual(plan['schema'], planner.PLAN_SCHEMA)
        self.assertEqual(plan['cell_count'], 36)
        self.assertEqual(plan['summary'], {'ready': 28, 'unevaluated': 8})
        self.assertIn('qualification plan: 36 cells', out)


class FullMatrixTests(RunnerTestBase):
    def test_full_matrix_with_recorded_poses(self):
        code, out, _ = self.run_main(self.argv())
        self.assertEqual(code, 3)
        document = self.load_qualification()
        self.assertEqual(document['schema'], rq.SCHEMA)
        self.assertEqual(document['summary'], {'pass': 28, 'fail': 0, 'invalid': 0,
                                               'process_failed': 0, 'unevaluated': 8})
        self.assertFalse(document['partial'])
        self.assertTrue(document['harness_complete'])
        self.assertTrue(document['adapter_identical'])
        self.assertTrue(document['adapter_matches_qualified'])
        self.assertFalse(document['render_floor_qualified'])
        self.assertFalse(document['expanded_qualified'])
        self.assertEqual(self.cell_of(document, 'cave-walk.quality.3840x1600')['reason'],
                         CAVE_BLOCKED)
        sources = {display['id']: display['refresh_source']
                   for display in document['displays']}
        self.assertEqual(sources, {'3840x1600': 'matrix', '3440x1440': 'undeclared'})
        self.assertIn('pass 28', out)
        self.assertTrue((self.out / 'cells' / f'{SURFACE_CELL}.verdict.json').is_file())

    def test_declared_refresh_recorded_but_unevaluated_cells_block(self):
        code, _, _ = self.run_main(self.argv('--declare-refresh', '3440x1440=60'))
        self.assertEqual(code, 3)
        document = self.load_qualification()
        self.assertEqual(document['declared_refresh'], {'3440x1440': 60})
        self.assertFalse(document['render_floor_qualified'])


class SelectionTests(RunnerTestBase):
    def test_default_matrix_leaves_fixed_views_unrecorded(self):
        args = ['--client', str(self.fake_client), '--out-dir', str(self.out),
                '--checker', str(self.fake_checker), '--only', FOREST_CELL]
        code, _, _ = self.run_main(args)
        self.assertEqual(code, 3)
        cell = self.cell_of(self.load_qualification(), FOREST_CELL)
        self.assertEqual((cell['state'], cell['reason']), ('unevaluated', 'pose-unrecorded'))
        self.assertFalse((self.out / 'cells' / f'{FOREST_CELL}.json').exists())

    def test_partial_recorded_pose_cell_passes_with_exit_0(self):
        code, _, _ = self.run_main(self.argv('--only', FOREST_CELL))
        self.assertEqual(code, 0)
        document = self.load_qualification()
        self.assertTrue(document['partial'])
        self.assertFalse(document['render_floor_qualified'])
        self.assertFalse(document['harness_complete'])
        self.assertEqual(document['summary']['pass'], 1)

    def test_unknown_only_id_exits_2(self):
        code, _, err = self.run_main(self.argv('--only', 'no-such.quality.3840x1600'))
        self.assertEqual(code, 2)
        self.assertIn('unknown cell id', err)
        self.assertNotIn('Traceback', err)

    def test_malformed_declare_refresh_exits_2(self):
        for value in ('3440x1440', '3440x1440=abc', '3440x1440=0', '3440x1440=1001',
                      '3440x1440=', '=60', 'nope=60', '3440x1440=60.5'):
            with self.subTest(value=value):
                code, _, err = self.run_main(self.argv('--declare-refresh', value))
                self.assertEqual(code, 2)
                self.assertIn('--declare-refresh', err)
                self.assertNotIn('Traceback', err)

    def test_declaration_over_matrix_refresh_exits_2(self):
        code, _, _ = self.run_main(self.argv('--declare-refresh', '3840x1600=60'))
        self.assertEqual(code, 2)

    def test_unwritable_out_dir_exits_2(self):
        blocker = self.tmp / 'blocker'
        blocker.write_text('not a directory', encoding='utf-8')
        code, _, err = self.run_main(self.argv(out=blocker / 'out'))
        self.assertEqual(code, 2)
        self.assertIn('cannot write', err)


class ProcessFailureTests(RunnerTestBase):
    def run_surface(self, env, *extra):
        return self.run_main(self.argv('--only', SURFACE_CELL, *extra), env=env)

    def test_checker_floor_failure_is_fail_and_exit_1(self):
        code, _, _ = self.run_surface({'FAKE_CHECKER_EXIT': '4'})
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']), ('fail', 'floor'))
        self.assertEqual(cell['verdict']['floor']['verdict'], 'fail')

    def test_checker_exit_1_is_invalid(self):
        code, _, _ = self.run_surface({'FAKE_CHECKER_EXIT': '1'})
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']), ('invalid', 'checker exit 1'))

    def test_client_exit_3_is_process_failed(self):
        code, _, _ = self.run_surface({'FAKE_CLIENT_MODE': 'exit3'})
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']), ('process_failed', 'exit 3'))

    def test_client_without_artifact_is_process_failed(self):
        code, _, _ = self.run_surface({'FAKE_CLIENT_MODE': 'noartifact'})
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']), ('process_failed', 'no-artifact'))

    def test_hang_times_out_and_writes_receipt(self):
        code, _, _ = self.run_surface({'FAKE_CLIENT_MODE': 'hang'},
                                      '--timeout-seconds', '1')
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']), ('process_failed', 'timed_out'))
        receipt = json.loads((self.out / 'cells' / f'{SURFACE_CELL}.process.json')
                             .read_text(encoding='utf-8'))
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['timeout_seconds'], 1)

    def test_missing_client_is_spawn_failed(self):
        args = ['--client', str(self.tmp / 'missing-client'), '--out-dir', str(self.out),
                '--matrix', str(self.matrix_path), '--checker', str(self.fake_checker),
                '--only', SURFACE_CELL]
        code, _, _ = self.run_main(args)
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual(cell['state'], 'process_failed')
        self.assertTrue(cell['reason'].startswith('spawn-failed: '), cell['reason'])

    def test_wrong_adapter_fails_qualification_even_when_checker_passes(self):
        code, _, _ = self.run_surface({'FAKE_CLIENT_MODE': 'wrongadapter'})
        self.assertEqual(code, 0)
        document = self.load_qualification()
        cell = self.cell_of(document, SURFACE_CELL)
        self.assertEqual(cell['state'], 'pass')
        self.assertEqual(cell['adapter']['renderer'], 'Mesa llvmpipe')
        self.assertFalse(document['adapter_matches_qualified'])
        self.assertFalse(document['render_floor_qualified'])


class ArtifactHandlingTests(RunnerTestBase):
    def test_prior_artifact_is_moved_aside_not_overwritten(self):
        cells = self.out / 'cells'
        cells.mkdir(parents=True)
        artifact = cells / f'{SURFACE_CELL}.json'
        artifact.write_text('old capture', encoding='utf-8')
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        self.assertEqual((cells / f'{SURFACE_CELL}.json.prev-1').read_text(encoding='utf-8'),
                         'old capture')
        self.assertIn('adapter', json.loads(artifact.read_text(encoding='utf-8')))
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        self.assertTrue((cells / f'{SURFACE_CELL}.json.prev-2').is_file())
        self.assertFalse((cells / f'{SURFACE_CELL}.json.prev-3').exists())

    def test_lumin_variables_are_not_seen_by_client(self):
        env = {'FAKE_CLIENT_MODE': 'dumpenv', 'LUMIN_GL_DEBUG': '1',
               'LUMINUMBRA_TEST_FLAG': '1'}
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL), env=env)[0], 0)
        artifact = json.loads((self.out / 'cells' / f'{SURFACE_CELL}.json')
                              .read_text(encoding='utf-8'))
        self.assertIs(artifact['saw_lumin_env'], False)


class RealCheckerTests(RunnerTestBase):
    def test_real_checker_refuses_adapter_mismatch_fixture(self):
        env = {'FAKE_CLIENT_MODE': 'copyfixture',
               'FAKE_CLIENT_FIXTURE': str(FIXTURE_PATH)}
        args = self.argv('--only', SURFACE_CELL, checker=False)
        code, _, _ = self.run_main(args, env=env)
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual(cell['state'], 'invalid')
        self.assertTrue(cell['reason'].startswith('checker exit '), cell['reason'])
        self.assertTrue((self.out / 'cells' / f'{SURFACE_CELL}.check.log').is_file())


class AggregateTests(unittest.TestCase):
    def setUp(self):
        self.matrix = matrix_lib.load_matrix(MATRIX_PATH)
        self.declared = {'3440x1440': 60}

    def aggregate(self, results, partial=False, declared=None):
        return rq.aggregate(self.matrix, results, partial=partial,
                            declared_refresh=self.declared if declared is None else declared)

    def test_all_pass_with_declared_refresh_is_qualified(self):
        document = self.aggregate(passing_results(self.matrix))
        self.assertEqual(document['summary']['pass'], 36)
        self.assertTrue(document['render_floor_qualified'])
        self.assertTrue(document['harness_complete'])
        self.assertTrue(document['adapter_identical'])
        self.assertTrue(document['adapter_matches_qualified'])
        self.assertFalse(document['expanded_qualified'])
        self.assertEqual(document['expanded_qualified_reason'], rq.EXPANDED_REASON)
        self.assertEqual(document['displays'][1],
                         {'id': '3440x1440', 'refresh_hz': 60,
                          'refresh_source': 'operator'})
        self.assertEqual(document['displays'][0]['refresh_source'], 'matrix')

    def test_one_differing_adapter_breaks_identity(self):
        results = passing_results(self.matrix)
        results[5]['adapter'] = adapter('Mesa llvmpipe')
        document = self.aggregate(results)
        self.assertFalse(document['adapter_identical'])
        self.assertFalse(document['adapter_matches_qualified'])
        self.assertFalse(document['render_floor_qualified'])

    def test_one_cell_not_pass_breaks_qualification(self):
        results = passing_results(self.matrix)
        results[0] = dict(results[0], state='fail', reason='floor')
        document = self.aggregate(results)
        self.assertEqual(document['summary']['fail'], 1)
        self.assertFalse(document['render_floor_qualified'])
        self.assertTrue(document['harness_complete'])

    def test_invalid_cell_breaks_harness_completeness(self):
        results = passing_results(self.matrix)
        results[0] = dict(results[0], state='invalid', reason='checker exit 1')
        document = self.aggregate(results)
        self.assertFalse(document['harness_complete'])
        self.assertFalse(document['render_floor_qualified'])

    def test_undeclared_display_breaks_qualification(self):
        document = self.aggregate(passing_results(self.matrix), declared={})
        self.assertEqual(document['displays'][1]['refresh_source'], 'undeclared')
        self.assertIsNone(document['displays'][1]['refresh_hz'])
        self.assertFalse(document['render_floor_qualified'])

    def test_partial_run_is_never_qualified(self):
        document = self.aggregate(passing_results(self.matrix), partial=True)
        self.assertTrue(document['partial'])
        self.assertFalse(document['harness_complete'])
        self.assertFalse(document['render_floor_qualified'])

    def test_unevaluated_cells_allowed_for_harness_but_not_qualification(self):
        results = passing_results(self.matrix)
        results[1] = dict(results[1], state='unevaluated', reason='pose-unrecorded',
                          adapter=None)
        document = self.aggregate(results)
        self.assertTrue(document['harness_complete'])
        self.assertFalse(document['render_floor_qualified'])

    def test_matrix_refresh_is_not_overridden(self):
        matrix = copy.deepcopy(self.matrix)
        matrix['displays'][0]['refresh_hz'] = 100
        document = rq.aggregate(matrix, passing_results(matrix), partial=False,
                                declared_refresh={})
        self.assertEqual(document['displays'][0]['refresh_hz'], 100)
        self.assertEqual(document['displays'][0]['refresh_source'], 'matrix')


if __name__ == '__main__':
    unittest.main()
