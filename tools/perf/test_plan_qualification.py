#!/usr/bin/env python3
"""Tests for the qualification matrix, planner CLI and parametrised process runner."""
import contextlib
import copy
import io
import json
import math
import ntpath
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import subprocess
import shutil
import stat
import sys
import tempfile
import time
import types
from unittest import mock
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import plan_qualification as planner  # noqa: E402
import qualification_matrix as qm  # noqa: E402
import perf  # noqa: E402
import render_contract as contract  # noqa: E402
import render_runner_process as runner  # noqa: E402
import validate_render_capture as capture  # noqa: E402
from test_render_contract import artifact as synthetic_artifact  # noqa: E402

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
        # Temporary roots may use directory aliases or Windows short names.
        # Match the identities used by the runner before installing path mocks.
        self.tmp = runner.resolve_path(self.directory.name)

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
            '--profile', 'quality', '--width=3840', '--height=1600',
            '--warmup-frames=400', '--workload-kind', 'fixed_view',
            '--position', '100.5', '-2.25', '30.0', '--yaw=45.0', '--pitch=-10.5',
            '--tod=0.04', '--frames=240'])
        traversal = qm.checker_command(
            find_cell(cells, 'surface-flight.quality.3840x1600'), artifact='/out/a.json',
            qualified_renderer=RENDERER, root=ROOT, python='/usr/bin/python3')
        self.assertEqual(traversal, [
            '/usr/bin/python3', str(ROOT / 'tools/perf/validate_render_capture.py'),
            '/out/a.json', '--qualified-renderer', RENDERER,
            '--profile', 'quality', '--width=3840', '--height=1600',
            '--warmup-frames=400', '--workload-kind', 'traversal',
            '--traversal', str(ROOT / SURFACE_SCRIPT)])


class CheckerArgparseTests(MatrixTestBase):
    def test_fixed_checker_argv_parses_with_tod(self):
        self.check_pose_round_trip(POSE)

    def test_negative_scientific_pose_round_trip(self):
        self.check_pose_round_trip(dict(x=-1e-5, y=-2e-7, z=-3e20,
                                       yaw=-4e-6, pitch=-0.00001))

    def test_extreme_finite_pose_round_trip(self):
        self.check_pose_round_trip(dict(x=-5e-324, y=-1.7976931348623157e308,
                                       z=1e-20, yaw=1e20, pitch=-1e-20))

    def check_pose_round_trip(self, pose):
        # Runs the real validator main() argument parsing; only validate() is replaced.
        matrix = with_pose(committed())
        matrix['workloads'][0]['pose'] = dict(pose)
        qm.validate_matrix(matrix)
        cells = qm.cells(matrix)
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
        self.assertEqual(args.position, [pose[key] for key in ('x', 'y', 'z')])
        self.assertEqual((args.yaw, args.pitch), (pose['yaw'], pose['pitch']))
        self.assertEqual(args.qualified_renderer, RENDERER)
        self.assertEqual(args.profile, 'quality')
        self.assertEqual((args.width, args.height), (3840, 1600))
        self.assertEqual((args.warmup_frames, args.frames), (400, 240))
        self.assertEqual(args.workload_kind, 'fixed_view')
        self.assertIsNone(args.traversal)

    def test_traversal_checker_argv_has_no_tod(self):
        cells = qm.cells(committed())
        argv = qm.checker_command(find_cell(cells, 'surface-flight.quality.3840x1600'),
                                  artifact='a.json', qualified_renderer=RENDERER, root=ROOT,
                                  python='python3')
        self.assertFalse(any(arg.startswith('--tod') for arg in argv))


class CheckerEvidenceTests(MatrixTestBase):
    def traversal_capture(self, *, profile='performance', width=3440, height=1440,
                          warmup=400):
        """Synthetic full-length traversal with internally consistent cell metadata."""
        script = ROOT / SURFACE_SCRIPT
        text = script.read_bytes().decode('utf-8')
        manifest = contract.traversal_manifest(text)
        count = manifest['expected_ticks']
        data = synthetic_artifact()
        scale = 1.0 if profile == 'quality' else 0.67
        data.update(profile=dict(name=profile, render_scale=scale, width=width, height=height),
                    width=width, height=height, render_scale=scale, warmup_frames=warmup,
                    frames=count,
                    internal_width=math.floor(contract.float32(width * contract.float32(scale)) + 0.5),
                    internal_height=math.floor(contract.float32(height * contract.float32(scale)) + 0.5))
        rows = data['frame_samples']
        data['frame_samples'] = [dict(copy.deepcopy(rows[index % len(rows)]),
                                     frame=warmup + index, gpu_frame=warmup + index)
                                 for index in range(count)]
        data['excluded_windows'][0]['last_frame'] = warmup - 1
        data['excluded_windows'][1].update(first_frame=warmup + count, last_frame=warmup + count)
        data['workload'].update({key: value for key, value in manifest.items() if key != 'path'})
        data['workload'].update(kind='traversal', expected_frames=count, actual_ticks=count,
                                measured_duration_seconds=0.01, script=text, script_path=str(script),
                                camera_samples=[contract.camera_sample(manifest, tick)
                                                for tick in range(1, count + 1)])
        for name in contract.METRICS:
            values = [row[name] for row in data['frame_samples']]
            summary = perf.summarize(values, 'ms')
            data['distribution'][name].update({key: summary[key] for key in contract.STATS},
                                             sample_count=count)
            data['avg'][name] = sum(values) / count
        for name in contract.PASSES:
            data['avg_ms'][name] = sum(row['passes'][name]['ms'] or 0
                                       for row in data['frame_samples']) / count
        data['avg_ms']['total'] = data['avg']['gpu_pass_sum_ms']
        self.assertEqual(contract.validate(data, RENDERER, traversal=script)['verdict'], 'PASS')
        return data

    def run_checker(self, data):
        cell_id = 'surface-flight.performance.3440x1440'
        plan = planner.build_plan(committed(), client='client', out_dir=self.tmp, root=ROOT,
                                  python=sys.executable, only=[cell_id])
        argv = plan['cells'][0]['checker_command']
        artifact = Path(argv[2])
        artifact.parent.mkdir(parents=True, exist_ok=True)
        artifact.write_text(json.dumps(data), encoding='utf-8')
        # Keep the real checker CLI and all validation, with isolated module loading.
        bootstrap = ('import sys, runpy; from pathlib import Path; '
                     'sys.path.insert(0, str(Path(sys.argv[1]).resolve().parent)); '
                     'sys.argv = sys.argv[1:]; runpy.run_path(sys.argv[0], run_name="__main__")')
        result = subprocess.run([argv[0], '-I', '-B', '-c', bootstrap, *argv[1:]],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.stderr, '')
        return result.returncode, json.loads(result.stdout)

    def test_matching_capture_passes_generated_checker(self):
        code, verdict = self.run_checker(self.traversal_capture())
        self.assertEqual(code, 0)
        self.assertEqual(verdict['verdict'], 'PASS')

    def test_other_profile_resolution_and_warmup_fail_generated_checker(self):
        data = self.traversal_capture(profile='quality', width=1280, height=720, warmup=2)
        code, verdict = self.run_checker(data)
        self.assertEqual(code, 1)
        self.assertEqual(verdict['verdict'], 'FAIL')
        self.assertIn('--profile mismatch', verdict['reason'])

    def test_each_cell_parameter_mismatch_fails_generated_checker(self):
        for option, change in (('profile', dict(profile='quality')),
                               ('width', dict(width=1280)), ('height', dict(height=720)),
                               ('warmup-frames', dict(warmup=2))):
            with self.subTest(option=option):
                code, verdict = self.run_checker(self.traversal_capture(**change))
                self.assertEqual(code, 1)
                self.assertEqual(verdict['verdict'], 'FAIL')
                self.assertIn(f'--{option} mismatch', verdict['reason'])


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


class PathResolutionTests(unittest.TestCase):
    @contextlib.contextmanager
    def windows_paths(self):
        class WindowsPath(PureWindowsPath):
            def resolve(self):
                return type(self)(ntpath.normpath(self))

            def is_file(self):
                return True

        windows = types.SimpleNamespace(**vars(os))
        windows.name, windows.sep, windows.altsep = 'nt', '\\', '/'
        windows.path, windows.pathsep = ntpath, ';'
        with mock.patch.object(runner, 'os', windows), \
                mock.patch.object(runner, 'Path', WindowsPath):
            yield WindowsPath

    def test_unsafe_extended_components_preserve_their_namespace(self):
        devices = ('CON', 'PRN', 'AUX', 'NUL', 'con.txt', 'nul .json',
                   'CONIN$', 'conout$.txt', 'COM\u00b9', 'LPT\u00b2.txt', 'com\u00b3')
        devices += tuple(f'{stem}{digit}{extension}'
                         for stem in ('COM', 'LPT') for digit in range(1, 10)
                         for extension in ('', '.exe'))
        with self.windows_paths() as WindowsPath:
            for root in (r'\\?\C:\qualification', r'\\?\UNC\server\share'):
                for component in ('client.exe.', 'client.exe ', '..') + devices:
                    for tail in ('', r'\client.exe'):
                        text = root + '\\' + component + tail
                        with self.subTest(path=text):
                            self.assertEqual(runner.path_without_extended_prefix(text), text)
                            path = WindowsPath(text)
                            self.assertEqual(runner.path_without_extended_prefix(path), path)
                            with self.assertRaisesRegex(OSError, 'safely normalized'):
                                runner.resolve_path(path)

    def test_literal_dot_and_space_paths_never_equal_the_normalized_twin(self):
        def handle_path(path):
            text = str(path)
            if text.startswith('\\\\?\\'):
                return PureWindowsPath(text)
            # Model Win32's ordinary-name normalization before handle lookup.
            text = '\\'.join(component.rstrip('. ') for component in text.split('\\'))
            return PureWindowsPath('\\\\?\\UNC\\' + text[2:] if text.startswith('\\\\')
                                   else '\\\\?\\' + text)

        with self.windows_paths() as WindowsPath:
            for root in (r'\\?\C:\qualification', r'\\?\UNC\server\share'):
                for suffix in ('.', ' '):
                    with self.subTest(root=root, suffix=suffix):
                        literal = WindowsPath(root + r'\client.exe' + suffix)
                        twin = WindowsPath(root + r'\client.exe')
                        self.assertNotEqual(
                            handle_path(runner.path_without_extended_prefix(literal)),
                            handle_path(runner.path_without_extended_prefix(twin)))

    def test_extended_path_length_limit_counts_utf16_and_terminator(self):
        with self.windows_paths():
            for root in ('C:\\', '\\\\server\\share\\'):
                prefix = '\\\\?\\' if root[1] == ':' else '\\\\?\\UNC\\'
                for length in (259, 260, 261):
                    for character in ('x', '\U0001f332'):
                        # Keep components below the filesystem's name limit.
                        ordinary = root + 'a' * 120 + '\\'
                        remaining = length - len(ordinary)
                        units = len(character.encode('utf-16-le')) // 2
                        ordinary += character * (remaining // units) + 'x' * (remaining % units)
                        extended = prefix + (ordinary if root[1] == ':' else ordinary[2:])
                        with self.subTest(root=root, length=length, character=character):
                            self.assertEqual(runner.path_without_extended_prefix(extended),
                                             ordinary if length < 260 else extended)
                            if length >= 260:
                                with self.assertRaisesRegex(OSError, 'safely normalized'):
                                    runner.resolve_path(extended)

    def test_unsafe_executables_are_refused_before_output_mutation(self):
        with self.windows_paths() as WindowsPath:
            for root in (r'\\?\C:\qualification', r'\\?\UNC\server\share'):
                for component in ('client.exe.', 'client.exe ', 'CON.exe'):
                    extended = root + '\\' + component
                    with mock.patch.object(runner.shutil, 'which', return_value=extended):
                        for program in (extended, 'client.exe'):
                            with self.subTest(path=extended, program=program), \
                                    mock.patch.object(runner, '_output_locks') as locks, \
                                    mock.patch.object(runner, '_run_process') as run:
                                with self.assertRaisesRegex(OSError, 'safely normalized'):
                                    runner.run_process(
                                        [program], cwd=WindowsPath(root), environment={},
                                        artifact='a.json', retained='r.json',
                                        receipt_path='receipt.json', log='log.txt')
                                locks.assert_not_called()
                                run.assert_not_called()

    def test_unsafe_outputs_and_resolved_parents_refuse_before_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / 'evidence.json'
            evidence.write_bytes(b'old evidence')
            before = (evidence.read_bytes(), evidence.stat().st_ino)
            with self.windows_paths() as WindowsPath:
                for root in (r'\\?\C:\qualification', r'\\?\UNC\server\share'):
                    for component in ('evidence.', 'evidence ', 'NUL.json', 'x' * 250):
                        unsafe = WindowsPath(root) / component
                        for alias in (False, True):
                            output = (WindowsPath(r'C:\alias\capture.json') if alias
                                      else unsafe / 'capture.json')
                            with self.subTest(path=unsafe, alias=alias), \
                                    mock.patch.object(WindowsPath, 'resolve',
                                                      return_value=unsafe), \
                                    mock.patch.object(runner, '_open_sidecar') as opened, \
                                    mock.patch.object(runner, '_run_process') as run:
                                with self.assertRaisesRegex(
                                        OSError, 'safely normalized|dot or space|device name'):
                                    runner.run_process(
                                        [r'C:\client.exe'], cwd=WindowsPath('C:\\'),
                                        environment={}, artifact=output, retained='r.json',
                                        receipt_path='receipt.json', log='log.txt')
                                opened.assert_not_called()
                                run.assert_not_called()
                                self.assertEqual((evidence.read_bytes(), evidence.stat().st_ino),
                                                 before)

    def test_windows_executable_paths_use_ordinary_drive_and_unc_spelling(self):
        class WindowsPath(PureWindowsPath):
            def is_file(self):
                return True

        windows = types.SimpleNamespace(**vars(os))
        windows.name, windows.sep, windows.altsep = 'nt', '\\', '/'
        windows.path, windows.pathsep = ntpath, ';'
        for ordinary, extended in (
                (r'C:\qualification\client.exe', r'\\?\C:\qualification\client.exe'),
                (r'\\server\share\client.exe', r'\\?\UNC\server\share\client.exe')):
            with self.subTest(path=ordinary), \
                    mock.patch.object(runner, 'os', windows), \
                    mock.patch.object(runner, 'Path', WindowsPath), \
                    mock.patch.object(runner.shutil, 'which', return_value=extended):
                for program in (extended, 'client.exe'):
                    self.assertEqual(str(runner.resolve_executable(
                        program, WindowsPath(ordinary).parent)), ordinary)

    def test_resolved_windows_prefixes_do_not_change_path_identity(self):
        class WindowsPath(PureWindowsPath):
            def resolve(self):
                # Model handle resolution expanding a short directory name.
                return type(self)(str(self).replace('RUNNER~1', 'Runner Account'))

        windows = types.SimpleNamespace(**vars(os))
        windows.name = 'nt'
        for ordinary, extended in (
                (r'C:\RUNNER~1\evidence', r'\\?\C:\RUNNER~1\evidence'),
                (r'\\server\share\evidence', r'\\?\UNC\server\share\evidence')):
            with self.subTest(path=ordinary), \
                    mock.patch.object(runner, 'os', windows), \
                    mock.patch.object(runner, 'Path', WindowsPath):
                expected = WindowsPath(ordinary.replace('RUNNER~1', 'Runner Account'))
                self.assertEqual(runner.path_without_extended_prefix(WindowsPath(extended)),
                                 WindowsPath(ordinary))
                self.assertEqual(runner.resolve_path(extended), expected)
                self.assertEqual(runner.resolve_path(ordinary), expected)
                identity = runner._output_identity(WindowsPath(extended) / 'Capture.json')
                self.assertEqual(str(identity), str(expected / 'capture.json'))
                self.assertEqual(identity, runner._output_identity(
                    WindowsPath(ordinary) / 'Capture.json'))
                with self.assertRaisesRegex(OSError, 'output overlaps a lock sidecar'):
                    runner._output_identity(WindowsPath(extended) / 'Capture.json.LOCK')

    def test_resolution_preserves_other_namespaces_and_posix_names(self):
        for platform, path in (
                ('nt', PureWindowsPath(r'\\?\Volume{123}\evidence')),
                ('posix', PurePosixPath('/evidence/ordinary')),
                ('posix', PurePosixPath('/evidence/\\\\?\\literal'))):
            system = types.SimpleNamespace(**vars(os))
            system.name = platform
            with self.subTest(platform=platform, path=path), \
                    mock.patch.object(runner, 'os', system), \
                    mock.patch.object(runner, 'Path') as constructor:
                constructor.return_value.resolve.return_value = path
                self.assertEqual(runner.resolve_path('input'), path)
                constructor.return_value.resolve.assert_called_once_with()


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
        # Explicitly allow the CLI's sibling modules in the isolated interpreter.
        bootstrap = ('import runpy, sys; from pathlib import Path; '
                     'script = sys.argv[1]; '
                     'sys.path.insert(0, str(Path(script).parent)); '
                     'sys.argv = sys.argv[1:]; runpy.run_path(script, run_name="__main__")')
        command = [sys.executable, '-I', '-B', '-c', bootstrap, str(PLANNER_SCRIPT),
                   '--client', '/bin/client',
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
        return [sys.executable, '-I', '-B', str(path), str(self.artifact)]

    def run_case(self, command, timeout_seconds=60):
        return runner.run_process(command, cwd=self.tmp, environment=os.environ.copy(),
                                  log=self.log, artifact=self.artifact,
                                  retained=self.retained, receipt_path=self.receipt,
                                  timeout_seconds=timeout_seconds)

    def read_receipt(self):
        return json.loads(self.receipt.read_text(encoding='utf-8'))

    def assert_output_locks_released(self):
        with runner._output_locks((self.artifact, self.retained, self.receipt, self.log)):
            pass

    @contextlib.contextmanager
    def output_lock_owners(self):
        owners = {}
        open_sidecar = runner._open_sidecar

        def open_lock(path):
            handle = open_sidecar(path)
            # A contender's handle must not replace the still-active owner.
            if path not in owners or owners[path].closed:
                owners[path] = handle
            return handle

        with mock.patch.object(runner, '_open_sidecar', side_effect=open_lock):
            yield owners

    def read_with_lock_owner(self, path, owners):
        owner = owners.get(path)
        if owner is None or owner.closed:
            return path.read_bytes()
        # Windows mandatory byte locks also apply to other handles in this process.
        owner.seek(0)
        return owner.read()

    def test_temporary_root_matches_resolved_sidecar_paths(self):
        identity = runner._output_identity(self.artifact)
        self.assertEqual(self.tmp, identity.parent)
        self.assertEqual(Path(str(identity) + '.lock'),
                         Path(str(self.artifact) + '.lock'))

    def test_sidecar_handles_share_access_while_byte_lock_contends(self):
        sidecar = Path(str(self.artifact) + '.lock')
        sidecar.write_bytes(b'held sidecar')
        with self.output_lock_owners() as owners, runner._output_locks((self.artifact,)):
            with runner._open_sidecar(sidecar) as contender:
                with self.assertRaises(FileExistsError):
                    runner._lock_handle(contender, sidecar)
            self.assertEqual(self.read_with_lock_owner(sidecar, owners), b'held sidecar')
        self.assertEqual(sidecar.read_bytes(), b'held sidecar')
        self.assert_output_locks_released()

    def test_atomic_publication_replaces_existing_files_without_rename(self):
        self.seed_old_evidence()
        with mock.patch.object(runner.os, 'rename', side_effect=FileExistsError(
                'destination already exists')), \
                mock.patch.object(runner.os, 'replace', wraps=os.replace) as replace:
            for path in (self.retained, self.receipt):
                runner.atomic_write(path, b'current evidence')
                self.assertEqual(path.read_bytes(), b'current evidence')
        self.assertEqual(replace.call_count, 2)
        self.assertFalse(list(self.tmp.glob('*.tmp')))

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

    def test_injected_timeout_raises_and_receipt_shows_timed_out(self):
        command = self.script('')
        error = subprocess.TimeoutExpired(command, 1)

        def timeout(argv, **kwargs):
            self.assertEqual(kwargs['timeout'], 1)
            kwargs['stdout'].write(b'x' * 20000 + b'\nEnsureSurfaceReadyNear: draining\n')
            kwargs['stdout'].flush()
            raise error

        diagnostic = io.StringIO()
        with contextlib.redirect_stderr(diagnostic), \
                mock.patch.object(runner.subprocess, 'run', side_effect=timeout), \
                self.assertRaises(subprocess.TimeoutExpired) as caught:
            self.run_case(command, timeout_seconds=1)
        self.assertIs(caught.exception, error)
        receipt = self.read_receipt()
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['timeout_seconds'], 1)
        self.assertEqual(receipt['last_observed_phase'],
                         'EnsureSurfaceReadyNear: draining')
        self.assertEqual(len(receipt['log_tail'].encode('utf-8')),
                         runner.TIMEOUT_LOG_TAIL_BYTES)
        self.assertIn('timed out after 1 s', diagnostic.getvalue())
        self.assertIn('EnsureSurfaceReadyNear: draining', diagnostic.getvalue())

    def test_timeout_raises_and_receipt_shows_timed_out(self):
        ready = self.tmp / 'ready'
        command = self.script(
            'import sys, time\nfrom pathlib import Path\n'
            'Path(sys.argv[1]).write_bytes(b\'{"complete":true}\')\n'
            'print("x" * 20000, flush=True)\n'
            'print("EnsureSurfaceReadyNear: draining", flush=True)\n'
            'Path(sys.argv[2]).touch()\n'
            'while True: time.sleep(1)\n') + [str(ready)]
        popen = subprocess.Popen
        children = []

        def start(*args, **kwargs):
            child = popen(*args, **kwargs)
            children.append(child)
            wait = child.wait

            def wait_for_timeout(timeout=None):
                if timeout is not None:
                    # This bound detects a deadlocked fixture; readiness, rather
                    # than interpreter startup speed, determines when to time out.
                    deadline = time.monotonic() + 60
                    while not ready.exists():
                        self.assertIsNone(child.poll(), 'child exited before capture')
                        self.assertLess(time.monotonic(), deadline, 'child never became ready')
                        time.sleep(0.01)
                    self.assertIsNone(child.poll(), 'child must still be running')
                    return wait(timeout=0)
                return wait()

            child.wait = wait_for_timeout
            child.kill = mock.Mock(wraps=child.kill)
            return child

        diagnostic = io.StringIO()
        with contextlib.redirect_stderr(diagnostic), \
                mock.patch.object(runner.subprocess, 'Popen', side_effect=start), \
                self.assertRaises(subprocess.TimeoutExpired):
            self.run_case(command, timeout_seconds=1)
        self.assertEqual(len(children), 1)
        children[0].kill.assert_called_once_with()
        self.assertIsNotNone(children[0].returncode, 'timed out child was not reaped')
        receipt = self.read_receipt()
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['timeout_seconds'], 1)
        self.assertEqual(receipt['last_observed_phase'], 'EnsureSurfaceReadyNear: draining')
        self.assertEqual(len(receipt['log_tail'].encode('utf-8')), runner.TIMEOUT_LOG_TAIL_BYTES)
        self.assertIn('timed out after 1 s', diagnostic.getvalue())
        self.assertIn('EnsureSurfaceReadyNear: draining', diagnostic.getvalue())
        self.assertEqual(self.retained.read_bytes(), b'{"complete":true}')
        self.assertEqual(receipt['capture_status'], 'retained')
        self.assertEqual(receipt['capture_sha256'], runner.file_sha256(self.retained))
        self.assert_output_locks_released()

    def test_interleaved_publication_refuses_competing_invocation(self):
        command = self.script('')
        outer_payload = b'{"invocation":"outer"}'
        original_write = runner.atomic_write
        observed = []
        interleaved = False
        launches = 0

        def emit_capture(argv, **kwargs):
            nonlocal launches
            launches += 1
            self.artifact.write_bytes(outer_payload if launches == 1
                                      else b'{"invocation":"inner"}')
            return subprocess.CompletedProcess(argv, 0)

        def interleave(path, payload):
            nonlocal interleaved
            original_write(path, payload)
            if path == self.retained and not interleaved:
                interleaved = True
                # Inject the competing invocation exactly between capture and
                # receipt publication, without scheduling or clock assumptions.
                before = {p: p.read_bytes() for p in
                          (self.artifact, self.retained, self.log)}
                with self.assertRaises(FileExistsError):
                    self.run_case(command)
                self.assertEqual(before, {p: p.read_bytes() for p in before})
                self.assertFalse(self.receipt.exists())
                observed.append('refused')

        with mock.patch.object(runner.subprocess, 'run', side_effect=emit_capture) as launch, \
                mock.patch.object(runner, 'atomic_write', side_effect=interleave):
            self.run_case(command)
        self.assertEqual(launch.call_count, 1)
        self.assertEqual(observed, ['refused'])
        receipt = self.read_receipt()
        self.assertEqual(Path(receipt['capture_path']).read_bytes(), outer_payload)
        self.assertEqual(receipt['capture_sha256'], runner.file_sha256(self.retained))
        self.assert_output_locks_released()
        # Normal repeated invocations still replace the current evidence.
        self.run_case(self.script('from pathlib import Path\nimport sys\n'
                                  'Path(sys.argv[1]).write_text("{}")\n'))
        self.assertEqual(self.retained.read_bytes(), b'{}')

    def test_each_shared_output_is_locked_before_spawn_and_aliases_agree(self):
        command = self.script('')
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        for output in outputs:
            Path(str(output) + '.lock').write_bytes(b'held sidecar')
        sidecars_before = {Path(str(output) + '.lock'): Path(str(output) + '.lock').read_bytes()
                           for output in outputs}

        def interleave(argv, **kwargs):
            for shared in outputs:
                with self.subTest(shared=shared.name):
                    # Use distinct other outputs and an aliased shared path.
                    inner = [self.tmp / ('inner-' + p.name) for p in outputs]
                    inner[outputs.index(shared)] = shared.parent / '..' / self.tmp.name / shared.name
                    other = [p for i, p in enumerate(inner) if i != outputs.index(shared)]
                    before = {p: sidecars_before[p] if p in sidecars_before else p.read_bytes()
                              for p in self.tmp.iterdir() if p.is_file()}
                    with self.assertRaises(FileExistsError):
                        runner.run_process(command, cwd=self.tmp, environment=os.environ.copy(),
                                           artifact=inner[0], retained=inner[1],
                                           receipt_path=inner[2], log=inner[3])
                    self.assertEqual(before, {
                        p: self.read_with_lock_owner(p, owners) for p in before})
                    self.assertFalse(any(p.exists() for p in other))
                    # Any locks acquired before the conflict must be released.
                    with runner._output_locks(other):
                        pass
            return subprocess.CompletedProcess(argv, 0)

        with self.output_lock_owners() as owners, \
                mock.patch.object(runner.subprocess, 'run', side_effect=interleave) as launch:
            self.run_case(command)
        self.assertEqual(launch.call_count, 1)
        self.assert_output_locks_released()

    def seed_old_evidence(self):
        for path in (self.artifact, self.retained, self.receipt, self.log):
            path.write_text('{"stale":true}', encoding='utf-8')

    def test_overlapping_outputs_are_refused_before_mutation(self):
        command = self.script('')
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        self.seed_old_evidence()
        for path in outputs:
            Path(str(path) + '.lock').write_bytes(b'old lock file')
        nested = self.tmp / 'nested'
        nested.mkdir()
        before = {p: (p.read_bytes(), p.stat().st_ino)
                  for p in self.tmp.iterdir() if p.is_file()}
        for source in range(len(outputs)):
            for destination in range(len(outputs)):
                if source == destination:
                    continue
                for suffix, message in (('', 'output paths must be distinct'),
                                        ('.lock', 'output overlaps a lock sidecar')):
                    for aliased in (False, True):
                        with self.subTest(source=source, destination=destination,
                                          suffix=suffix, aliased=aliased):
                            collision = Path(str(outputs[source]) + suffix)
                            if aliased:
                                collision = nested / '..' / collision.name
                            inner = list(outputs)
                            inner[destination] = collision
                            with mock.patch.object(Path, 'open') as opened, \
                                    mock.patch.object(Path, 'unlink') as unlinked, \
                                    mock.patch.object(runner.subprocess, 'run') as launch, \
                                    self.assertRaisesRegex(OSError, message):
                                runner.run_process(
                                    command, cwd=self.tmp, environment=os.environ.copy(),
                                    artifact=inner[0], retained=inner[1],
                                    receipt_path=inner[2], log=inner[3])
                            opened.assert_not_called()
                            unlinked.assert_not_called()
                            launch.assert_not_called()
                            self.assertEqual(before, {
                                p: (p.read_bytes(), p.stat().st_ino)
                                for p in self.tmp.iterdir() if p.is_file()})

    def test_sidecar_collision_preserves_held_lock_and_reusable_file(self):
        command = self.script('')
        sidecar = Path(str(self.artifact) + '.lock')
        with runner._output_locks((self.artifact,)):
            identity = sidecar.stat().st_ino
            with mock.patch.object(runner.subprocess, 'run') as launch, \
                    self.assertRaisesRegex(OSError, 'output overlaps a lock sidecar'):
                runner.run_process(command, cwd=self.tmp, environment=os.environ.copy(),
                                   artifact=self.artifact, retained=sidecar,
                                   receipt_path=self.receipt, log=self.log)
            launch.assert_not_called()
            self.assertEqual(sidecar.stat().st_ino, identity)
            with self.assertRaises(FileExistsError):
                with runner._output_locks((self.artifact,)):
                    self.fail('a competing invocation acquired the held output lock')
        self.assertEqual(sidecar.stat().st_ino, identity)
        with runner._output_locks((self.artifact,)):
            self.assertEqual(sidecar.stat().st_ino, identity)
        self.assertEqual(sidecar.stat().st_ino, identity)

    def test_cross_invocation_sidecar_output_is_refused_before_mutation(self):
        command = self.script('')
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        sidecar = Path(str(self.artifact) + '.lock')
        sidecar.write_bytes(b'held sidecar')
        self.seed_old_evidence()
        nested = self.tmp / 'nested'
        nested.mkdir()
        before = {p: (p.read_bytes(), p.stat().st_ino)
                  for p in self.tmp.iterdir() if p.is_file()}
        with self.output_lock_owners() as owners, runner._output_locks((self.artifact,)):
            for destination in range(len(outputs)):
                for aliased in (False, True):
                    with self.subTest(destination=destination, aliased=aliased):
                        inner = [self.tmp / ('other-' + p.name) for p in outputs]
                        inner[destination] = (nested / '..' / sidecar.name
                                              if aliased else sidecar)
                        self.assertNotIn(self.artifact, inner)
                        with mock.patch.object(Path, 'open', side_effect=AssertionError(
                                'output refusal must precede file opens')) as opened, \
                                mock.patch.object(Path, 'unlink') as unlinked, \
                                mock.patch.object(runner.subprocess, 'run') as launch, \
                                self.assertRaisesRegex(OSError, 'output overlaps a lock sidecar'):
                            runner.run_process(
                                command, cwd=self.tmp, environment=os.environ.copy(),
                                artifact=inner[0], retained=inner[1],
                                receipt_path=inner[2], log=inner[3])
                        opened.assert_not_called()
                        unlinked.assert_not_called()
                        launch.assert_not_called()
                        self.assertEqual(before, {
                            p: (self.read_with_lock_owner(p, owners), p.stat().st_ino)
                            for p in self.tmp.iterdir() if p.is_file()})
                        with self.assertRaises(FileExistsError):
                            with runner._output_locks((self.artifact,)):
                                self.fail('a competing invocation acquired the held output lock')
        self.assertEqual((sidecar.read_bytes(), sidecar.stat().st_ino), before[sidecar])
        with self.output_lock_owners() as owners, runner._output_locks((self.artifact,)):
            self.assertEqual((self.read_with_lock_owner(sidecar, owners), sidecar.stat().st_ino),
                             before[sidecar])
        self.assertEqual((sidecar.read_bytes(), sidecar.stat().st_ino), before[sidecar])

    def test_sidecar_suffix_is_reserved_without_an_existing_owner(self):
        command = self.script('')
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        self.seed_old_evidence()
        before = {p: (p.read_bytes(), p.stat().st_ino) for p in self.tmp.iterdir()}
        for destination in range(len(outputs)):
            for suffix in ('.lock', '.lock.lock', '.LOCK'):
                with self.subTest(destination=destination, suffix=suffix):
                    inner = list(outputs)
                    inner[destination] = Path(str(inner[destination]) + suffix)
                    with mock.patch.object(Path, 'open') as opened, \
                            mock.patch.object(Path, 'unlink') as unlinked, \
                            mock.patch.object(runner.subprocess, 'run') as launch, \
                            self.assertRaisesRegex(OSError, 'output overlaps a lock sidecar'):
                        runner.run_process(
                            command, cwd=self.tmp, environment=os.environ.copy(),
                            artifact=inner[0], retained=inner[1],
                            receipt_path=inner[2], log=inner[3])
                    opened.assert_not_called()
                    unlinked.assert_not_called()
                    launch.assert_not_called()
                    self.assertEqual(before, {
                        p: (p.read_bytes(), p.stat().st_ino) for p in self.tmp.iterdir()})

    def test_symlink_sidecars_are_refused_before_any_open_or_mutation(self):
        command = self.script('')
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        self.seed_old_evidence()
        target = self.tmp / 'alias.json'
        for exists in (False, True):
            if exists:
                target.write_bytes(b'held inode')
            for output in outputs:
                with self.subTest(output=output.name, target_exists=exists):
                    sidecar = Path(str(output) + '.lock')
                    sidecar.symlink_to(target)
                    before = {p: (p.read_bytes(), p.stat().st_ino)
                              for p in self.tmp.iterdir() if not p.is_symlink()}
                    with mock.patch.object(runner.os, 'open') as opened, \
                            mock.patch.object(Path, 'unlink') as unlinked, \
                            mock.patch.object(runner.subprocess, 'run') as launch, \
                            self.assertRaisesRegex(OSError, 'lock sidecar must be a regular file'):
                        self.run_case(command)
                    opened.assert_not_called()
                    unlinked.assert_not_called()
                    launch.assert_not_called()
                    self.assertTrue(sidecar.is_symlink())
                    self.assertEqual(runner.path_without_extended_prefix(sidecar.readlink()), target)
                    self.assertEqual(before, {
                        p: (p.read_bytes(), p.stat().st_ino)
                        for p in self.tmp.iterdir() if not p.is_symlink()})
                    self.assertEqual(list(self.tmp.glob('*.lock')), [sidecar])
                    sidecar.unlink()

    def test_nonregular_sidecars_are_refused_before_any_open_or_mutation(self):
        command = self.script('')
        self.seed_old_evidence()
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        before = {p: (p.read_bytes(), p.stat().st_ino) for p in self.tmp.iterdir()}
        lstat = Path.lstat
        for output in outputs:
            sidecar = Path(str(output) + '.lock')
            for kind in (stat.S_IFDIR, stat.S_IFIFO, stat.S_IFSOCK,
                         stat.S_IFCHR, stat.S_IFBLK):
                with self.subTest(output=output.name, kind=kind):
                    def metadata(path, *args, **kwargs):
                        if path == sidecar:
                            return types.SimpleNamespace(st_mode=kind, st_nlink=1)
                        return lstat(path, *args, **kwargs)

                    with mock.patch.object(Path, 'lstat', metadata), \
                            mock.patch.object(runner.os, 'open') as opened, \
                            mock.patch.object(Path, 'unlink') as unlinked, \
                            mock.patch.object(runner.subprocess, 'run') as launch, \
                            self.assertRaisesRegex(OSError, 'lock sidecar must be a regular file'):
                        self.run_case(command)
                    opened.assert_not_called()
                    unlinked.assert_not_called()
                    launch.assert_not_called()
                    self.assertEqual(before, {
                        p: (p.read_bytes(), p.stat().st_ino) for p in self.tmp.iterdir()})

    def test_symlink_sidecar_alias_cannot_unlink_another_invocations_lock(self):
        command = self.script('')
        self.seed_old_evidence()
        alias = self.tmp / 'alias.json'
        alias.write_bytes(b'{"held":true}')
        sidecar = Path(str(self.artifact) + '.lock')
        sidecar.symlink_to(alias)
        before = {p: (p.read_bytes(), p.stat().st_ino) for p in self.tmp.iterdir()}
        # Model an already active owner from before symlink acquisition was
        # refused. POSIX permits its real exclusive lock on a read-only handle.
        with sidecar.open('r+b' if os.name == 'nt' else 'rb') as owner:
            runner._lock_handle(owner, sidecar)
            try:
                held = os.fstat(owner.fileno())
                self.assertEqual(held.st_ino, alias.stat().st_ino)
                inner = [self.tmp / ('other-' + p.name) for p in
                         (self.artifact, self.retained, self.receipt, self.log)]
                for destination in range(len(inner)):
                    with self.subTest(destination=destination):
                        outputs = list(inner)
                        outputs[destination] = alias
                        with mock.patch.object(runner, '_open_sidecar') as opened, \
                                mock.patch.object(Path, 'unlink') as unlinked, \
                                mock.patch.object(runner.subprocess, 'run') as launch, \
                                self.assertRaises(FileExistsError):
                            runner.run_process(
                                command, cwd=self.tmp, environment=os.environ.copy(),
                                artifact=outputs[0], retained=outputs[1],
                                receipt_path=outputs[2], log=outputs[3])
                        opened.assert_not_called()
                        unlinked.assert_not_called()
                        launch.assert_not_called()
                        # Windows requires reads of the locked byte to use its owner.
                        owner.seek(0)
                        held_contents = owner.read()
                        self.assertEqual(before, {
                            p: (held_contents if p in (alias, sidecar) else p.read_bytes(),
                                p.stat().st_ino) for p in self.tmp.iterdir()})
                        self.assertTrue(sidecar.is_symlink())
                        with self.assertRaisesRegex(OSError, 'lock sidecar must be a regular file'):
                            with runner._output_locks((self.artifact,)):
                                self.fail('a third invocation acquired a replacement lock')
                        with alias.open('r+b' if os.name == 'nt' else 'rb') as contender:
                            with self.assertRaises(FileExistsError):
                                runner._lock_handle(contender, alias)
            finally:
                if os.name == 'nt':
                    owner.seek(0)
                    runner.msvcrt.locking(owner.fileno(), runner.msvcrt.LK_UNLCK, 1)
        self.assertEqual((alias.read_bytes(), alias.stat().st_ino), before[alias])
        sidecar.unlink()
        with runner._output_locks((self.artifact,)):
            self.assertFalse(sidecar.is_symlink())

    def test_hardlinked_sidecars_and_outputs_are_refused_before_mutation(self):
        command = self.script('')
        self.seed_old_evidence()
        alias = self.tmp / 'alias.json'
        alias.hardlink_to(self.artifact)
        with mock.patch.object(runner.os, 'open') as opened, \
                mock.patch.object(Path, 'unlink') as unlinked, \
                self.assertRaisesRegex(OSError, 'output must be a regular file with one link'):
            self.run_case(command)
        opened.assert_not_called()
        unlinked.assert_not_called()
        alias.unlink()
        sidecar = Path(str(self.artifact) + '.lock')
        sidecar.write_bytes(b'held inode')
        alias.hardlink_to(sidecar)
        with mock.patch.object(runner.os, 'open') as opened, \
                mock.patch.object(Path, 'unlink') as unlinked, \
                self.assertRaisesRegex(OSError, 'lock sidecar must be a regular file with one link'):
            self.run_case(command)
        opened.assert_not_called()
        unlinked.assert_not_called()
        self.assertEqual(sidecar.read_bytes(), b'held inode')
        self.assertEqual(alias.stat().st_ino, sidecar.stat().st_ino)

    def test_sidecar_open_refuses_symlink_installation_races(self):
        sidecar = Path(str(self.artifact) + '.lock')
        target = self.tmp / 'alias.json'
        target.write_bytes(b'untouched')
        for exists in (False, True):
            for nofollow in (0, getattr(os, 'O_NOFOLLOW', 0)):
                with self.subTest(existing_sidecar=exists, nofollow=nofollow):
                    if exists:
                        sidecar.write_bytes(b'original')
                    system = types.SimpleNamespace(**vars(os))
                    system.O_NOFOLLOW = nofollow

                    def replace_before_open(path, flags, mode):
                        self.assertEqual(path, sidecar)
                        self.assertEqual(flags & nofollow, nofollow)
                        sidecar.unlink(missing_ok=True)
                        sidecar.symlink_to(target)
                        return os.open(path, flags, mode)

                    system.open = replace_before_open
                    with mock.patch.object(runner, 'os', system), self.assertRaises(OSError):
                        runner._open_sidecar(sidecar)
                    self.assertTrue(sidecar.is_symlink())
                    self.assertEqual(target.read_bytes(), b'untouched')
                    sidecar.unlink()

    def test_sidecar_open_compares_handle_with_both_lstat_results(self):
        sidecar = Path(str(self.artifact) + '.lock')
        backup = self.tmp / 'original'
        for replace_before_open in (False, True):
            with self.subTest(replace_before_open=replace_before_open):
                sidecar.write_bytes(b'original')
                system = types.SimpleNamespace(**vars(os))
                system.O_NOFOLLOW = 0  # Exercise the Windows fallback too.
                descriptors = []

                def open_replacement(path, flags, mode):
                    self.assertEqual(path, sidecar)
                    sidecar.rename(backup)
                    sidecar.write_bytes(b'replacement')
                    # Open the original inode via its new name to model a handle
                    # surviving replacement without renaming an open file on Windows.
                    descriptor = os.open(path if replace_before_open else backup, flags, mode)
                    descriptors.append(descriptor)
                    return descriptor

                system.open = open_replacement
                with mock.patch.object(runner, 'os', system), \
                        self.assertRaisesRegex(OSError, 'lock sidecar changed during acquisition'):
                    runner._open_sidecar(sidecar)
                self.assertEqual(len(descriptors), 1)
                with self.assertRaises(OSError):
                    os.fstat(descriptors[0])
                self.assertEqual(backup.read_bytes(), b'original')
                self.assertEqual(sidecar.read_bytes(), b'replacement')
                sidecar.unlink()
                backup.unlink()

    def assert_output_names_refused(self, outputs, message):
        command = self.script('')
        self.seed_old_evidence()
        for path in (self.artifact, self.retained, self.receipt, self.log):
            Path(str(path) + '.lock').write_bytes(b'reusable sidecar')
        before = {p: (p.read_bytes(), p.stat().st_ino) for p in self.tmp.iterdir()}
        with mock.patch.object(Path, 'open', side_effect=AssertionError(
                'output refusal must precede file opens')) as opened, \
                mock.patch.object(Path, 'unlink') as unlinked, \
                mock.patch.object(runner.subprocess, 'run') as launch, \
                self.assertRaisesRegex(OSError, message):
            runner.run_process(command, cwd=self.tmp, environment=os.environ.copy(),
                               artifact=outputs[0], retained=outputs[1],
                               receipt_path=outputs[2], log=outputs[3])
        opened.assert_not_called()
        unlinked.assert_not_called()
        launch.assert_not_called()
        self.assertEqual(before, {
            p: (p.read_bytes(), p.stat().st_ino) for p in self.tmp.iterdir()})

    def test_trailing_dots_and_spaces_are_refused_before_mutation(self):
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        for destination in range(len(outputs)):
            for suffix in ('.', ' ', '.lock.', '.lock ', '.LOCK. '):
                with self.subTest(destination=destination, suffix=suffix):
                    inner = list(outputs)
                    inner[destination] = Path(str(self.artifact) + suffix)
                    self.assert_output_names_refused(
                        inner, 'output path component ends in a dot or space')

    def test_trailing_dot_duplicate_is_refused_before_mutation(self):
        self.assert_output_names_refused(
            (self.artifact, Path(str(self.artifact) + '.'), self.receipt, self.log),
            'output path component ends in a dot or space')

    def test_windows_stream_and_device_names_are_refused_before_mutation(self):
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        for name in ('capture.json:stream', 'capture.json.lock:stream',
                     'capture.json::$DATA', 'CON', 'con.json', 'NUL', 'nul .json',
                     'PRN.txt', 'aux', 'COM1.json', 'com9', 'LPT1', 'lpt9.txt',
                     'COM\u00b9.json', 'LPT\u00b2', 'com\u00b3', 'CONIN$', 'conout$.txt'):
            for destination in range(len(outputs)):
                with self.subTest(name=name, destination=destination):
                    inner = list(outputs)
                    inner[destination] = self.tmp / name
                    self.assert_output_names_refused(
                        inner, 'output path contains (an invalid Windows character|'
                        'a reserved Windows device name)')

    def test_unsafe_parent_components_are_refused_before_resolution(self):
        for component, message in (
                ('nested.', 'ends in a dot or space'),
                ('nested ', 'ends in a dot or space'),
                ('nested:stream', 'invalid Windows character'),
                ('NUL.json', 'reserved Windows device name'),
                ('nested\x01', 'invalid Windows character'),
                ('nested?', 'invalid Windows character')):
            for remove_component in (False, True):
                with self.subTest(component=component, remove_component=remove_component):
                    parent = self.tmp / component
                    if remove_component:
                        parent = parent / '..'
                    self.assert_output_names_refused(
                        (parent / self.artifact.name, self.retained, self.receipt, self.log),
                        message)

    def test_casefolded_duplicate_outputs_are_refused_before_mutation(self):
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        for source in range(len(outputs)):
            for destination in range(len(outputs)):
                if source == destination:
                    continue
                with self.subTest(source=source, destination=destination):
                    inner = list(outputs)
                    inner[destination] = outputs[source].with_name(outputs[source].name.upper())
                    self.assert_output_names_refused(inner, 'output paths must be distinct')

    def test_casefolded_output_names_contend_on_reusable_sidecars(self):
        for output in (self.artifact, self.retained, self.receipt, self.log):
            with self.subTest(output=output.name):
                sidecar = Path(str(output) + '.lock')
                sidecar.write_bytes(b'reusable sidecar')
                before = (sidecar.read_bytes(), sidecar.stat().st_ino)
                alias = output.with_name(output.name.upper())
                # Either spelling must use the same sidecar, including when the
                # uppercase invocation is the first owner on a sensitive filesystem.
                for owner, contender in ((output, alias), (alias, output)):
                    with runner._output_locks((owner,)):
                        with self.assertRaises(FileExistsError):
                            with runner._output_locks((contender,)):
                                self.fail('a competing invocation acquired the held output lock')
                    self.assertEqual((sidecar.read_bytes(), sidecar.stat().st_ino), before)
                self.assertEqual([p for p in self.tmp.iterdir()
                                  if p.name.casefold() == sidecar.name], [sidecar])

    def test_windows_locks_are_unlocked_before_close(self):
        for failure in (None, 'body', 'contention', 'acquisition'):
            with self.subTest(failure=failure):
                events = []
                handles = []
                error = OSError(runner.errno.EACCES if failure == 'contention'
                                else runner.errno.EIO, 'fixture failure')
                windows = types.SimpleNamespace(**vars(os))
                windows.name = 'nt'
                locking = mock.Mock(LK_NBLCK=1, LK_UNLCK=2)
                open_sidecar = runner._open_sidecar
                descriptors = {}

                def open_lock(path):
                    descriptor = len(handles) + 1
                    original = open_sidecar(path)
                    descriptors[original.fileno()] = descriptor
                    handle = mock.Mock(wraps=original)

                    def seek(offset):
                        events.append(('seek', descriptor, offset))
                        return original.seek(offset)

                    def close():
                        events.append(('close', descriptor))
                        original.close()

                    handle.seek.side_effect = seek
                    handle.close.side_effect = close
                    handles.append(handle)
                    events.append(('open', descriptor, 'r+b'))
                    return handle

                def lock(fileno, operation, length):
                    descriptor = descriptors[fileno]
                    events.append(('lock', descriptor, operation, length))
                    if descriptor == 2 and operation == locking.LK_NBLCK \
                            and failure in ('contention', 'acquisition'):
                        raise error

                locking.locking.side_effect = lock
                with mock.patch.object(runner, 'os', windows), \
                        mock.patch.object(runner, 'msvcrt', locking, create=True), \
                        mock.patch.object(runner, '_open_sidecar', side_effect=open_lock):
                    def acquire():
                        with runner._output_locks((self.artifact, self.retained)):
                            events.append(('body',))
                            if failure == 'body':
                                raise error

                    if failure is None:
                        acquire()
                    else:
                        expected = FileExistsError if failure == 'contention' else OSError
                        with self.assertRaises(expected) as caught:
                            acquire()
                        if failure != 'contention':
                            self.assertIs(caught.exception, error)

                expected = [('open', 1, 'r+b'), ('seek', 1, 0), ('lock', 1, 1, 1),
                            ('open', 2, 'r+b'), ('seek', 2, 0), ('lock', 2, 1, 1)]
                if failure in (None, 'body'):
                    expected.extend([('body',), ('seek', 2, 0), ('lock', 2, 2, 1)])
                expected.extend([('close', 2), ('seek', 1, 0), ('lock', 1, 2, 1),
                                 ('close', 1)])
                self.assertEqual(events, expected)
                for handle in handles:
                    handle.close.assert_called_once_with()

    def test_symlink_parent_aliases_contend_on_the_same_destinations(self):
        self.assert_symlink_parent_aliases_contend()

    def test_symlink_parent_aliases_contend_with_windows_parent_normalization(self):
        resolve = Path.resolve

        def windows_resolve(path, *args, **kwargs):
            # Win32 removes lexical dot components before following reparse points.
            return resolve(Path(os.path.normpath(path)), *args, **kwargs)

        with mock.patch.object(Path, 'resolve', windows_resolve):
            self.assert_symlink_parent_aliases_contend()

    def assert_symlink_parent_aliases_contend(self):
        command = self.script('')
        (self.tmp / 'aliases').mkdir()
        alias = self.tmp / 'aliases' / 'directory'
        # A direct directory alias has the same meaning on POSIX and Windows;
        # symlink/.. depends on the platform's parent-normalization semantics.
        alias.symlink_to(self.tmp, target_is_directory=True)
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        self.seed_old_evidence()
        with runner._output_locks(outputs):
            before = {p: p.read_bytes() for p in outputs}
            for index, shared in enumerate(outputs):
                with self.subTest(shared=shared.name):
                    inner = [self.tmp / ('inner-' + p.name) for p in outputs]
                    inner[index] = alias / shared.name
                    with mock.patch.object(runner.subprocess, 'run') as launch, \
                            self.assertRaises(FileExistsError):
                        runner.run_process(command, cwd=self.tmp, environment=os.environ.copy(),
                                           artifact=inner[0], retained=inner[1],
                                           receipt_path=inner[2], log=inner[3])
                    launch.assert_not_called()
                    self.assertEqual(before, {p: p.read_bytes() for p in outputs})
                    self.assertFalse(any(p.exists() for i, p in enumerate(inner) if i != index))
        self.assert_output_locks_released()

    def test_crashed_owner_leaves_reusable_lock_files(self):
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        child = ('import os, sys\n'
                 'from pathlib import Path\n'
                 'sys.path.insert(0, sys.argv[1])\n'
                 'import render_runner_process as runner\n'
                 'with runner._output_locks(map(Path, sys.argv[2:])):\n'
                 '    os._exit(23)\n')
        result = subprocess.run([sys.executable, '-I', '-B', '-c', child,
                                 str(Path(runner.__file__).parent), *map(str, outputs)],
                                cwd=self.tmp, check=False, capture_output=True)
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(len(list(self.tmp.glob('*.lock'))), 4)
        self.assertFalse(any(path.exists() for path in outputs))
        result, errors = self.run_case(self.script(
            'from pathlib import Path\nimport sys\n'
            'Path(sys.argv[1]).write_text("{}")\n'))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(errors, [])
        self.assertEqual(self.read_receipt()['outcome'], 'returned')
        self.assertEqual(self.read_receipt()['capture_sha256'],
                         runner.file_sha256(self.retained))
        self.assert_output_locks_released()

    def test_empty_abandoned_marker_files_are_reusable(self):
        for path in (self.artifact, self.retained, self.receipt, self.log):
            Path(str(path) + '.lock').touch()
        result, errors = self.run_case(self.script(''))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(errors, [])
        self.assertEqual(self.read_receipt()['outcome'], 'returned')
        self.assert_output_locks_released()

    def test_symlink_outputs_are_refused_before_mutation(self):
        command = self.script('')
        outputs = (self.artifact, self.retained, self.receipt, self.log)
        target = self.tmp / 'target.json'
        for exists in (True, False):
            for output in outputs:
                with self.subTest(output=output.name, target_exists=exists):
                    self.seed_old_evidence()
                    if exists:
                        target.write_bytes(b'{"target":true}')
                    else:
                        target.unlink(missing_ok=True)
                    output.unlink()
                    try:
                        output.symlink_to(target)
                    except (OSError, NotImplementedError) as error:
                        self.skipTest(f'symlink creation unavailable: {error}')
                    before = {p: p.read_bytes() for p in self.tmp.iterdir()
                              if not p.is_symlink()}
                    with mock.patch.object(runner.subprocess, 'run') as launch, \
                            self.assertRaisesRegex(OSError, 'output must not be a symlink'):
                        self.run_case(command)
                    launch.assert_not_called()
                    self.assertTrue(output.is_symlink())
                    self.assertEqual(runner.path_without_extended_prefix(output.readlink()), target)
                    self.assertEqual(before, {p: p.read_bytes() for p in self.tmp.iterdir()
                                              if not p.is_symlink()})
                    self.assertFalse(list(self.tmp.glob('*.lock')))
                    output.unlink()

    def test_capture_allocation_failure_publishes_current_failure_receipt(self):
        self.seed_old_evidence()
        command = self.script('from pathlib import Path\nimport sys\n'
                              'Path(sys.argv[1]).write_text("{}")\n')
        allocate = runner.tempfile.NamedTemporaryFile
        error = OSError('fixture capture allocation refused')

        def fail_capture(**kwargs):
            if kwargs['prefix'] == self.retained.name + '.':
                raise error
            return allocate(**kwargs)

        with mock.patch.object(runner.tempfile, 'NamedTemporaryFile', side_effect=fail_capture), \
                contextlib.redirect_stderr(io.StringIO()):
            result, errors = self.run_case(command)
        self.assertEqual(result.returncode, 0)
        receipt = self.read_receipt()
        self.assertEqual(receipt['outcome'], 'returned')
        self.assertEqual(receipt['capture_status'], 'write_error')
        self.assertIsNone(receipt['capture_path'])
        self.assertIsNone(receipt['capture_sha256'])
        self.assertEqual(errors, receipt['evidence_errors'])
        self.assertIn(str(error), errors[0])
        self.assertFalse(self.retained.exists())
        self.assertFalse(list(self.tmp.glob('*.tmp')))
        self.assert_output_locks_released()

    def test_receipt_allocation_failure_retires_stale_receipt(self):
        self.seed_old_evidence()
        allocate = runner.tempfile.NamedTemporaryFile

        def fail_receipt(**kwargs):
            if kwargs['prefix'] == self.receipt.name + '.':
                raise OSError('fixture receipt allocation refused')
            return allocate(**kwargs)

        with mock.patch.object(runner.tempfile, 'NamedTemporaryFile', side_effect=fail_receipt), \
                contextlib.redirect_stderr(io.StringIO()):
            result, errors = self.run_case(self.script(''))
        self.assertEqual(result.returncode, 0)
        self.assertIn('process receipt:', errors[0])
        self.assertFalse(self.receipt.exists())
        self.assertFalse(list(self.tmp.glob('*.tmp')))
        self.assert_output_locks_released()

    def test_capture_allocation_failure_preserves_exception_and_current_receipt(self):
        command = self.script('')
        allocate = runner.tempfile.NamedTemporaryFile

        def fail_capture(**kwargs):
            if kwargs['prefix'] == self.retained.name + '.':
                raise OSError('fixture capture allocation refused')
            return allocate(**kwargs)

        for error, outcome in ((OSError('fixture spawn refused'), 'spawn_error'),
                               (subprocess.TimeoutExpired(command, 60), 'timed_out')):
            with self.subTest(outcome=outcome):
                self.seed_old_evidence()

                def fail_process(argv, **kwargs):
                    self.artifact.write_bytes(b'{}')
                    raise error

                with mock.patch.object(runner.subprocess, 'run', side_effect=fail_process), \
                        mock.patch.object(runner.tempfile, 'NamedTemporaryFile',
                                          side_effect=fail_capture), \
                        contextlib.redirect_stderr(io.StringIO()), \
                        self.assertRaises(type(error)) as caught:
                    self.run_case(command)
                self.assertIs(caught.exception, error)
                receipt = self.read_receipt()
                self.assertEqual(receipt['outcome'], outcome)
                self.assertEqual(receipt['exception_type'], type(error).__name__)
                self.assertEqual(receipt['exception_message'], str(error))
                self.assertEqual(receipt['capture_status'], 'write_error')
                self.assertIsNone(receipt['capture_path'])
                self.assertIsNone(receipt['capture_sha256'])
                self.assertIn('capture retention:', receipt['evidence_errors'][0])
                self.assertFalse(self.retained.exists())
                self.assertFalse(list(self.tmp.glob('*.tmp')))
                self.assert_output_locks_released()

    def test_evidence_allocation_failure_preserves_process_exception(self):
        command = self.script('')
        for error in (OSError('fixture spawn refused'),
                      subprocess.TimeoutExpired(command, 60)):
            with self.subTest(error=type(error).__name__):
                self.seed_old_evidence()
                with mock.patch.object(runner.subprocess, 'run', side_effect=error), \
                        mock.patch.object(runner.tempfile, 'NamedTemporaryFile',
                                          side_effect=OSError('fixture allocation refused')), \
                        contextlib.redirect_stderr(io.StringIO()), \
                        self.assertRaises(type(error)) as caught:
                    self.run_case(command)
                self.assertIs(caught.exception, error)
                self.assertFalse(self.receipt.exists())
                self.assertFalse(self.retained.exists())
                self.assertFalse(list(self.tmp.glob('*.tmp')))
                self.assert_output_locks_released()

    def open_with_lock_close_error(self, attempted, error):
        open_file = runner._open_sidecar

        def open_output(path):
            handle = open_file(path)

            def close():
                handle.close()
                attempted.append(path)
                raise error

            wrapped = mock.Mock(wraps=handle)
            wrapped.close.side_effect = close
            return wrapped

        return open_output

    def test_lock_cleanup_failure_preserves_process_exception(self):
        command = self.script('')
        error = OSError('fixture spawn refused')
        attempted = []
        fail_lock_cleanup = self.open_with_lock_close_error(
            attempted, OSError('fixture lock cleanup refused'))

        with mock.patch.object(runner.subprocess, 'run', side_effect=error), \
                mock.patch.object(runner, '_open_sidecar', side_effect=fail_lock_cleanup), \
                self.assertRaises(OSError) as caught:
            self.run_case(command)
        self.assertIs(caught.exception, error)
        self.assertEqual(len(attempted), 4)
        self.assertEqual(self.read_receipt()['exception_message'], str(error))
        self.assert_output_locks_released()

    def test_held_lock_refuses_without_changing_old_evidence(self):
        command = self.script('')
        self.seed_old_evidence()
        self.assert_output_locks_released()
        Path(str(self.receipt) + '.lock').write_bytes(b'held sidecar')
        before = {p: p.read_bytes() for p in self.tmp.iterdir()}
        with self.output_lock_owners() as owners, runner._output_locks((self.receipt,)):
            with mock.patch.object(runner.subprocess, 'run') as launch, \
                    self.assertRaises(FileExistsError):
                self.run_case(command)
            launch.assert_not_called()
            self.assertEqual(before, {
                p: self.read_with_lock_owner(p, owners) for p in self.tmp.iterdir()})
        self.assert_output_locks_released()

    def test_lock_cleanup_failure_after_normal_exit_is_reported(self):
        command = self.script('')
        error = OSError('fixture lock cleanup refused')
        attempted = []
        fail_lock_cleanup = self.open_with_lock_close_error(attempted, error)

        with mock.patch.object(runner.subprocess, 'run',
                               return_value=subprocess.CompletedProcess(command, 0)), \
                mock.patch.object(runner, '_open_sidecar', side_effect=fail_lock_cleanup), \
                self.assertRaises(OSError) as caught:
            self.run_case(command)
        self.assertIs(caught.exception, error)
        self.assertEqual(len(attempted), 4)
        self.assertEqual(self.read_receipt()['outcome'], 'returned')
        self.assertEqual(self.read_receipt()['return_code'], 0)
        self.assert_output_locks_released()

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
