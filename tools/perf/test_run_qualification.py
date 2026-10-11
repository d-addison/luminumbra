#!/usr/bin/env python3
"""Tests for the qualification execution runner, its aggregate and exit codes.

The client and checker are fake scripts written into a temporary directory, so no
GPU is needed. Some tests run the real validate_render_capture.py checker.
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
VERDICT_SCHEMA = 'luminumbra.render_verdict.v1'
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


def value(flag):
    return argv[argv.index(flag) + 1] if flag in argv else None


if mode == 'exit3':
    sys.exit(3)
if mode == 'hang':
    time.sleep(30)
    sys.exit(0)
artifact = Path(value('--render-benchmark'))
if mode == 'noartifact':
    sys.exit(0)
if mode == 'copyfixture':
    artifact.write_bytes(Path(os.environ['FAKE_CLIENT_FIXTURE']).read_bytes())
    sys.exit(0)
width, height = (int(part) for part in value('--capture-size').split('x'))
payload = {'schema': 'luminumbra.render_benchmark.v3',
           'profile': {'name': value('--perf-profile')},
           'width': width, 'height': height,
           'warmup_frames': int(value('--render-benchmark-warmup')),
           'adapter': {'vendor': 'NVIDIA Corporation',
                       'renderer': ('Mesa llvmpipe' if mode == 'wrongadapter'
                                    else 'NVIDIA GeForce RTX 5070 Ti'),
                       'version': '4.6'}}
if value('--render-benchmark-frames') is not None:
    payload['frames'] = int(value('--render-benchmark-frames'))
if mode != 'nop99':
    payload['distribution'] = {'frame_wall_ms': {'p50': 8.0, 'p99': 12.0,
                                                 'sample_count': 240,
                                                 'unavailable_count': 0}}
if mode == 'wrongprofile':
    other = 'performance' if payload['profile']['name'] == 'quality' else 'quality'
    payload['profile']['name'] = other
if mode == 'wrongsize':
    payload['width'] += 1
if mode == 'wrongwarmup':
    payload['warmup_frames'] += 1
if mode == 'wrongframes':
    payload['frames'] += 1
if mode == 'dumpenv':
    payload['saw_lumin_env'] = any(key.startswith(('LUMIN_', 'LUMINUMBRA_'))
                                   for key in os.environ)
    payload['seen'] = {key: os.environ.get(key)
                       for key in ('HOME', 'APPDATA', 'XDG_CONFIG_HOME')}
artifact.write_text(json.dumps(payload), encoding='utf-8')
sys.exit(0)
'''

FAKE_CHECKER = '''#!/usr/bin/env python3
"""Fake checker: echoes the capture into a floor verdict and exits FAKE_CHECKER_EXIT.

FAKE_CHECKER_VERDICT: auto (default), none, malformed, disagree.
FAKE_CHECKER_TAMPER: profile, adapter, frames, kind, p99 (99.0) or p99_low (11.0)
alters that verdict field.
"""
import json
import os
from pathlib import Path
import sys

argv = sys.argv[1:]
code = int(os.environ.get('FAKE_CHECKER_EXIT', '0'))
mode = os.environ.get('FAKE_CHECKER_VERDICT', 'auto')
tamper = os.environ.get('FAKE_CHECKER_TAMPER', '')
path = Path(argv[argv.index('--verdict-out') + 1])
data = json.loads(Path(argv[0]).read_text(encoding='utf-8'))
frames = data.get('frames')
p99 = data.get('distribution', {}).get('frame_wall_ms', {}).get('p99', 0.0)
document = {
    'schema': 'luminumbra.render_verdict.v1',
    'workload_kind': 'fixed_view' if frames is not None else 'traversal',
    'profile': data['profile']['name'],
    'adapter': dict(data['adapter']),
    'frames': frames,
    'floor': {'limit_ms': 16.67, 'p99_ms': p99,
              'verdict': 'pass' if p99 <= 16.67 else 'fail'},
    'target': {'target_ms': 8.33},
}
if code != 0 or mode == 'disagree':
    document['floor']['verdict'] = 'fail'
if tamper == 'profile':
    document['profile'] = 'performance' if document['profile'] == 'quality' else 'quality'
if tamper == 'adapter':
    document['adapter']['renderer'] = 'Mesa llvmpipe'
if tamper == 'frames':
    document['frames'] = (frames or 0) + 1
if tamper == 'p99':
    document['floor']['p99_ms'] = 99.0
if tamper == 'p99_low':
    document['floor']['p99_ms'] = 11.0
if tamper == 'kind':
    document['workload_kind'] = 'fixed_view' if frames is None else 'traversal'
if mode == 'malformed':
    path.write_text('not json', encoding='utf-8')
elif mode != 'none':
    path.write_text(json.dumps(document), encoding='utf-8')
sys.exit(code)
'''


def adapter(renderer=RENDERER):
    return dict(vendor=VENDOR, renderer=renderer, version='4.6')


def verdict(value='pass'):
    return dict(schema=VERDICT_SCHEMA, floor=dict(verdict=value))


def verdict_for(workload_kind, p99=12.0):
    """A complete checker verdict for a quality traversal capture with the given kind."""
    return dict(schema=VERDICT_SCHEMA, workload_kind=workload_kind, profile='quality',
                adapter=adapter(), frames=None,
                floor=dict(limit_ms=rq.render_budget.FLOOR_MS, p99_ms=p99, verdict='pass'),
                target=dict(limit_ms=8.33))


def placeholder_script(workload):
    return f'synthetic/{workload}.traversal'


# Binding for the blocked traversals, used only by tests that need an all-pass path.
PLACEHOLDER_BINDINGS = {workload: placeholder_script(workload)
                        for workload in ('cave-walk', 'edited-world-tour')}


@contextlib.contextmanager
def placeholder_bindings():
    """Bind the blocked traversals to placeholder scripts for the duration of the block."""
    with mock.patch.dict(rq.BOUND_TRAVERSAL_SCRIPTS, PLACEHOLDER_BINDINGS):
        yield


def passing_results(matrix, renderer=RENDERER):
    """Synthetic all-pass results with a complete adapter, artifact and verdict.

    Traversal cells without a recorded script get a placeholder script, so the canonical
    record check sees a script for every traversal workload. Every result came from the
    canonical checker.
    """
    results = []
    for cell in matrix_lib.cells(matrix):
        script = None
        if cell['kind'] == 'traversal':
            script = cell['script'] or placeholder_script(cell['workload'])
        results.append(dict(
            id=cell['id'], workload=cell['workload'], kind=cell['kind'],
            warmup_frames=cell['warmup_frames'], measured_frames=cell['measured_frames'],
            script=script, state='pass', reason=None, adapter=adapter(renderer),
            artifact=f"cells/{cell['id']}.json", verdict=verdict(), checker_canonical=True))
    return results


def index_of(results, cell_id):
    return next(index for index, result in enumerate(results) if result['id'] == cell_id)


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
        base = {'FAKE_CLIENT_MODE': 'ok', 'FAKE_CHECKER_EXIT': '0',
                'FAKE_CHECKER_VERDICT': 'auto'}
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

    def test_dry_run_does_not_touch_existing_aggregate(self):
        self.out.mkdir(parents=True)
        (self.out / 'qualification.json').write_text('keep', encoding='utf-8')
        self.assertEqual(self.run_main(self.argv('--dry-run'))[0], 0)
        self.assertEqual((self.out / 'qualification.json').read_text(encoding='utf-8'),
                         'keep')


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
        self.assertFalse(document['adapter_identical'])  # unevaluated cells have none
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

    def test_relative_paths_resolve_from_caller_cwd(self):
        previous = os.getcwd()
        os.chdir(self.tmp)
        try:
            args = ['--client', './fake_client.py', '--out-dir', 'rel-out',
                    '--matrix', str(self.matrix_path), '--checker', './fake_checker.py',
                    '--only', SURFACE_CELL]
            code, _, _ = self.run_main(args)
        finally:
            os.chdir(previous)
        self.assertEqual(code, 0)
        out = (self.tmp / 'rel-out').resolve()
        document = json.loads((out / 'qualification.json').read_text(encoding='utf-8'))
        cell = self.cell_of(document, SURFACE_CELL)
        self.assertEqual(cell['state'], 'pass')
        self.assertTrue(Path(cell['artifact']).is_absolute())
        self.assertTrue(cell['artifact'].startswith(str(out)), cell['artifact'])
        self.assertFalse(cell['checker_canonical'])
        self.assertFalse(document['render_floor_qualified'])


class ProcessFailureTests(RunnerTestBase):
    def run_surface(self, env, *extra):
        return self.run_main(self.argv('--only', SURFACE_CELL, *extra), env=env)

    def assert_cell(self, state, reason, env, *extra, code=1):
        exit_status, _, _ = self.run_surface(env, *extra)
        self.assertEqual(exit_status, code)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']), (state, reason))
        return cell

    def test_checker_floor_failure_is_fail_and_exit_1(self):
        cell = self.assert_cell('fail', 'floor', {'FAKE_CHECKER_EXIT': '4'})
        self.assertEqual(cell['verdict']['floor']['verdict'], 'fail')

    def test_checker_exit_1_is_invalid(self):
        self.assert_cell('invalid', 'checker exit 1', {'FAKE_CHECKER_EXIT': '1'})

    def test_checker_exit_0_without_verdict_is_invalid(self):
        self.assert_cell('invalid', 'verdict missing/malformed',
                         {'FAKE_CHECKER_VERDICT': 'none'})

    def test_checker_exit_0_with_malformed_verdict_is_invalid(self):
        self.assert_cell('invalid', 'verdict missing/malformed',
                         {'FAKE_CHECKER_VERDICT': 'malformed'})

    def test_checker_exit_0_with_failing_verdict_is_invalid(self):
        self.assert_cell('invalid', 'verdict-disagrees-with-checker',
                         {'FAKE_CHECKER_VERDICT': 'disagree'})

    def test_client_exit_3_is_process_failed(self):
        self.assert_cell('process_failed', 'exit 3', {'FAKE_CLIENT_MODE': 'exit3'})

    def test_client_without_artifact_is_process_failed(self):
        self.assert_cell('process_failed', 'no-artifact', {'FAKE_CLIENT_MODE': 'noartifact'})

    def test_hang_times_out_and_writes_receipt(self):
        self.assert_cell('process_failed', 'timed_out', {'FAKE_CLIENT_MODE': 'hang'},
                         '--timeout-seconds', '1')
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


class CaptureBindingTests(RunnerTestBase):
    def test_wrong_profile_is_refused(self):
        self.run_main(self.argv('--only', SURFACE_CELL),
                      env={'FAKE_CLIENT_MODE': 'wrongprofile'})
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']),
                         ('invalid', 'capture-does-not-match-cell: profile'))

    def test_wrong_size_is_refused(self):
        self.run_main(self.argv('--only', SURFACE_CELL),
                      env={'FAKE_CLIENT_MODE': 'wrongsize'})
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']),
                         ('invalid', 'capture-does-not-match-cell: width'))

    def test_wrong_warmup_is_refused(self):
        self.run_main(self.argv('--only', SURFACE_CELL),
                      env={'FAKE_CLIENT_MODE': 'wrongwarmup'})
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']),
                         ('invalid', 'capture-does-not-match-cell: warmup_frames'))

    def test_wrong_frames_is_refused_for_fixed_view(self):
        self.run_main(self.argv('--only', FOREST_CELL),
                      env={'FAKE_CLIENT_MODE': 'wrongframes'})
        cell = self.cell_of(self.load_qualification(), FOREST_CELL)
        self.assertEqual((cell['state'], cell['reason']),
                         ('invalid', 'capture-does-not-match-cell: frames'))


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

    def test_prior_aggregate_is_moved_aside_before_launch(self):
        self.out.mkdir(parents=True)
        (self.out / 'qualification.json').write_text('{"render_floor_qualified": true}',
                                                     encoding='utf-8')
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        self.assertEqual(
            (self.out / 'qualification.json.prev-1').read_text(encoding='utf-8'),
            '{"render_floor_qualified": true}')
        self.assertFalse(self.load_qualification()['render_floor_qualified'])

    def test_forced_exception_leaves_unqualified_error_aggregate(self):
        self.out.mkdir(parents=True)
        (self.out / 'qualification.json').write_text('old', encoding='utf-8')
        with mock.patch.object(rq, 'run_cells', side_effect=RuntimeError('boom')):
            code, _, err = self.run_main(self.argv('--only', SURFACE_CELL))
        self.assertEqual(code, 1)
        self.assertIn('boom', err)
        document = self.load_qualification()
        self.assertEqual(document['error'], 'boom')
        self.assertFalse(document['render_floor_qualified'])
        self.assertFalse(document['harness_complete'])
        self.assertFalse(document['expanded_qualified'])
        self.assertEqual((self.out / 'qualification.json.prev-1').read_text(encoding='utf-8'),
                         'old')

    def test_lumin_variables_are_not_seen_by_client(self):
        env = {'FAKE_CLIENT_MODE': 'dumpenv', 'LUMIN_GL_DEBUG': '1',
               'LUMINUMBRA_TEST_FLAG': '1'}
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL), env=env)[0], 0)
        artifact = json.loads((self.out / 'cells' / f'{SURFACE_CELL}.json')
                              .read_text(encoding='utf-8'))
        self.assertIs(artifact['saw_lumin_env'], False)

    def test_client_settings_are_redirected_into_out_dir(self):
        env = {'FAKE_CLIENT_MODE': 'dumpenv'}
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL), env=env)[0], 0)
        artifact = json.loads((self.out / 'cells' / f'{SURFACE_CELL}.json')
                              .read_text(encoding='utf-8'))
        settings = (self.out / 'settings').resolve()
        self.assertTrue(settings.is_dir())
        for key in ('HOME', 'APPDATA', 'XDG_CONFIG_HOME'):
            self.assertEqual(Path(artifact['seen'][key]).resolve(), settings, key)


class VerdictReconciliationTests(RunnerTestBase):
    def assert_invalid(self, env, reason, cell_id=SURFACE_CELL):
        code, _, _ = self.run_main(self.argv('--only', cell_id), env=env)
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), cell_id)
        self.assertEqual((cell['state'], cell['reason']), ('invalid', reason))

    def test_pass_verdict_with_p99_over_limit_is_invalid(self):
        self.assert_invalid({'FAKE_CHECKER_TAMPER': 'p99'},
                            'verdict-does-not-match-capture: p99_ms')

    def test_verdict_profile_mismatch_is_invalid(self):
        self.assert_invalid({'FAKE_CHECKER_TAMPER': 'profile'},
                            'verdict-does-not-match-capture: profile')

    def test_verdict_adapter_mismatch_is_invalid(self):
        self.assert_invalid({'FAKE_CHECKER_TAMPER': 'adapter'},
                            'verdict-does-not-match-capture: adapter')

    def test_verdict_frames_mismatch_is_invalid_for_fixed_view(self):
        self.assert_invalid({'FAKE_CHECKER_TAMPER': 'frames'},
                            'verdict-does-not-match-capture: frames', cell_id=FOREST_CELL)

    def test_capture_without_frame_wall_p99_is_invalid(self):
        self.assert_invalid({'FAKE_CLIENT_MODE': 'nop99'},
                            'capture-lacks-frame-wall-p99')

    def test_verdict_p99_differing_from_capture_is_invalid_when_in_range(self):
        # Capture p99 is 12.0; the verdict's 11.0 passes the limit, so only reconciliation
        # can reject it.
        self.assert_invalid({'FAKE_CHECKER_TAMPER': 'p99_low'},
                            'verdict-does-not-match-capture: p99_ms')

    def test_verdict_workload_kind_mismatch_is_invalid(self):
        self.assert_invalid({'FAKE_CHECKER_TAMPER': 'kind'},
                            'verdict-does-not-match-capture: workload_kind')

    def test_capture_workload_kind_mismatch_is_invalid(self):
        verdict_document = verdict_for(workload_kind='traversal')
        capture = dict(profile=dict(name='quality'), adapter=adapter(),
                       frames=None, distribution=dict(frame_wall_ms=dict(p99=12.0)),
                       workload=dict(kind='fixed_view'))
        reason = rq.verdict_reason(verdict_document, capture, dict(kind='traversal'))
        self.assertEqual(reason, 'verdict-does-not-match-capture: workload_kind')

    def test_reconciled_verdict_passes(self):
        code, _, _ = self.run_main(self.argv('--only', SURFACE_CELL))
        self.assertEqual(code, 0)
        document = self.load_qualification()
        cell = self.cell_of(document, SURFACE_CELL)
        self.assertEqual(cell['state'], 'pass')
        self.assertEqual(cell['verdict']['floor']['p99_ms'], 12.0)
        self.assertFalse(document['render_floor_qualified'])


class CheckCommandTests(unittest.TestCase):
    def setUp(self):
        matrix = matrix_lib.load_matrix(MATRIX_PATH)
        for workload in matrix['workloads']:
            if workload['kind'] == 'fixed_view':
                workload['pose'] = dict(POSE)
        self.matrix = matrix
        self.cells = {cell['id']: cell for cell in matrix_lib.cells(matrix)}

    def command(self, cell_id):
        return rq.check_command(self.cells[cell_id], matrix=self.matrix,
                                artifact=Path('/out/cells/x.json'),
                                verdict_path=Path('/out/cells/x.verdict.json'),
                                root=planner.ROOT, python='python3',
                                checker=Path('/chk/validate.py'))

    def test_fixed_view_gets_settled_geometry_camera_and_fov_flags(self):
        command = self.command(FOREST_CELL)
        for flag in ('--require-settled', '--require-geometry', '--require-camera-chunk'):
            self.assertIn(flag, command)
        self.assertEqual(command[command.index('--fov') + 1], '45.0')
        self.assertEqual(command[1], str(Path('/chk/validate.py')))
        self.assertEqual(command[-3:], ['--enforce-floor', '--verdict-out',
                                        str(Path('/out/cells/x.verdict.json'))])

    def test_traversal_gets_no_fixed_view_flags(self):
        command = self.command(SURFACE_CELL)
        for flag in ('--require-settled', '--require-geometry', '--require-camera-chunk',
                     '--fov'):
            self.assertNotIn(flag, command)
        self.assertEqual(command[-3:], ['--enforce-floor', '--verdict-out',
                                        str(Path('/out/cells/x.verdict.json'))])


class SettingsResetTests(RunnerTestBase):
    def test_overlay_from_earlier_run_is_removed(self):
        overlay = self.out / 'settings' / 'overlay.cfg'
        overlay.parent.mkdir(parents=True)
        overlay.write_text('stale', encoding='utf-8')
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        self.assertFalse(overlay.exists())
        self.assertEqual(list((self.out / 'settings').iterdir()), [])


class RealCheckerTests(RunnerTestBase):
    def run_real(self, fixture):
        env = {'FAKE_CLIENT_MODE': 'copyfixture', 'FAKE_CLIENT_FIXTURE': str(fixture)}
        args = self.argv('--only', SURFACE_CELL, checker=False)
        return self.run_main(args, env=env)

    def test_committed_fixture_is_refused_by_capture_binding(self):
        code, _, _ = self.run_real(FIXTURE_PATH)
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual((cell['state'], cell['reason']),
                         ('invalid', 'capture-does-not-match-cell: width'))

    def test_real_checker_refuses_bound_adapter_mismatch_fixture(self):
        data = json.loads(FIXTURE_PATH.read_text(encoding='utf-8'))
        data.update(width=3840, height=1600, warmup_frames=400)
        bound = self.tmp / 'bound-fixture.json'
        bound.write_text(json.dumps(data), encoding='utf-8')
        code, _, _ = self.run_real(bound)
        self.assertEqual(code, 1)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual(cell['state'], 'invalid')
        # Exact reason: argparse usage errors exit 2 and must not satisfy this test.
        self.assertEqual(cell['reason'], 'checker exit 1')
        self.assertTrue(cell['checker_canonical'])
        log = (self.out / 'cells' / f'{SURFACE_CELL}.check.log').read_text(encoding='utf-8')
        self.assertIn('"verdict": "FAIL"', log)
        self.assertNotIn('usage:', log)


class AggregateTests(unittest.TestCase):
    def setUp(self):
        self.matrix = matrix_lib.load_matrix(MATRIX_PATH)
        self.declared = {'3440x1440': 60}

    def aggregate(self, results, partial=False, declared=None, matrix=None):
        return rq.aggregate(matrix or self.matrix, results, partial=partial,
                            declared_refresh=self.declared if declared is None else declared)

    def baseline(self, matrix=None):
        """Fresh unmodified all-pass results; asserts they qualify before a defect is added.

        Call inside placeholder_bindings() so the traversal bindings under test are in force.
        """
        results = passing_results(matrix or self.matrix)
        self.assertTrue(self.aggregate(results, matrix=matrix)['render_floor_qualified'])
        return results

    def test_all_pass_with_declared_refresh_is_qualified(self):
        with placeholder_bindings():
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
        with placeholder_bindings():
            results = self.baseline()
            results[5]['adapter'] = adapter('Mesa llvmpipe')
            document = self.aggregate(results)
        self.assertFalse(document['adapter_identical'])
        self.assertFalse(document['adapter_matches_qualified'])
        self.assertFalse(document['render_floor_qualified'])

    def test_one_cell_adapter_none_breaks_identity_and_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            results[3] = dict(results[3], adapter=None)
            document = self.aggregate(results)
        self.assertFalse(document['adapter_identical'])
        self.assertFalse(document['render_floor_qualified'])

    def test_one_cell_verdict_none_breaks_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            results[4] = dict(results[4], verdict=None)
            document = self.aggregate(results)
        self.assertTrue(document['adapter_identical'])
        self.assertFalse(document['render_floor_qualified'])

    def test_one_cell_artifact_none_breaks_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            results[6] = dict(results[6], artifact=None)
            document = self.aggregate(results)
        self.assertFalse(document['render_floor_qualified'])

    def test_one_cell_verdict_not_pass_breaks_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            results[7] = dict(results[7], verdict=verdict('fail'))
            self.assertFalse(self.aggregate(results)['render_floor_qualified'])

    def test_empty_adapter_field_breaks_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            results[2] = dict(results[2], adapter=dict(vendor='', renderer=RENDERER,
                                                       version='4.6'))
            self.assertFalse(self.aggregate(results)['render_floor_qualified'])

    def test_one_cell_not_pass_breaks_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            results[0] = dict(results[0], state='fail', reason='floor')
            document = self.aggregate(results)
        self.assertEqual(document['summary']['fail'], 1)
        self.assertFalse(document['render_floor_qualified'])
        self.assertTrue(document['harness_complete'])

    def test_invalid_cell_breaks_harness_completeness(self):
        with placeholder_bindings():
            results = self.baseline()
            results[0] = dict(results[0], state='invalid', reason='checker exit 1')
            document = self.aggregate(results)
        self.assertFalse(document['harness_complete'])
        self.assertFalse(document['render_floor_qualified'])

    def test_undeclared_display_breaks_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            document = self.aggregate(results, declared={})
        self.assertEqual(document['displays'][1]['refresh_source'], 'undeclared')
        self.assertIsNone(document['displays'][1]['refresh_hz'])
        self.assertFalse(document['render_floor_qualified'])

    def test_partial_run_is_never_qualified(self):
        with placeholder_bindings():
            results = self.baseline()
            document = self.aggregate(results, partial=True)
        self.assertTrue(document['partial'])
        self.assertFalse(document['harness_complete'])
        self.assertFalse(document['render_floor_qualified'])

    def test_missing_canonical_cell_breaks_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            document = self.aggregate(results[1:])
        self.assertEqual(document['summary']['pass'], 35)
        self.assertFalse(document['render_floor_qualified'])

    def test_unevaluated_cells_allowed_for_harness_but_not_qualification(self):
        with placeholder_bindings():
            results = self.baseline()
            results[1] = dict(results[1], state='unevaluated', reason='pose-unrecorded',
                              adapter=None, artifact=None, verdict=None)
            document = self.aggregate(results)
        self.assertTrue(document['harness_complete'])
        self.assertFalse(document['render_floor_qualified'])

    def test_two_cell_matrix_never_qualifies(self):
        small = copy.deepcopy(self.matrix)
        small['workloads'] = [w for w in small['workloads'] if w['id'] == 'surface-flight']
        small['displays'] = small['displays'][:1]
        with placeholder_bindings():
            self.baseline()
            document = self.aggregate(passing_results(small), matrix=small)
        self.assertEqual(document['summary']['pass'], 2)
        self.assertTrue(document['harness_complete'])
        self.assertFalse(document['render_floor_qualified'])

    def test_non_canonical_qualified_renderer_never_qualifies(self):
        with placeholder_bindings():
            self.baseline()
            matrix = copy.deepcopy(self.matrix)
            matrix['qualified_renderer'] = 'Mesa llvmpipe'
            document = self.aggregate(passing_results(matrix, renderer='Mesa llvmpipe'),
                                      matrix=matrix)
        self.assertEqual(document['summary']['pass'], 36)
        self.assertTrue(document['adapter_identical'])
        self.assertFalse(document['render_floor_qualified'])

    def test_fixed_view_with_short_warmup_and_frames_never_qualifies(self):
        with placeholder_bindings():
            results = self.baseline()
            index = index_of(results, FOREST_CELL)
            results[index] = dict(results[index], warmup_frames=2, measured_frames=4)
            document = self.aggregate(results)
        self.assertEqual(document['summary']['pass'], 36)
        self.assertFalse(document['render_floor_qualified'])

    def test_cave_walk_recorded_as_fixed_view_never_qualifies(self):
        with placeholder_bindings():
            results = self.baseline()
            index = index_of(results, 'cave-walk.quality.3840x1600')
            results[index] = dict(results[index], kind='fixed_view', measured_frames=240,
                                  script=None)
            self.assertFalse(self.aggregate(results)['render_floor_qualified'])

    def test_surface_flight_with_other_script_never_qualifies(self):
        with placeholder_bindings():
            results = self.baseline()
            index = index_of(results, SURFACE_CELL)
            results[index] = dict(results[index],
                                  script='tools/perf/fixtures/other.traversal')
            self.assertFalse(self.aggregate(results)['render_floor_qualified'])

    def test_traversals_carrying_surface_script_never_qualify(self):
        # Baseline binds cave-walk and edited-world-tour to placeholders; the surface script
        # then disagrees with those bindings, and that alone must block qualification.
        with placeholder_bindings():
            results = self.baseline()
            for result in results:
                if result['workload'] in ('cave-walk', 'edited-world-tour'):
                    result['script'] = rq.CANONICAL_SURFACE_SCRIPT
            document = self.aggregate(results)
        self.assertEqual(document['summary']['pass'], 36)
        self.assertFalse(document['render_floor_qualified'])

    def test_unbound_traversals_never_qualify(self):
        # Without placeholder bindings cave-walk and edited-world-tour have no canonical script.
        document = self.aggregate(passing_results(self.matrix))
        self.assertEqual(document['summary']['pass'], 36)
        self.assertFalse(document['render_floor_qualified'])

    def test_traversal_records_rebound_to_surface_flight_never_qualify(self):
        # Ids still name cave-walk / edited-world-tour, so the record is not bound to its id.
        with placeholder_bindings():
            results = self.baseline()
            for result in results:
                if result['workload'] in ('cave-walk', 'edited-world-tour'):
                    result['workload'] = 'surface-flight'
                    result['script'] = rq.CANONICAL_SURFACE_SCRIPT
            document = self.aggregate(results)
        self.assertEqual(document['summary']['pass'], 36)
        self.assertFalse(document['render_floor_qualified'])

    def test_result_from_replacement_checker_never_qualifies(self):
        with placeholder_bindings():
            results = self.baseline()
            results[0] = dict(results[0], checker_canonical=False)
            document = self.aggregate(results)
        self.assertFalse(document['render_floor_qualified'])
        self.assertFalse(self.cell_of_document(document, results[0]['id'])['checker_canonical'])

    def test_cells_carry_canonical_fields(self):
        document = self.aggregate(passing_results(self.matrix))
        cell = self.cell_of_document(document, FOREST_CELL)
        self.assertEqual((cell['kind'], cell['warmup_frames'], cell['measured_frames']),
                         ('fixed_view', 400, 240))
        cell = self.cell_of_document(document, SURFACE_CELL)
        self.assertEqual((cell['kind'], cell['script']),
                         ('traversal', 'tools/perf/fixtures/surface-flight.traversal'))

    def cell_of_document(self, document, cell_id):
        return next(cell for cell in document['cells'] if cell['id'] == cell_id)

    def test_matrix_refresh_is_not_overridden(self):
        matrix = copy.deepcopy(self.matrix)
        matrix['displays'][0]['refresh_hz'] = 100
        document = rq.aggregate(matrix, passing_results(matrix), partial=False,
                                declared_refresh={})
        self.assertEqual(document['displays'][0]['refresh_hz'], 100)
        self.assertEqual(document['displays'][0]['refresh_source'], 'matrix')


if __name__ == '__main__':
    unittest.main()
