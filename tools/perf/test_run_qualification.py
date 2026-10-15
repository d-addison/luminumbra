#!/usr/bin/env python3
"""Tests for the qualification execution runner, its aggregate and exit codes.

The client and checker are fake scripts written into a temporary directory, so no
GPU is needed. Some tests run the real validate_render_capture.py checker.
"""
import contextlib
import copy
import io
import json
import math
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import plan_qualification as planner
import qualification_matrix as matrix_lib
import run_qualification as rq
import validate_render_capture as capture_checker
from check_gl_debug_log import MAX_FILE_BYTES

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
if mode == 'selfchange':
    with Path(__file__).open('a', encoding='utf-8') as source:
        source.write('\\n# changed during run\\n')
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
    return {'vendor': VENDOR, 'renderer': renderer, 'version': '4.6'}


def verdict(value='pass'):
    return {'schema': VERDICT_SCHEMA, 'floor': {'verdict': value}}


def verdict_for(workload_kind, p99=12.0):
    """A complete checker verdict for a quality traversal capture with the given kind."""
    return {'schema': VERDICT_SCHEMA, 'workload_kind': workload_kind, 'profile': 'quality',
                'adapter': adapter(), 'frames': None,
                'floor': {'limit_ms': rq.render_budget.FLOOR_MS, 'p99_ms': p99, 'verdict': 'pass'},
                'target': {'limit_ms': 8.33}}


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
        results.append({
            'id': cell['id'], 'workload': cell['workload'], 'kind': cell['kind'],
            'warmup_frames': cell['warmup_frames'], 'measured_frames': cell['measured_frames'],
            'script': script, 'state': 'pass', 'reason': None, 'adapter': adapter(renderer),
            'artifact': f"cells/{cell['id']}.json", 'verdict': verdict(), 'checker_canonical': True,
            'matrix_canonical': True,
            'client': {'kind': 'native', 'native_build': True, 'sha256_verified': True,
                        'sha256': 'a' * 64, 'build_manifest_client_sha256': 'a' * 64,
                        'reason': None}})
    return results


def index_of(results, cell_id):
    return next(index for index, result in enumerate(results) if result['id'] == cell_id)


class RunnerTestBase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        # Match the runner's canonical output paths when the temporary directory
        # uses a Windows short name or a macOS directory alias.
        self.tmp = Path(self.directory.name).resolve()
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

    def write_build_manifest(self, client):
        configuration = {key: 'OFF' for key in rq.perf.BUILD_CONFIGURATION_FIELDS}
        configuration.update(build_type='Release', effective_flags_sha256='a' * 64)
        document = {'schema': rq.BUILD_SCHEMA,
                        'compiler': {'id': 'GNU', 'version': '14.2', 'target': 'x86_64'},
                        'configuration': configuration,
                        'system': {'name': 'fixture', 'processor': 'x86_64'},
                        'client': {'sha256': rq.runner.file_sha256(client)}}
        path = self.tmp / 'performance-build.json'
        path.write_text(json.dumps(document), encoding='utf-8')
        return path


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
    def test_client_and_checker_do_not_leak_resolved_windows_prefixes(self):
        windows = types.SimpleNamespace(**vars(os))
        windows.name = 'nt'
        for resolver, path in ((rq.resolve_client, self.fake_client),
                               (rq.resolve_checker, self.fake_checker)):
            for parent in (r'C:\qualification', r'UNC\server\share\qualification'):
                resolved = PureWindowsPath('\\\\?\\' + parent) / path.name
                expected = (PureWindowsPath(r'C:\qualification') / path.name
                            if parent.startswith('C:') else
                            PureWindowsPath(r'\\server\share\qualification') / path.name)
                with self.subTest(resolver=resolver.__name__, path=resolved), \
                        mock.patch.object(rq.runner, 'os', windows), \
                        mock.patch.object(Path, 'resolve', return_value=resolved):
                    self.assertEqual(resolver(str(path)), str(expected))

    def test_extended_windows_spelling_preserves_canonical_matrix_identity(self):
        matrix = matrix_lib.load_matrix(MATRIX_PATH)
        windows = types.SimpleNamespace(**vars(os))
        windows.name = 'nt'
        ordinary = PureWindowsPath(r'C:\qualification\matrix.json')
        extended = PureWindowsPath(r'\\?\C:\qualification\matrix.json')
        with mock.patch.object(rq.runner, 'os', windows), \
                mock.patch.object(Path, 'resolve', side_effect=[extended, ordinary]):
            self.assertTrue(rq.matrix_identity(matrix, self.matrix_path)['canonical'])

    def test_bare_python_filename_resolves_from_caller_cwd(self):
        previous = os.getcwd()
        os.chdir(self.tmp)
        try:
            args = self.argv('--only', SURFACE_CELL)
            args[1] = 'fake_client.py'
            code, _, _ = self.run_main(args)
        finally:
            os.chdir(previous)
        self.assertEqual(code, 0)
        cell = self.cell_of(self.load_qualification(), SURFACE_CELL)
        self.assertEqual(cell['state'], 'pass')
        receipt = rq.read_json_object(self.out / 'cells' / f'{SURFACE_CELL}.process.json')
        self.assertEqual(receipt['command'][1], str(self.fake_client.resolve()))

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

    def test_out_dir_file_is_not_reported_as_held_lock(self):
        self.out.write_text('not a directory', encoding='utf-8')
        with mock.patch.object(rq, 'run_cells') as launch:
            code, _, err = self.run_main(self.argv('--only', SURFACE_CELL))
        self.assertEqual(code, 2, err)
        self.assertIn('cannot write', err)
        self.assertNotIn('output directory locked', err)
        launch.assert_not_called()
        self.assertEqual(self.out.read_text(encoding='utf-8'), 'not a directory')

    def test_mkdir_file_exists_error_is_not_reported_as_held_lock(self):
        # Windows can report an obstructing parent file as FileExistsError.
        with mock.patch.object(Path, 'mkdir', side_effect=FileExistsError('blocked parent')), \
                mock.patch.object(rq.os, 'open') as open_file, \
                mock.patch.object(rq, 'run_cells') as launch:
            code, _, err = self.run_main(self.argv('--only', SURFACE_CELL))
        self.assertEqual(code, 2, err)
        self.assertIn('cannot write', err)
        self.assertIn('blocked parent', err)
        self.assertNotIn('output directory locked', err)
        open_file.assert_not_called()
        launch.assert_not_called()
        self.assertFalse(self.out.exists())

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
        self.assertEqual(cell['artifact'], f'cells/{SURFACE_CELL}.json')
        self.assertTrue((out / cell['artifact']).is_file())
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
        self.assert_cell('process_failed', 'capture evidence: capture not retained',
                         {'FAKE_CLIENT_MODE': 'noartifact'})

    def test_hang_times_out_and_writes_receipt(self):
        self.assert_cell('process_failed', 'timed_out', {'FAKE_CLIENT_MODE': 'hang'},
                         '--timeout-seconds', '1')
        receipt = json.loads((self.out / 'cells' / f'{SURFACE_CELL}.process.json')
                             .read_text(encoding='utf-8'))
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['timeout_seconds'], 1)

    def test_injected_timeout_writes_receipt(self):
        def timeout(command, **kwargs):
            raise subprocess.TimeoutExpired(command, kwargs['timeout'])

        with mock.patch.object(rq.runner.subprocess, 'run', side_effect=timeout):
            self.assert_cell('process_failed', 'timed_out', {}, '--timeout-seconds', '1')
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
    def test_unready_cell_retires_capture_verdict_and_receipt(self):
        # Reuse the same output directory after a passing pose-recorded run.
        self.assertEqual(self.run_main(self.argv('--only', FOREST_CELL))[0], 0)
        args = self.argv('--only', FOREST_CELL, matrix=MATRIX_PATH)
        self.assertEqual(self.run_main(args)[0], 3)
        cell = self.cell_of(self.load_qualification(), FOREST_CELL)
        self.assertEqual(cell['state'], 'unevaluated')
        for key in ('artifact', 'adapter', 'verdict', 'client'):
            self.assertIsNone(cell[key])
        for suffix in ('.json', '.verdict.json', '.artifact.json', '.process.json'):
            path = self.out / 'cells' / (FOREST_CELL + suffix)
            self.assertFalse(path.exists())
            self.assertTrue(path.with_name(path.name + '.prev-1').is_file())

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
        self.assertFalse((self.out / rq.LOCK_NAME).exists())
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
        capture = {'profile': {'name': 'quality'}, 'adapter': adapter(),
                       'frames': None, 'distribution': {'frame_wall_ms': {'p99': 12.0}},
                       'workload': {'kind': 'fixed_view'}}
        reason = rq.verdict_reason(verdict_document, capture, {'kind': 'traversal'})
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
        self.assert_cell_expectations(command, self.cells[FOREST_CELL])

    def test_traversal_gets_no_fixed_view_flags(self):
        command = self.command(SURFACE_CELL)
        for flag in ('--require-settled', '--require-geometry', '--require-camera-chunk',
                     '--fov'):
            self.assertNotIn(flag, command)
        self.assert_cell_expectations(command, self.cells[SURFACE_CELL])

    def assert_cell_expectations(self, command, cell):
        expected = matrix_lib.checker_command(
            cell, artifact=Path('/out/cells/x.json'),
            verdict=Path('/out/cells/x.verdict.json'),
            qualified_renderer=self.matrix['qualified_renderer'],
            root=planner.ROOT, python='python3')
        expected[1] = str(Path('/chk/validate.py'))
        self.assertEqual(command[:len(expected)], expected)
        for option in ('--enforce-floor', '--verdict-out', '--profile', '--workload-kind'):
            self.assertEqual(command.count(option), 1)
        self.assertEqual(command[command.index('--verdict-out') + 1],
                         str(Path('/out/cells/x.verdict.json')))
        for name, value in (('width', cell['width']), ('height', cell['height']),
                            ('warmup-frames', cell['warmup_frames'])):
            self.assertEqual(sum(arg.startswith(f'--{name}=') for arg in command), 1)
            self.assertIn(f'--{name}={value}', command)
        if cell['kind'] == 'fixed_view':
            self.assertEqual(sum(arg.startswith('--frames=') for arg in command), 1)
            self.assertIn(f"--frames={cell['measured_frames']}", command)
        else:
            self.assertFalse(any(arg.startswith('--frames') for arg in command))


class SettingsResetTests(RunnerTestBase):
    def test_overlay_from_earlier_run_is_removed(self):
        overlay = self.out / 'settings' / 'overlay.cfg'
        overlay.parent.mkdir(parents=True)
        overlay.write_text('stale', encoding='utf-8')
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        self.assertFalse(overlay.exists())
        self.assertEqual(list((self.out / 'settings').iterdir()), [])


class DirectoryLockTests(RunnerTestBase):
    def test_held_lock_refuses_without_touching_any_run_files(self):
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        lock = self.out / rq.LOCK_NAME
        lock.write_text('held by another worker', encoding='utf-8')
        before = {path: path.read_bytes() for path in self.out.rglob('*') if path.is_file()}
        with mock.patch.object(rq, 'run_cells') as launch:
            code, _, err = self.run_main(self.argv('--only', SURFACE_CELL,
                                                   out=self.out / '..' / self.out.name))
        self.assertEqual(code, 2)
        launch.assert_not_called()
        self.assertIn(str(lock), err)
        self.assertIn('no automatic stale-lock reclaim', err)
        self.assertEqual(before, {path: path.read_bytes() for path in self.out.rglob('*')
                                  if path.is_file()})

    def test_lock_covers_client_checker_and_atomic_publication(self):
        original_run = rq.runner.run_process
        original_write = rq.runner.atomic_write
        observed = []

        def run_process(*args, **kwargs):
            self.assertTrue((self.out / rq.LOCK_NAME).is_file())
            code, _, _ = self.run_main(self.argv('--only', SURFACE_CELL))
            self.assertEqual(code, 2)
            observed.append('client')
            return original_run(*args, **kwargs)

        def atomic_write(path, payload):
            self.assertTrue((self.out / rq.LOCK_NAME).is_file())
            if Path(path).name == rq.QUALIFICATION_NAME:
                observed.append('aggregate')
            return original_write(path, payload)

        with mock.patch.object(rq.runner, 'run_process', side_effect=run_process), \
                mock.patch.object(rq.runner, 'atomic_write', side_effect=atomic_write):
            self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        self.assertEqual(observed, ['client', 'aggregate'])
        self.assertFalse((self.out / rq.LOCK_NAME).exists())
        self.assertEqual(list(self.out.glob('qualification.json.*.tmp')), [])

    def test_lock_released_after_publication_failure(self):
        original = rq.runner.atomic_write

        def fail_aggregate(path, payload):
            if Path(path).name == rq.QUALIFICATION_NAME:
                raise OSError('publication failed')
            return original(path, payload)

        with mock.patch.object(rq.runner, 'atomic_write', side_effect=fail_aggregate):
            code, _, err = self.run_main(self.argv('--only', SURFACE_CELL))
        self.assertEqual(code, 2)
        self.assertIn('publication failed', err)
        self.assertFalse((self.out / rq.LOCK_NAME).exists())
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)


class HostEvidenceTests(RunnerTestBase):
    def setUp(self):
        super().setUp()
        self.host = {'os_caption': 'Windows', 'os_version': '10.0',
                     'powershell': '7.4',
                     'gpus': [{'Name': RENDERER, 'DriverVersion': '1.2'}]}
        self.staged_host = self.tmp / 'staged-host.json'
        # Windows PowerShell 5.1 Set-Content emits a UTF-8 BOM.
        self.staged_host.write_text(json.dumps(self.host), encoding='utf-8-sig')
        self.out.mkdir()
        self.previous_qualification = b'previous qualification\n'
        self.previous_host = b'previous host\n'
        (self.out / rq.QUALIFICATION_NAME).write_bytes(self.previous_qualification)
        (self.out / 'host.json').write_bytes(self.previous_host)

    def host_argv(self, *extra, **kwargs):
        return self.argv('--host-record', str(self.staged_host), *extra, **kwargs)

    def assert_previous_pair(self):
        self.assertEqual((self.out / rq.QUALIFICATION_NAME).read_bytes(),
                         self.previous_qualification)
        self.assertEqual((self.out / 'host.json').read_bytes(), self.previous_host)
        self.assertEqual(list(self.out.glob('*.prev-*')), [])

    def assert_current_host(self):
        self.assertEqual(rq.read_json_object(self.out / 'host.json'), self.host)
        self.assertTrue(self.staged_host.exists())
        self.assertFalse((self.out / rq.LOCK_NAME).exists())

    def test_host_published_for_pass_fail_and_unevaluated_exits(self):
        cases = [(0, self.matrix_path, SURFACE_CELL, {}),
                 (1, self.matrix_path, SURFACE_CELL, {'FAKE_CHECKER_EXIT': '4'}),
                 (3, MATRIX_PATH, FOREST_CELL, {})]
        for expected, matrix, cell, env in cases:
            with self.subTest(exit_code=expected):
                (self.out / 'host.json').write_bytes(self.previous_host)
                code, _, err = self.run_main(self.host_argv('--only', cell, matrix=matrix),
                                            env=env)
                self.assertEqual(code, expected, err)
                self.assert_current_host()
                self.assertEqual(rq.exit_code(self.load_qualification()['summary']), expected)

    def test_host_published_with_fail_closed_error_record(self):
        with mock.patch.object(rq, 'run_cells', side_effect=RuntimeError('boom')):
            code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
        self.assertEqual(code, 1, err)
        self.assertEqual(self.load_qualification()['error'], 'boom')
        self.assert_current_host()

    def test_missing_or_invalid_host_refuses_without_changing_pair(self):
        documents = [None, [], {}, dict(self.host, os_version=''),
                     dict(self.host, powershell=7), dict(self.host, gpus=[]),
                     dict(self.host, gpus=[{'Name': RENDERER}]),
                     dict(self.host, gpus=[None]), dict(self.host, extra=float('nan'))]
        for document in documents:
            with self.subTest(document=document):
                self.staged_host.write_text(json.dumps(document), encoding='utf-8')
                with mock.patch.object(rq, 'run_cells') as launch:
                    code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
                self.assertEqual(code, 2, err)
                self.assertIn('--host-record', err)
                launch.assert_not_called()
                self.assert_previous_pair()
                self.assertFalse((self.out / rq.LOCK_NAME).exists())
        for content in (b'not JSON', b'\xff'):
            self.staged_host.write_bytes(content)
            self.assertEqual(self.run_main(self.host_argv('--only', SURFACE_CELL))[0], 2)
            self.assert_previous_pair()
        self.staged_host.unlink()
        self.assertEqual(self.run_main(self.host_argv('--only', SURFACE_CELL))[0], 2)
        self.assert_previous_pair()

    def test_preflight_and_lock_refusals_preserve_pair(self):
        for extra in (('--only', 'unknown-cell'), ('--timeout-seconds', '0'),
                      ('--build-manifest', str(self.tmp / 'absent.json'))):
            with self.subTest(extra=extra):
                self.assertEqual(self.run_main(self.host_argv(*extra))[0], 2)
                self.assert_previous_pair()
        lock = self.out / rq.LOCK_NAME
        lock.write_text('held', encoding='utf-8')
        with mock.patch.object(rq, 'run_cells') as launch:
            self.assertEqual(self.run_main(self.host_argv('--only', SURFACE_CELL))[0], 2)
        launch.assert_not_called()
        self.assert_previous_pair()
        self.assertEqual(lock.read_text(encoding='utf-8'), 'held')

    def test_dry_run_preserves_pair_and_publishes_separate_host(self):
        with mock.patch.object(rq, 'run_cells') as launch:
            self.assertEqual(self.run_main(self.host_argv('--dry-run'))[0], 0)
        launch.assert_not_called()
        self.assert_previous_pair()
        self.assertEqual(rq.read_json_object(self.out / 'host-dryrun.json'), self.host)

    def test_both_records_publish_under_lock_and_competing_run_refuses(self):
        original_write = rq.runner.atomic_write
        observed = []

        def write(path, payload):
            if path.name in ('host.json', rq.QUALIFICATION_NAME):
                self.assertTrue((self.out / rq.LOCK_NAME).is_file())
                before = {p.name: p.read_bytes() for p in self.out.iterdir() if p.is_file()}
                with mock.patch.object(rq, 'run_cells') as launch:
                    code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
                self.assertEqual(code, 2, err)
                launch.assert_not_called()
                self.assertEqual(before, {p.name: p.read_bytes() for p in self.out.iterdir()
                                          if p.is_file()})
                observed.append(path.name)
            return original_write(path, payload)

        for fail_closed in (False, True):
            with self.subTest(fail_closed=fail_closed):
                observed.clear()
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(rq.runner, 'atomic_write',
                                                           side_effect=write))
                    if fail_closed:
                        stack.enter_context(mock.patch.object(rq, 'run_cells',
                                                               side_effect=RuntimeError('boom')))
                    code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
                self.assertEqual(code, 1 if fail_closed else 0, err)
                self.assertEqual(observed, ['host.json', rq.QUALIFICATION_NAME])
                self.assert_current_host()
                self.assertEqual(list(self.out.glob('*.tmp')), [])

    def test_execution_uses_validated_host_snapshot(self):
        original_run = rq.run_cells

        def run(*args, **kwargs):
            self.staged_host.write_text('changed after validation', encoding='utf-8')
            return original_run(*args, **kwargs)

        with mock.patch.object(rq, 'run_cells', side_effect=run):
            self.assertEqual(self.run_main(self.host_argv('--only', SURFACE_CELL))[0], 0)
        self.assert_current_host()

    def test_publication_failures_do_not_leave_new_host_without_aggregate(self):
        original_write = rq.runner.atomic_write
        for failing in ('host.json', rq.QUALIFICATION_NAME):
            for prior_host in (False, True):
                with self.subTest(failing=failing, prior_host=prior_host):
                    host_path = self.out / 'host.json'
                    if prior_host:
                        host_path.write_bytes(self.previous_host)
                    else:
                        host_path.unlink(missing_ok=True)

                    def write(path, payload):
                        self.assertTrue((self.out / rq.LOCK_NAME).is_file())
                        if path.name == failing:
                            raise OSError('publication failed')
                        return original_write(path, payload)

                    with mock.patch.object(rq.runner, 'atomic_write', side_effect=write):
                        code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
                    self.assertEqual(code, 2, err)
                    self.assertIn('publication failed', err)
                    self.assertFalse((self.out / rq.LOCK_NAME).exists())
                    self.assertFalse((self.out / rq.QUALIFICATION_NAME).exists())
                    if prior_host:
                        self.assertEqual(host_path.read_bytes(), self.previous_host)
                    else:
                        self.assertFalse(host_path.exists())

    def test_hostless_publications_retire_host_under_lock(self):
        original_write = rq.runner.atomic_write
        host_path = self.out / 'host.json'
        observed = []

        def write(path, payload):
            if path.name == rq.QUALIFICATION_NAME:
                self.assertTrue((self.out / rq.LOCK_NAME).is_file())
                self.assertFalse(host_path.exists())
                with mock.patch.object(rq, 'run_cells') as launch:
                    self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 2)
                launch.assert_not_called()
                observed.append(path.name)
            return original_write(path, payload)

        cases = [(0, self.matrix_path, SURFACE_CELL, {}, False),
                 (1, self.matrix_path, SURFACE_CELL, {'FAKE_CHECKER_EXIT': '4'}, False),
                 (3, MATRIX_PATH, FOREST_CELL, {}, False),
                 (1, self.matrix_path, SURFACE_CELL, {}, True)]
        for expected, matrix, cell, env, fail_closed in cases:
            with self.subTest(exit_code=expected, fail_closed=fail_closed):
                host_path.write_bytes(self.previous_host)
                observed.clear()
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(rq.runner, 'atomic_write',
                                                           side_effect=write))
                    if fail_closed:
                        stack.enter_context(mock.patch.object(rq, 'run_cells',
                                                               side_effect=RuntimeError('boom')))
                    code, _, err = self.run_main(self.argv('--only', cell, matrix=matrix), env)
                self.assertEqual(code, expected, err)
                self.assertEqual(observed, [rq.QUALIFICATION_NAME])
                self.assertFalse(host_path.exists())
                self.assertFalse((self.out / rq.LOCK_NAME).exists())

    def test_hostless_publication_failure_restores_host_under_lock(self):
        original_write = rq.runner.atomic_write
        host_path = self.out / 'host.json'
        observed = []

        def write(path, payload):
            self.assertTrue((self.out / rq.LOCK_NAME).is_file())
            if path.name == rq.QUALIFICATION_NAME:
                self.assertFalse(host_path.exists())
                observed.append('aggregate')
                raise OSError('publication failed')
            if path == host_path:
                self.assertFalse(host_path.exists())
                self.assertEqual(payload, self.previous_host)
                observed.append('restore')
            result = original_write(path, payload)
            if path == host_path:
                self.assertTrue((self.out / rq.LOCK_NAME).is_file())
                self.assertEqual(host_path.read_bytes(), self.previous_host)
                with mock.patch.object(rq, 'run_cells') as launch:
                    code, _, err = self.run_main(self.argv('--only', SURFACE_CELL))
                self.assertEqual(code, 2, err)
                launch.assert_not_called()
            return result

        with mock.patch.object(rq.runner, 'atomic_write', side_effect=write):
            code, _, err = self.run_main(self.argv('--only', SURFACE_CELL,
                                                   out=self.out / '..' / self.out.name))
        self.assertEqual(code, 2, err)
        self.assertEqual(observed, ['aggregate', 'restore'])
        self.assertEqual(host_path.read_bytes(), self.previous_host)
        self.assertFalse((self.out / rq.LOCK_NAME).exists())

    def test_deep_host_json_refuses_before_decode_or_execution(self):
        prefix = json.dumps(self.host)[:-1] + ', "extra": '
        for opening, closing in (('[', ']'), ('{"nested":', '}')):
            with self.subTest(opening=opening):
                self.staged_host.write_text(prefix + opening * 2000 + '0'
                                            + closing * 2000 + '}', encoding='utf-8')
                original_loads = rq.json.loads

                def loads(payload, *args, **kwargs):
                    self.assertNotIn('"extra"', str(payload))
                    return original_loads(payload, *args, **kwargs)

                with mock.patch.object(rq.json, 'loads', side_effect=loads), \
                        mock.patch.object(rq, 'run_cells') as launch:
                    code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
                self.assertEqual(code, 2, err)
                self.assertIn('depth', err)
                launch.assert_not_called()
                self.assert_previous_pair()
                self.assertFalse((self.out / rq.LOCK_NAME).exists())

    def test_oversized_staged_host_refuses_without_modifying_pair(self):
        content = json.dumps(self.host).encode('utf-8')
        self.staged_host.write_bytes(content + b' ' * (MAX_FILE_BYTES + 1 - len(content)))
        with mock.patch.object(rq, 'run_cells') as launch:
            code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
        self.assertEqual(code, 2, err)
        self.assertIn('byte cap', err)
        launch.assert_not_called()
        self.assert_previous_pair()

    def test_host_json_depth_boundary_and_escaped_brackets(self):
        prefix = json.dumps(self.host)[:-1] + ', "extra": '
        for depth in (rq.MAX_HOST_JSON_DEPTH, rq.MAX_HOST_JSON_DEPTH + 1):
            with self.subTest(depth=depth):
                # The host object itself accounts for one container level.
                self.staged_host.write_text(prefix + '[' * (depth - 1) + '0'
                                            + ']' * (depth - 1) + '}', encoding='utf-8')
                if depth == rq.MAX_HOST_JSON_DEPTH:
                    self.assertIsNotNone(rq.load_host_record(self.staged_host))
                else:
                    with self.assertRaisesRegex(rq.UsageError, 'depth'):
                        rq.load_host_record(self.staged_host)
        note = ('[{' * 2000) + '"escaped quote" \\ ' + ('}]' * 2000)
        self.host['extra'] = note
        self.staged_host.write_text(json.dumps(self.host), encoding='utf-8-sig')
        self.assertEqual(json.loads(rq.load_host_record(self.staged_host)), self.host)

    def test_host_reads_accept_exact_byte_cap(self):
        content = json.dumps(self.host).encode('utf-8')
        payload = content + b' ' * (MAX_FILE_BYTES - len(content))
        self.staged_host.write_bytes(payload)
        self.assertEqual(json.loads(rq.load_host_record(self.staged_host)), self.host)
        host_path = self.out / 'host.json'
        host_path.write_bytes(payload)
        code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
        self.assertEqual(code, 0, err)
        self.assert_current_host()

    def test_non_ascii_host_near_cap_publishes_and_round_trips(self):
        self.host['extra'] = ''
        empty = json.dumps(self.host, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        self.host['extra'] = 'é' * ((MAX_FILE_BYTES - len(empty) - 1) // 2)
        content = json.dumps(self.host, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        self.staged_host.write_bytes(content)
        self.assertLessEqual(len(content), MAX_FILE_BYTES)
        self.assertGreaterEqual(len(content), MAX_FILE_BYTES - 2)
        code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
        self.assertEqual(code, 0, err)
        host_path = self.out / 'host.json'
        published = host_path.read_bytes()
        self.assertLessEqual(len(published), MAX_FILE_BYTES)
        self.assertIn('é'.encode('utf-8'), published)
        self.assert_current_host()
        # Use the published record as both staged input and the previous host.
        code, _, err = self.run_main(self.argv('--host-record', str(host_path),
                                               '--only', SURFACE_CELL))
        self.assertEqual(code, 0, err)
        self.assert_current_host()
        self.assertEqual(host_path.read_bytes(), published)
        code, _, err = self.run_main(self.argv('--only', SURFACE_CELL))
        self.assertEqual(code, 0, err)
        self.assertFalse(host_path.exists())

    def test_serialized_host_over_cap_refuses_before_execution(self):
        self.host['extra'] = ''
        empty = json.dumps(self.host, separators=(',', ':')).encode('utf-8')
        self.host['extra'] = 'x' * (MAX_FILE_BYTES - len(empty))
        content = json.dumps(self.host, separators=(',', ':')).encode('utf-8')
        self.assertEqual(len(content), MAX_FILE_BYTES)
        self.staged_host.write_bytes(content)
        # The terminating newline makes the published record exceed the cap.
        with mock.patch.object(rq, 'run_cells') as launch:
            code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
        self.assertEqual(code, 2, err)
        self.assertIn('byte cap', err)
        launch.assert_not_called()
        self.assert_previous_pair()
        self.assertFalse((self.out / rq.LOCK_NAME).exists())

    def test_host_growth_after_stat_is_bounded(self):
        original_fstat = os.fstat
        for candidate in (self.staged_host, self.out / 'host.json'):
            with self.subTest(candidate=candidate.name):
                candidate.write_text(json.dumps(self.host), encoding='utf-8')
                identity = candidate.stat()

                def fstat(descriptor):
                    info = original_fstat(descriptor)
                    if (info.st_dev, info.st_ino) == (identity.st_dev, identity.st_ino):
                        with candidate.open('ab') as output:
                            output.write(b' ' * (MAX_FILE_BYTES + 1 - info.st_size))
                    return info

                with mock.patch.object(os, 'fstat', side_effect=fstat), \
                        mock.patch.object(rq, 'run_cells') as launch:
                    code, _, err = self.run_main(self.host_argv('--only', SURFACE_CELL))
                self.assertEqual(code, 2, err)
                self.assertIn('byte cap', err)
                launch.assert_not_called()
                self.assertEqual((self.out / rq.QUALIFICATION_NAME).read_bytes(),
                                 self.previous_qualification)
                self.assertFalse((self.out / rq.LOCK_NAME).exists())
                candidate.write_text(json.dumps(self.host), encoding='utf-8')

    def test_oversized_previous_host_refuses_hostful_and_hostless_runs(self):
        host_path = self.out / 'host.json'
        with host_path.open('wb') as output:
            output.truncate(MAX_FILE_BYTES + 1)
        for hostful in (False, True):
            with self.subTest(hostful=hostful):
                argv = self.host_argv if hostful else self.argv
                with mock.patch.object(rq, 'run_cells') as launch:
                    code, _, err = self.run_main(argv('--only', SURFACE_CELL))
                self.assertEqual(code, 2, err)
                self.assertIn('byte cap', err)
                launch.assert_not_called()
                self.assertEqual(host_path.stat().st_size, MAX_FILE_BYTES + 1)
                self.assertEqual((self.out / rq.QUALIFICATION_NAME).read_bytes(),
                                 self.previous_qualification)
                self.assertEqual(list(self.out.glob('*.prev-*')), [])
                self.assertFalse((self.out / rq.LOCK_NAME).exists())

    def test_nonregular_hosts_refuse_before_open_or_execution(self):
        original_lstat = Path.lstat
        original_open = Path.open
        original_os_open = os.open
        for candidate in (self.staged_host, self.out / 'host.json'):
            for mode in (stat.S_IFLNK, stat.S_IFDIR, stat.S_IFCHR, stat.S_IFIFO):
                for hostful in ((True,) if candidate == self.staged_host else (False, True)):
                    with self.subTest(candidate=candidate.name, mode=mode, hostful=hostful):
                        # Model filesystem types without symlink or device privileges.
                        inspected = []

                        def lstat(path, *args, **kwargs):
                            if path == candidate:
                                inspected.append(path)
                                return mock.Mock(st_mode=mode | 0o600)
                            return original_lstat(path, *args, **kwargs)

                        def open_file(path, *args, **kwargs):
                            self.assertNotEqual(path, candidate)
                            return original_open(path, *args, **kwargs)

                        def os_open(path, *args, **kwargs):
                            self.assertNotEqual(Path(path), candidate)
                            return original_os_open(path, *args, **kwargs)

                        argv = self.host_argv if hostful else self.argv
                        with mock.patch.object(Path, 'lstat', lstat), \
                                mock.patch.object(Path, 'open', open_file), \
                                mock.patch.object(rq.os, 'open', os_open), \
                                mock.patch.object(rq, 'run_cells') as launch:
                            code, _, err = self.run_main(argv(
                                '--only', SURFACE_CELL, out=self.out / '..' / self.out.name))
                        self.assertEqual(code, 2, err)
                        self.assertIn('regular file', err)
                        self.assertEqual(inspected, [candidate])
                        launch.assert_not_called()
                        self.assert_previous_pair()
                        self.assertFalse((self.out / rq.LOCK_NAME).exists())


class ClientIdentityTests(RunnerTestBase):
    def receipt(self):
        return rq.read_json_object(self.out / 'cells' / f'{SURFACE_CELL}.process.json')

    def test_script_receipt_hashes_client_separately_from_interpreter(self):
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        receipt = self.receipt()
        self.assertEqual(receipt['binary_sha256'], rq.runner.file_sha256(sys.executable))
        self.assertEqual(receipt['client']['sha256'], rq.runner.file_sha256(self.fake_client))
        self.assertNotEqual(receipt['client']['sha256'], receipt['binary_sha256'])
        self.assertTrue(receipt['client']['sha256_verified'])
        self.assertFalse(receipt['client']['native_build'])
        self.assertEqual(receipt['capture_path'], f'cells/{SURFACE_CELL}.artifact.json')
        document = self.load_qualification()
        self.assertFalse(document['render_floor_qualified'])
        self.assertIn('client_not_native_build', document['render_floor_qualified_reasons'])

    def test_client_code_changed_during_execution_is_refused(self):
        before = rq.runner.file_sha256(self.fake_client)
        code, _, _ = self.run_main(self.argv('--only', SURFACE_CELL),
                                   env={'FAKE_CLIENT_MODE': 'selfchange'})
        self.assertEqual(code, 1)
        receipt = self.receipt()
        self.assertEqual(receipt['client']['sha256'], before)
        self.assertFalse(receipt['client']['sha256_verified'])
        self.assertNotEqual(before, rq.runner.file_sha256(self.fake_client))
        self.assertIn('client SHA-256', self.load_qualification()['cells'][0]['reason'])


class MatrixIdentityTests(RunnerTestBase):
    def test_identical_copy_of_committed_matrix_is_still_custom(self):
        self.matrix_path.write_bytes(MATRIX_PATH.read_bytes())
        self.assertEqual(self.run_main(self.argv('--only', SURFACE_CELL))[0], 0)
        document = self.load_qualification()
        self.assertEqual(document['matrix']['sha256'], document['matrix']['canonical_sha256'])
        self.assertFalse(document['matrix']['canonical'])
        self.assertTrue(all(not cell['matrix_canonical'] for cell in document['cells']))
        self.assertFalse(document['render_floor_qualified'])
        self.assertIn('non_canonical_matrix', document['render_floor_qualified_reasons'])

    def test_custom_pose_matrix_marks_every_cell_noncanonical(self):
        code, _, _ = self.run_main(self.argv())
        self.assertEqual(code, 3)
        document = self.load_qualification()
        self.assertNotEqual(document['matrix']['sha256'],
                            document['matrix']['canonical_sha256'])
        self.assertTrue(all(not cell['matrix_canonical'] for cell in document['cells']))
        self.assertIn('non_canonical_matrix', document['render_floor_qualified_reasons'])

    def test_default_matrix_hash_is_computed_from_repo_file(self):
        identity = rq.matrix_identity(matrix_lib.load_matrix(MATRIX_PATH))
        self.assertTrue(identity['canonical'])
        self.assertEqual(identity['sha256'], identity['canonical_sha256'])


class RealCheckerTests(RunnerTestBase):
    def positive_control(self):
        """Use the committed measurement fixture with a matching adapter/report pose.

        All timing samples/distributions remain exactly the committed fixture's.
        The fixture is synthetic and is never native performance evidence.
        """
        data = json.loads(FIXTURE_PATH.read_text(encoding='utf-8'))
        data['adapter']['renderer'] = RENDERER
        matrix = matrix_lib.load_matrix(self.matrix_path)
        display = next(item for item in matrix['displays'] if item['id'] == '3840x1600')
        data.update(width=display['width'], height=display['height'],
                    internal_width=display['width'], internal_height=display['height'])
        data['profile'].update(width=display['width'], height=display['height'])
        position = [POSE[key] for key in ('x', 'y', 'z')]
        right, up, forward = capture_checker.camera_basis(POSE['yaw'], POSE['pitch'])
        dot = capture_checker.dot
        view = [right[0], up[0], -forward[0], 0, right[1], up[1], -forward[1], 0,
                right[2], up[2], -forward[2], 0,
                -dot(right, position), -dot(up, position), dot(forward, position), 1]
        inverse = right + [0] + up + [0] + [-x for x in forward] + [0] + position + [1]
        scale = 1 / math.tan(math.radians(45) / 2)
        projection = [scale / (data['width'] / data['height']), 0, 0, 0,
                      0, scale, 0, 0, 0, 0, -1, -1, 0, 0, -0.2, 0]
        data.update(camera={'position': position, 'yaw': POSE['yaw'], 'pitch': POSE['pitch'],
                                'fov': 45.0},
                    capture_context={'time_of_day_at_report': 0.04,
                                         'last_world_streaming_position': position},
                    streaming={'terrain_draws': 1, 'generation_pending': 0,
                                   'meshing_pending': 0, 'terrain_uploads_pending': 0},
                    camera_chunk={'resident': True},
                    render_uniforms={
                        'matrix_layout': 'column_major', 'observation': 'report_time_shader_state',
                        'geometry': {'available': True, 'view': view, 'projection': projection},
                        'instanced_static_mesh': {'available': True, 'view': view,
                                                   'projection': projection},
                        'lighting': {'available': True, 'u_viewPos': position,
                                  'u_inverseView': inverse,
                                  'u_sun.direction': [math.sin(0.04 * 2 * math.pi),
                                                      -math.cos(0.04 * 2 * math.pi), 0]}})
        fixture = self.tmp / 'positive-control.json'
        fixture.write_text(json.dumps(data), encoding='utf-8')
        matrix.update(warmup_frames=data['warmup_frames'],
                      fixed_view_measured_frames=data['frames'])
        matrix_lib.validate_matrix(matrix)
        self.matrix_path.write_text(json.dumps(matrix), encoding='utf-8')
        return fixture, FOREST_CELL

    def test_real_checker_and_runner_accept_committed_fixture_positive_control(self):
        fixture, cell_id = self.positive_control()
        code, _, _ = self.run_main(self.argv('--only', cell_id, checker=False),
                                   env={'FAKE_CLIENT_MODE': 'copyfixture',
                                        'FAKE_CLIENT_FIXTURE': str(fixture)})
        self.assertEqual(code, 0)
        document = self.load_qualification()
        cell = self.cell_of(document, cell_id)
        self.assertEqual(cell['state'], 'pass')
        self.assertTrue(cell['checker_canonical'])
        expected = rq.render_budget.evaluate(json.loads(fixture.read_text(encoding='utf-8')))
        self.assertEqual(cell['verdict'], expected)
        self.assertEqual(rq.read_json_object(self.out / 'cells' / f'{cell_id}.verdict.json'),
                         expected)
        self.assertFalse(document['render_floor_qualified'])
        self.assertEqual(document['qualification_status'], 'NEEDS-NATIVE-RUN')

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


class BuildManifestTests(RealCheckerTests):
    def test_invalid_manifests_refused_before_run(self):
        path = self.write_build_manifest(self.fake_client)
        valid = rq.read_json_object(path)
        defects = [dict(valid, schema='wrong'), dict(valid, client={'sha256': 'xyz'}),
                   dict(valid, compiler={}), dict(valid, configuration={}),
                   dict(valid, system={}), {key: value for key, value in valid.items()
                                           if key != 'client'}]
        for document in defects:
            with self.subTest(document=document):
                path.write_text(json.dumps(document), encoding='utf-8')
                with mock.patch.object(rq, 'run_cells') as launch:
                    code, _, err = self.run_main(self.argv('--build-manifest', str(path)))
                self.assertEqual(code, 2)
                self.assertIn('--build-manifest', err)
                launch.assert_not_called()
                self.assertFalse(self.out.exists())

    def test_matching_manifest_cannot_qualify_forged_python_client(self):
        path = self.write_build_manifest(self.fake_client)
        code, _, _ = self.run_main(self.argv('--only', SURFACE_CELL,
                                             '--build-manifest', str(path)))
        self.assertEqual(code, 0)
        client = self.load_qualification()['cells'][0]['client']
        self.assertTrue(client['sha256_verified'])
        self.assertEqual(client['sha256'], client['build_manifest_client_sha256'])
        self.assertEqual(client['reason'], 'client_not_native_build')
        self.assertFalse(client['native_build'])
        # Even grant the forged capture every other prerequisite, including all 36
        # cells and the canonical checker: its client evidence still blocks qualification.
        matrix = matrix_lib.load_matrix(MATRIX_PATH)
        results = passing_results(matrix)
        for result in results:
            result['client'] = client
        with placeholder_bindings():
            document = rq.aggregate(matrix, results, partial=False,
                                    declared_refresh={'3440x1440': 60})
        self.assertFalse(document['render_floor_qualified'])
        self.assertIn('client_not_native_build', document['render_floor_qualified_reasons'])

    def test_suffixless_script_is_never_a_native_build(self):
        client = self.tmp / 'renamed-client'
        client.write_text(FAKE_CLIENT, encoding='utf-8')
        client.chmod(0o755)
        path = self.write_build_manifest(client)
        args = self.argv('--only', SURFACE_CELL, '--build-manifest', str(path))
        args[1] = str(client)
        original_run = subprocess.run

        def run(command, **kwargs):
            # Execute the suffixless fixture portably while identity checks and
            # receipt hashing still see the original client path.
            if Path(command[0]) == client.resolve():
                command = [sys.executable] + command
            return original_run(command, **kwargs)

        with mock.patch.object(rq.runner.subprocess, 'run', side_effect=run):
            code, _, err = self.run_main(args)
        self.assertEqual(code, 0, err)
        cell = self.load_qualification()['cells'][0]
        self.assertEqual(cell['state'], 'pass')
        self.assertEqual(cell['client']['reason'], 'client_not_native_build')
        self.assertEqual(cell['client']['kind'], 'script')
        self.assertTrue(cell['client']['sha256_verified'])
        self.assertFalse(cell['client']['native_build'])

    def test_mz_prefixed_command_scripts_are_never_native(self):
        for suffix in ('.cmd', '.bat', '.CMD', '.exe'):
            with self.subTest(suffix=suffix):
                client = self.tmp / ('client' + suffix)
                client.write_bytes(b'MZ&@echo off\r\n' + b' ' * 256)
                client.chmod(0o755)
                self.assertFalse(rq.native_executable(client))

    def test_native_image_with_script_suffix_is_refused(self):
        for suffix in ('.cmd', '.bat', '.py', '.sh', '.ps1'):
            with self.subTest(suffix=suffix):
                client = self.tmp / ('client' + suffix)
                shutil.copy2(sys.executable, client)
                client.chmod(0o755)
                self.assertFalse(rq.native_executable(client))

    def native_control(self, manifest_mode='match', mutate=False, capture_mode='private',
                       receipt_defect=None, mutate_checked_capture=False):
        # Copy an existing native binary; never compile or claim GPU performance.
        client = self.tmp / ('native-client.exe' if os.name == 'nt' else 'native-client')
        shutil.copy2(sys.executable, client)
        client.chmod(0o755)
        path = self.write_build_manifest(client)
        if manifest_mode == 'mismatch':
            document = rq.read_json_object(path)
            document['client']['sha256'] = '0' * 64
            path.write_text(json.dumps(document), encoding='utf-8')
        fixture, cell_id = self.positive_control()
        original = rq.runner.run_process

        def native_process(command, **kwargs):
            def emit_capture(argv, **spawn_kwargs):
                self.assertEqual(Path(argv[0]), client.resolve())
                capture = Path(argv[argv.index('--render-benchmark') + 1])
                self.assertEqual(capture, kwargs['artifact'])
                public = self.out / 'cells' / f'{cell_id}.json'
                self.assertNotEqual(capture, public)
                target = capture if capture_mode == 'private' else public
                target.write_bytes(fixture.read_bytes())
                if mutate:
                    with client.open('ab') as output:
                        output.write(b'changed')
                return subprocess.CompletedProcess(argv, 0)

            # Only native client execution is simulated. Receipt retention/hashing and
            # the real checker subprocess execute normally.
            with mock.patch.object(rq.runner.subprocess, 'run', side_effect=emit_capture):
                result = original(command, **kwargs)
            if receipt_defect is not None:
                receipt = rq.read_json_object(kwargs['receipt_path'])
                receipt.update(receipt_defect)
                kwargs['receipt_path'].write_text(json.dumps(receipt), encoding='utf-8')
            return result

        args = self.argv('--only', cell_id, checker=False)
        args[1] = str(client)
        if manifest_mode != 'missing':
            args += ['--build-manifest', str(path)]
        original_checker = rq.check_command

        def checker_command(*command_args, **kwargs):
            command = original_checker(*command_args, **kwargs)
            if mutate_checked_capture:
                # Whitespace preserves every parsed field and verdict, but changes ownership.
                with kwargs['artifact'].open('ab') as output:
                    output.write(b'\n')
            return command

        with mock.patch.object(rq.runner, 'run_process', side_effect=native_process), \
                mock.patch.object(rq, 'check_command', side_effect=checker_command):
            code, _, _ = self.run_main(args)
        document = self.load_qualification()
        receipt = rq.read_json_object(self.out / 'cells' / f'{cell_id}.process.json')
        return code, document['cells'][0], receipt

    def test_manifest_bound_native_control_can_qualify(self):
        code, cell, receipt = self.native_control()
        self.assertEqual(code, 0)
        self.assertEqual(cell['state'], 'pass')
        self.assertTrue(cell['checker_canonical'])
        client = cell['client']
        self.assertEqual(client, receipt['client'])
        self.assertEqual(client['sha256'], receipt['binary_sha256'])
        self.assertEqual(receipt['capture_status'], 'retained')
        self.assertEqual(receipt['capture_path'], f"cells/{cell['id']}.artifact.json")
        digest = rq.runner.file_sha256(self.out / cell['artifact'])
        self.assertEqual(receipt['capture_sha256'], digest)
        self.assertEqual(rq.runner.file_sha256(self.out / receipt['capture_path']), digest)
        self.assertTrue(client['sha256_verified'])
        self.assertTrue(client['native_build'])
        matrix = matrix_lib.load_matrix(MATRIX_PATH)
        results = passing_results(matrix)
        for result in results:
            result['client'] = client
        with placeholder_bindings():
            document = rq.aggregate(matrix, results, partial=False,
                                    declared_refresh={'3440x1440': 60})
        self.assertTrue(document['render_floor_qualified'])
        self.assertFalse(document['expanded_qualified'])

    def test_public_artifact_without_owned_capture_is_refused(self):
        code, cell, receipt = self.native_control(capture_mode='public')
        self.assertEqual(code, 1)
        self.assertEqual(cell['state'], 'process_failed')
        self.assertEqual(cell['reason'], 'capture evidence: capture not retained')
        self.assertEqual(receipt['capture_status'], 'missing')
        self.assertTrue((self.out / cell['artifact']).is_file())
        self.assertFalse((self.out / 'cells' / f"{cell['id']}.check.log").exists())
        self.assertFalse(self.load_qualification()['render_floor_qualified'])

    def test_receipt_hash_must_match_checked_artifact(self):
        code, cell, receipt = self.native_control(receipt_defect={'capture_sha256': '0' * 64})
        self.assertEqual(code, 1)
        self.assertEqual(cell['state'], 'process_failed')
        self.assertEqual(cell['reason'], 'capture evidence: retained hash mismatch')
        self.assertEqual(receipt['capture_status'], 'retained')
        self.assertIsNone(cell['artifact'])
        self.assertFalse((self.out / 'cells' / f"{cell['id']}.check.log").exists())

    def test_capture_changed_before_checker_is_refused(self):
        code, cell, receipt = self.native_control(mutate_checked_capture=True)
        self.assertEqual(code, 1)
        self.assertEqual(cell['state'], 'invalid')
        self.assertEqual(cell['reason'], 'capture evidence: checked hash mismatch')
        self.assertNotEqual(rq.runner.file_sha256(self.out / cell['artifact']),
                            receipt['capture_sha256'])

    def test_native_manifest_missing_or_mismatched_blocks_qualification(self):
        for mode, reason in (('missing', 'build_manifest_missing'),
                             ('mismatch', 'client_build_hash_mismatch')):
            with self.subTest(mode=mode):
                code, cell, receipt = self.native_control(mode)
                self.assertEqual(code, 0)
                self.assertEqual(cell['state'], 'pass')
                self.assertTrue(receipt['client']['sha256_verified'])
                self.assertFalse(cell['client']['native_build'])
                self.assertEqual(cell['client']['reason'], reason)

    def test_native_binary_changed_during_run_is_refused(self):
        code, cell, receipt = self.native_control(mutate=True)
        self.assertEqual(code, 1)
        self.assertEqual(cell['state'], 'process_failed')
        self.assertFalse(receipt['client']['sha256_verified'])
        self.assertFalse(receipt['client']['native_build'])


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

    def test_changed_fixed_view_pose_and_provenance_never_qualify(self):
        for field, value in (('pose', dict(POSE)), ('pose_provenance', 'easier custom view')):
            with self.subTest(field=field), placeholder_bindings():
                matrix = copy.deepcopy(self.matrix)
                matrix['workloads'][0][field] = value
                document = self.aggregate(passing_results(matrix), matrix=matrix)
                self.assertFalse(document['render_floor_qualified'])
                self.assertIn('non_canonical_matrix',
                              document['render_floor_qualified_reasons'])
                self.assertNotEqual(document['matrix']['sha256'],
                                    document['matrix']['canonical_sha256'])

    def test_client_identity_absent_or_forged_never_qualifies(self):
        for defect in (None, {'kind': 'script'}, {'sha256_verified': False},
                       {'build_manifest_client_sha256': '0' * 64}):
            with self.subTest(defect=defect), placeholder_bindings():
                results = self.baseline()
                if defect is None:
                    results[0].pop('client')
                else:
                    results[0]['client'].update(defect)
                self.assertFalse(self.aggregate(results)['render_floor_qualified'])

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
            results[2] = dict(results[2], adapter={'vendor': '', 'renderer': RENDERER,
                                                       'version': '4.6'})
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
