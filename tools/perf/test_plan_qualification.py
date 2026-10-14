#!/usr/bin/env python3
"""Tests for the qualification matrix, planner CLI and parametrised process runner."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
from unittest import mock
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import plan_qualification as planner  # noqa: E402
import qualification_matrix as qm  # noqa: E402
import render_runner_process as runner  # noqa: E402
import validate_render_capture as capture  # noqa: E402

ROOT = planner.ROOT
MATRIX_PATH = planner.DEFAULT_MATRIX
PLANNER_SCRIPT = Path(__file__).resolve().parent / 'plan_qualification.py'
RENDERER = 'NVIDIA GeForce RTX 5070 Ti'
SURFACE_SCRIPT = 'tools/perf/fixtures/surface-flight.traversal'
POSE = {'x': 100.5, 'y': -2.25, 'z': 30.0, 'yaw': 45.0, 'pitch': -10.5}
CAVE_BLOCKED = 'traversal v2 events (fw #132); spec only'
EDITED_BLOCKED = 'edited-world authority (fw #132); spec only'


def committed():
    return qm.load_matrix(MATRIX_PATH)


def find_cell(cells, cell_id):
    return next(cell for cell in cells if cell['id'] == cell_id)


def with_pose(matrix):
    """Copy of the committed matrix with a recorded pose on forest-settled."""
    result = copy.deepcopy(matrix)
    result['workloads'][0]['pose'] = dict(POSE)
    return result


class MatrixTestBase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.tmp = Path(self.directory.name)

    def write_text(self, text, name='matrix.json'):
        path = self.tmp / name
        path.write_text(text, encoding='utf-8')
        return path

    def assert_refused(self, text, pattern):
        path = self.write_text(text)
        with self.assertRaisesRegex(qm.MatrixError, pattern):
            qm.load_matrix(path)


class CommittedMatrixTests(MatrixTestBase):
    def test_committed_matrix_has_36_unique_cells_in_order(self):
        cells = qm.cells(committed())
        ids = [cell['id'] for cell in cells]
        self.assertEqual(len(cells), 36)
        self.assertEqual(len(set(ids)), 36)
        self.assertEqual(ids[:4], ['forest-settled.quality.3840x1600',
                                   'forest-settled.quality.3440x1440',
                                   'forest-settled.performance.3840x1600',
                                   'forest-settled.performance.3440x1440'])

    def test_committed_fixed_views_are_pose_unrecorded(self):
        cells = qm.cells(committed())
        fixed = [cell for cell in cells if cell['kind'] == 'fixed_view']
        self.assertEqual(len(fixed), 24)
        for cell in fixed:
            with self.subTest(cell=cell['id']):
                self.assertEqual(qm.readiness(cell, ROOT),
                                 ('unevaluated', 'pose-unrecorded'))

    def test_surface_flight_ready_and_blocked_traversals(self):
        cells = qm.cells(committed())
        for cell in cells:
            if cell['workload'] == 'surface-flight':
                with self.subTest(cell=cell['id']):
                    self.assertEqual(qm.readiness(cell, ROOT), ('ready', None))
            elif cell['workload'] == 'cave-walk':
                self.assertEqual(qm.readiness(cell, ROOT),
                                 ('unevaluated', f'blocked: {CAVE_BLOCKED}'))
            elif cell['workload'] == 'edited-world-tour':
                self.assertEqual(qm.readiness(cell, ROOT),
                                 ('unevaluated', f'blocked: {EDITED_BLOCKED}'))

    def test_build_command_refuses_unready_cells(self):
        cells = qm.cells(committed())
        with self.assertRaisesRegex(qm.MatrixError, 'pose-unrecorded|not ready'):
            qm.build_command(find_cell(cells, 'forest-settled.quality.3840x1600'),
                             client='client', artifact='a.json', root=ROOT)
        with self.assertRaisesRegex(qm.MatrixError, 'not ready'):
            qm.build_command(find_cell(cells, 'cave-walk.quality.3840x1600'),
                             client='client', artifact='a.json', root=ROOT)
        with self.assertRaisesRegex(qm.MatrixError, 'not ready'):
            qm.checker_command(find_cell(cells, 'cave-walk.quality.3840x1600'),
                               artifact='a.json', qualified_renderer=RENDERER, root=ROOT,
                               python='python3')


class CommandGoldenTests(MatrixTestBase):
    def test_fixed_view_client_argv(self):
        cells = qm.cells(with_pose(committed()))
        cell = find_cell(cells, 'forest-settled.quality.3840x1600')
        command = qm.build_command(cell, client='/bin/client', artifact='/out/a.json',
                                   root=ROOT)
        self.assertEqual(command, [
            '/bin/client', '--no-audio', '--no-menu-backdrop', '--render-benchmark',
            '/out/a.json', '--render-benchmark-schema', 'v3', '--capture-size', '3840x1600',
            '--perf-profile', 'quality', '--render-benchmark-warmup', '400',
            '--render-benchmark-frames', '240', '--cam-pos', '100.5,-2.25,30.0',
            '--cam-yaw', '45.0', '--cam-pitch', '-10.5'])
        self.assertNotIn('--traversal', command)

    def test_traversal_client_argv(self):
        cells = qm.cells(committed())
        cell = find_cell(cells, 'surface-flight.performance.3440x1440')
        command = qm.build_command(cell, client='/bin/client', artifact='/out/a.json',
                                   root=ROOT)
        self.assertEqual(command, [
            '/bin/client', '--no-audio', '--no-menu-backdrop', '--render-benchmark',
            '/out/a.json', '--render-benchmark-schema', 'v3', '--capture-size', '3440x1440',
            '--perf-profile', 'performance', '--render-benchmark-warmup', '400',
            '--traversal', str(ROOT / SURFACE_SCRIPT)])
        self.assertNotIn('--cam-pos', command)
        self.assertNotIn('--render-benchmark-frames', command)

    def test_checker_goldens(self):
        cells = qm.cells(with_pose(committed()))
        fixed = qm.checker_command(find_cell(cells, 'forest-settled.quality.3840x1600'),
                                   artifact='/out/a.json', qualified_renderer=RENDERER,
                                   root=ROOT, python='/usr/bin/python3')
        self.assertEqual(fixed, [
            '/usr/bin/python3', str(ROOT / 'tools/perf/validate_render_capture.py'),
            '/out/a.json', '--qualified-renderer', RENDERER,
            '--position', '100.5', '-2.25', '30.0', '--yaw', '45.0', '--pitch', '-10.5',
            '--tod', '0.04'])
        traversal = qm.checker_command(
            find_cell(cells, 'surface-flight.quality.3840x1600'), artifact='/out/a.json',
            qualified_renderer=RENDERER, root=ROOT, python='/usr/bin/python3')
        self.assertEqual(traversal, [
            '/usr/bin/python3', str(ROOT / 'tools/perf/validate_render_capture.py'),
            '/out/a.json', '--qualified-renderer', RENDERER,
            '--traversal', str(ROOT / SURFACE_SCRIPT)])


class CheckerArgparseTests(MatrixTestBase):
    def test_fixed_checker_argv_parses_with_tod(self):
        # Runs the real validator main() argument parsing; only validate() is replaced.
        cells = qm.cells(with_pose(committed()))
        artifact = self.tmp / 'capture.json'
        artifact.write_text('{}', encoding='utf-8')
        argv = qm.checker_command(find_cell(cells, 'forest-settled.quality.3840x1600'),
                                  artifact=artifact, qualified_renderer=RENDERER,
                                  root=ROOT, python=sys.executable)
        seen = {}

        def fake_validate(data, args):
            seen['args'] = args
            return {'verdict': 'PASS'}

        with mock.patch.object(capture, 'validate', fake_validate), \
                mock.patch.object(sys, 'argv', argv[1:]), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(capture.main(), 0)
        args = seen['args']
        self.assertEqual(args.tod, 0.04)
        self.assertEqual(args.position, [100.5, -2.25, 30.0])
        self.assertEqual((args.yaw, args.pitch), (45.0, -10.5))
        self.assertEqual(args.qualified_renderer, RENDERER)
        self.assertIsNone(args.traversal)

    def test_traversal_checker_argv_has_no_tod(self):
        cells = qm.cells(committed())
        argv = qm.checker_command(find_cell(cells, 'surface-flight.quality.3840x1600'),
                                  artifact='a.json', qualified_renderer=RENDERER, root=ROOT,
                                  python='python3')
        self.assertNotIn('--tod', argv)


class MatrixValidationTests(MatrixTestBase):
    def test_null_renderer_refused(self):
        matrix = copy.deepcopy(committed())
        matrix['qualified_renderer'] = None
        self.assert_refused(json.dumps(matrix), 'qualified_renderer')

    def test_performance_scale_must_be_067(self):
        matrix = copy.deepcopy(committed())
        matrix['profiles'][1]['resolution_scale'] = 1.0
        self.assert_refused(json.dumps(matrix), "resolution_scale")

    def test_wrong_world_kind_refused(self):
        matrix = copy.deepcopy(committed())
        matrix['world']['kind'] = 'seeded'
        self.assert_refused(json.dumps(matrix), "world kind")

    def test_time_of_day_out_of_range_refused(self):
        matrix = copy.deepcopy(committed())
        matrix['time_of_day'] = 1.0
        self.assert_refused(json.dumps(matrix), 'time_of_day')

    def test_time_of_day_other_than_client_fixed_value_refused(self):
        matrix = copy.deepcopy(committed())
        matrix['time_of_day'] = 0.5
        self.assert_refused(json.dumps(matrix), 'time_of_day')

    def test_missing_refresh_hz_refused(self):
        matrix = copy.deepcopy(committed())
        del matrix['displays'][0]['refresh_hz']
        self.assert_refused(json.dumps(matrix), 'refresh_hz')

    def test_display_bounds_refused(self):
        for label, change in (('width 16385', {'width': 16385}),
                              ('refresh 0', {'refresh_hz': 0}),
                              ('bool width', {'width': True})):
            with self.subTest(label):
                matrix = copy.deepcopy(committed())
                matrix['displays'][0].update(change)
                if 'width' in change:
                    matrix['displays'][0]['id'] = f"{matrix['displays'][0]['width']}x1600"
                self.assert_refused(json.dumps(matrix), 'display')

    def test_wrong_schema_refused(self):
        matrix = copy.deepcopy(committed())
        matrix['schema'] = 'luminumbra.qualification_matrix.v0'
        self.assert_refused(json.dumps(matrix), 'schema')

    def test_duplicate_workload_ids_refused(self):
        matrix = copy.deepcopy(committed())
        matrix['workloads'][1]['id'] = matrix['workloads'][0]['id']
        self.assert_refused(json.dumps(matrix), 'duplicate workload id')

    def test_bad_pose_key_set_refused(self):
        for label, pose in (('extra key', dict(POSE, roll=0.0)),
                            ('missing key', {k: v for k, v in POSE.items() if k != 'pitch'})):
            with self.subTest(label):
                matrix = copy.deepcopy(committed())
                matrix['workloads'][0]['pose'] = pose
                self.assert_refused(json.dumps(matrix), 'pose')

    def test_nan_pose_literal_refused(self):
        matrix = with_pose(committed())
        matrix['workloads'][0]['pose'] = dict(POSE, x=1.0)
        text = json.dumps(matrix).replace('"x": 1.0', '"x": NaN', 1)
        self.assertIn('NaN', text)
        self.assert_refused(text, 'NaN')

    def test_duplicate_json_key_refused(self):
        text = json.dumps(committed())
        text = '{"schema": "luminumbra.qualification_matrix.v1", ' + text[1:]
        self.assert_refused(text, 'duplicate JSON key')

    def test_display_id_mismatch_refused(self):
        matrix = copy.deepcopy(committed())
        matrix['displays'][0]['id'] = '3840x1601'
        self.assert_refused(json.dumps(matrix), "display id '3840x1601'")


class ExecutableResolutionTests(MatrixTestBase):
    def test_relative_path_resolves_against_cwd(self):
        tool = self.tmp / 'bin' / 'tool'
        tool.parent.mkdir()
        tool.write_text('x', encoding='utf-8')
        resolved = runner.resolve_executable('bin/tool', self.tmp)
        self.assertEqual(resolved, Path(os.path.abspath(tool)))

    def test_bare_name_resolves_through_path(self):
        name = Path(sys.executable).name
        resolved = runner.resolve_executable(name, self.tmp,
                                             {'PATH': str(Path(sys.executable).parent)})
        self.assertTrue(os.path.samefile(resolved, sys.executable))

    def test_relative_path_entry_resolves_against_launch_cwd(self):
        name = Path(sys.executable).name
        (self.tmp / 'bin').mkdir()
        shutil.copy2(sys.executable, self.tmp / 'bin' / name)
        resolved = runner.resolve_executable(name, self.tmp, {'PATH': 'bin'})
        self.assertEqual(resolved.parent, Path(os.path.abspath(self.tmp / 'bin')))

    def test_unresolved_name_raises(self):
        with self.assertRaises(FileNotFoundError):
            runner.resolve_executable('no-such-luminumbra-tool', self.tmp, {'PATH': ''})
        with self.assertRaises(FileNotFoundError):
            runner.resolve_executable('bin/no-such-tool', self.tmp)

    def test_run_process_refuses_unresolved_before_spawn(self):
        log = self.tmp / 'log.txt'
        receipt = self.tmp / 'receipt.json'
        with self.assertRaises(FileNotFoundError):
            runner.run_process(['no-such-luminumbra-tool'], cwd=self.tmp,
                               environment={'PATH': ''}, log=log,
                               artifact=self.tmp / 'a.json', retained=self.tmp / 'r.json',
                               receipt_path=receipt)
        self.assertFalse(log.exists())
        self.assertFalse(receipt.exists())


class PlannerCliTests(MatrixTestBase):
    def run_planner(self, *extra):
        out_dir = self.tmp / 'out'
        command = [sys.executable, str(PLANNER_SCRIPT), '--client', '/bin/client',
                   '--out-dir', str(out_dir), *extra]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        return result, out_dir / 'qualification-plan.json'

    def test_dry_run_writes_plan(self):
        result, path = self.run_planner()
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(path.read_text(encoding='utf-8'))
        self.assertEqual(plan['schema'], 'luminumbra.qualification_plan.v1')
        self.assertEqual(plan['matrix_schema'], qm.SCHEMA)
        self.assertFalse(plan['partial'])
        self.assertEqual(plan['cell_count'], 36)
        self.assertEqual(len(plan['cells']), 36)
        self.assertEqual(plan['summary'], {'ready': 4, 'unevaluated': 32})
        ready = [cell for cell in plan['cells'] if cell['state'] == 'ready']
        self.assertTrue(all(cell['command'] and cell['checker_command'] for cell in ready))
        unevaluated = [cell for cell in plan['cells'] if cell['state'] != 'ready']
        self.assertTrue(all(cell['command'] is None and cell['checker_command'] is None
                            for cell in unevaluated))

    def test_only_filters_and_marks_partial(self):
        result, path = self.run_planner('--only', 'surface-flight.quality.3840x1600',
                                        '--only', 'forest-settled.quality.3440x1440')
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(path.read_text(encoding='utf-8'))
        self.assertTrue(plan['partial'])
        self.assertEqual(plan['cell_count'], 2)
        self.assertEqual([cell['id'] for cell in plan['cells']],
                         ['forest-settled.quality.3440x1440',
                          'surface-flight.quality.3840x1600'])

    def test_unknown_only_exits_2(self):
        result, path = self.run_planner('--only', 'no-such.cell.3840x1600')
        self.assertEqual(result.returncode, 2)
        self.assertIn('unknown cell id', result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertFalse(path.exists())

    def test_invalid_matrix_exits_2(self):
        bad = self.write_text('{"schema": "wrong"}')
        result, _ = self.run_planner('--matrix', str(bad))
        self.assertEqual(result.returncode, 2)
        self.assertNotIn('Traceback', result.stderr)


class ProcessRunnerTests(MatrixTestBase):
    def setUp(self):
        super().setUp()
        self.artifact = self.tmp / 'capture.json'
        self.retained = self.tmp / 'retained.json'
        self.receipt = self.tmp / 'receipt.json'
        self.log = self.tmp / 'log.txt'

    def script(self, body):
        path = self.tmp / 'fake_client.py'
        path.write_text(body, encoding='utf-8')
        return [sys.executable, str(path), str(self.artifact)]

    def run_case(self, command, timeout_seconds=60):
        return runner.run_process(command, cwd=self.tmp, environment=os.environ.copy(),
                                  log=self.log, artifact=self.artifact,
                                  retained=self.retained, receipt_path=self.receipt,
                                  timeout_seconds=timeout_seconds)

    def read_receipt(self):
        return json.loads(self.receipt.read_text(encoding='utf-8'))

    def test_success_retains_capture_and_records_timeout(self):
        command = self.script('import sys\nfrom pathlib import Path\n'
                              'Path(sys.argv[1]).write_text("{}")\n')
        result, errors = self.run_case(command, timeout_seconds=60)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(errors, [])
        receipt = self.read_receipt()
        self.assertEqual(receipt['schema'], 'luminumbra.render_runner_process.v1')
        self.assertEqual(receipt['capture_status'], 'retained')
        self.assertEqual(receipt['outcome'], 'returned')
        self.assertEqual(receipt['timeout_seconds'], 60)
        self.assertTrue(self.retained.is_file())

    def test_timeout_raises_and_receipt_shows_timed_out(self):
        command = self.script('import time\n'
                              'print("x" * 20000, flush=True)\n'
                              'print("EnsureSurfaceReadyNear: draining", flush=True)\n'
                              'time.sleep(30)\n')
        diagnostic = io.StringIO()
        with contextlib.redirect_stderr(diagnostic), \
                self.assertRaises(subprocess.TimeoutExpired):
            self.run_case(command, timeout_seconds=1)
        receipt = self.read_receipt()
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['timeout_seconds'], 1)
        self.assertEqual(receipt['last_observed_phase'],
                         'EnsureSurfaceReadyNear: draining')
        self.assertEqual(len(receipt['log_tail'].encode('utf-8')),
                         runner.TIMEOUT_LOG_TAIL_BYTES)
        self.assertIn('timed out after 1 s', diagnostic.getvalue())
        self.assertIn('EnsureSurfaceReadyNear: draining', diagnostic.getvalue())

    def test_nonzero_exit_code_recorded(self):
        command = self.script('import sys\nfrom pathlib import Path\n'
                              'Path(sys.argv[1]).write_text("{}")\nsys.exit(3)\n')
        result, _ = self.run_case(command)
        self.assertEqual(result.returncode, 3)
        receipt = self.read_receipt()
        self.assertEqual(receipt['return_code'], 3)
        self.assertEqual(receipt['outcome'], 'returned')

    def test_missing_artifact_status(self):
        command = self.script('import sys\n')
        result, _ = self.run_case(command)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.read_receipt()['capture_status'], 'missing')


if __name__ == '__main__':
    unittest.main()
