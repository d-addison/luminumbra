#!/usr/bin/env python3
"""Device-free process-custody, refusal and publication controls."""
import contextlib
import copy
import io
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import far_budget as budget
import render_runner_process as process
import run_far_budget as runner
import qualification_matrix as matrix_lib
from test_far_budget import synthetic_capture


def write_evidence(root, cell, data):
    selected = runner.paths(root, cell)
    selected['capture'].parent.mkdir(parents=True, exist_ok=True)
    payload = budget.canonical(data)
    selected['capture'].write_bytes(payload)
    selected['retained'].write_bytes(payload)
    identity = 'a' * 64
    receipt = dict(schema=process.RECEIPT_SCHEMA, outcome='returned', return_code=0,
        binary_identity_status='verified', binary_sha256=identity, evidence_errors=[],
        command=['synthetic-client', '--render-benchmark', str(selected['capture'])],
        capture_status='retained', capture_path=selected['retained'].relative_to(root).as_posix(),
        capture_sha256=budget.digest(payload),
        client=dict(kind='native', native_build=True, sha256_verified=True, sha256=identity,
                    build_manifest_client_sha256=identity, build_manifest_sha256='b' * 64),
        systems_config=dict(sha256='c' * 64, sha256_verified=True))
    selected['receipt'].write_bytes(budget.canonical(receipt))
    return selected, receipt


class FarBudgetRunnerTests(unittest.TestCase):
    def test_observation_admission_boundaries_and_overflow(self):
        frames = matrix_lib.benchmark_observation_frames
        for arguments, expected in (((18000, 8008), 36000), ((400, 18000), 28392),
                                    ((400, 1800, 0, 1), 15792),
                                    ((2200, 1800, 0, 1), 17592),
                                    ((400, 18000, 408, 2), 36000), ((18000, 5000), 32992), ((0, 1), 9993)):
            self.assertEqual(frames(*arguments), expected)
        for arguments in ((18000, 18000), (18000, 8009), (400, 18000, 409, 2),
                          (400, 18000, 0, 3), (18000, 5000, 0, 1), (1, 1, 2**64, 0), (1, 1, 0, 2**64),
                          (18001, 1), (1, 0), (1, 18001), (-1, 1), (1, 1, -1),
                          (1, 1, 0, -1), (True, 1), (1, 1.0), (1, '1'), (1, None)):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                frames(*arguments)

    def test_native_observation_budget_source_parity(self):
        root = budget.ROOT / 'src/luminumbra_client'
        workload = (root / 'app/RenderWorkload.h').read_text()
        presentation = (root / 'world/WorldPresentation.h').read_text()
        measurement = (root / 'app/RenderMeasurement.h').read_text()
        constants = ((workload, 'kMaxReloadSettlingPasses', matrix_lib.MAX_RELOAD_SETTLING_PASSES),
                     (workload, 'kMaxBenchmarkUploadWaitFrames', matrix_lib.MAX_BENCHMARK_UPLOAD_WAIT_FRAMES),
                     (presentation, 'kWorldPresentationFrameDeadline', matrix_lib.WORLD_PRESENTATION_FRAME_DEADLINE),
                     (measurement, 'kMaxFrames', matrix_lib.MAX_BENCHMARK_FRAMES))
        for source, name, value in constants:
            self.assertRegex(source, rf'\b{name} = {value};')
        self.assertIn('kMaxBudgetObservations = 2 * kMaxFrames;', workload)
        self.assertEqual(matrix_lib.MAX_BUDGET_OBSERVATIONS,
                         2 * matrix_lib.MAX_BENCHMARK_FRAMES)
        compact = re.sub(r'\s+', ' ', workload)
        for fragment in ('reload_frames = kWorldPresentationFrameDeadline + kMaxReloadSettlingPasses;',
                         'total = kWorldPresentationFrameDeadline + 2 * kMaxBenchmarkUploadWaitFrames;',
                         'for (const auto frames : {warmup, measured, runtime_boot})',
                         'frames > kMaxBudgetObservations - total', 'total += frames;',
                         'reloads > (kMaxBudgetObservations - total) / reload_frames',
                         'return total + reloads * reload_frames;'):
            self.assertIn(fragment, compact)
        recorder = (root / 'app/FarBudgetMeasurement.h').read_text()
        self.assertIn('kMaxObservations = kMaxBudgetObservations;', recorder)

    def test_far_runner_admission_precedes_directories_and_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'absent'
            matrix = budget.load_matrix()
            expanded = budget.cells(matrix)
            matrix.update(warmup_frames=18000, measured_frames=18000)
            cell = next(item for item in expanded if item['workload'] == 'settled')
            world = copy.deepcopy(matrix['worlds'][0])
            world['pose'] = dict(x=1.0, y=2.0, z=3.0, yaw=4.0, pitch=5.0)
            with self.assertRaisesRegex(ValueError, '36000'):
                runner.command(cell, world, matrix, 'client', 'capture', None)
            with self.assertRaisesRegex(ValueError, '36000'):
                runner.launch_cell(out, matrix, cell, world, client=Path('client'),
                                   runtime_root=Path('runtime'), build_manifest=None, timeout=1)
            self.assertFalse(out.exists())
            with mock.patch.object(budget, 'load_matrix', return_value=matrix), \
                    mock.patch.object(budget, 'cells', return_value=expanded), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.main(['--out-dir', str(out)]), 2)
            self.assertFalse(out.exists())
            matrix.update(measured_frames=8008)
            # Fixed view fits exactly, but v1 traversal still reserves 18000.
            self.assertEqual(runner.admit_cell(cell, matrix), 36000)
            self.assertIn('8008', runner.command(cell, world, matrix, 'client', 'capture', None))
            with mock.patch.object(budget, 'load_matrix', return_value=matrix), \
                    mock.patch.object(budget, 'cells', return_value=expanded), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.main(['--out-dir', str(out)]), 2)
            self.assertFalse(out.exists())

    def test_qualification_admission_precedes_moves_settings_and_directories(self):
        qualification = runner.qualification
        matrix = matrix_lib.load_matrix(budget.ROOT / 'tools/perf/qualification/matrix.json')
        matrix.update(warmup_frames=18000, fixed_view_measured_frames=18000)
        cell = next(cell for cell in matrix_lib.cells(matrix) if cell['kind'] == 'fixed_view')
        cell.update(pose=dict(x=1.0, y=2.0, z=3.0, yaw=4.0, pitch=5.0),
                    pose_source=None, pose_provenance='native-confirmed:fixture')
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'out'
            with self.assertRaisesRegex(ValueError, '36000'):
                matrix_lib.build_command(cell, client='client', artifact='capture', root=budget.ROOT)
            with self.assertRaisesRegex(ValueError, '36000'):
                qualification.run_cell(cell, matrix, client='client', out_dir=out, root=budget.ROOT,
                    python=sys.executable, checker='checker', timeout_seconds=1, environment={},
                    checker_canonical=False, matrix_canonical=False, build_manifest=None,
                    runtime_root=budget.ROOT)
            self.assertFalse(out.exists())
            admitted = dict(cell, measured_frames=8008)
            command = matrix_lib.build_command(admitted, client='client', artifact='capture',
                                               root=budget.ROOT)
            self.assertEqual(command[command.index('--render-benchmark-frames') + 1], '8008')
            out.mkdir()
            marker = out / qualification.QUALIFICATION_NAME
            marker.write_bytes(b'previous evidence')
            cells = out / 'cells'
            cells.mkdir()
            capture = cells / (cell['id'] + '.json')
            capture.write_bytes(b'previous capture')
            with self.assertRaisesRegex(ValueError, '36000'):
                qualification.run_cells(matrix, client=sys.executable, out_dir=out,
                                        root=budget.ROOT, runtime_root=budget.ROOT)
            self.assertEqual(marker.read_bytes(), b'previous evidence')
            self.assertEqual(capture.read_bytes(), b'previous capture')
            self.assertEqual(sorted(path.relative_to(out).as_posix() for path in out.rglob('*')),
                             ['cells', 'cells/' + capture.name, marker.name])
            with mock.patch.object(matrix_lib, 'load_matrix', return_value=matrix), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(qualification.main(['--client', sys.executable, '--out-dir', str(out), '--dry-run']), 2)
            self.assertEqual(marker.read_bytes(), b'previous evidence')
            self.assertEqual(capture.read_bytes(), b'previous capture')
            absent = Path(directory) / 'absent'
            with mock.patch.object(matrix_lib, 'load_matrix', return_value=matrix), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(qualification.main(['--client', sys.executable, '--out-dir', str(absent)]), 2)
            self.assertFalse(absent.exists())

    def test_owned_capture_complete_and_missing_receipt_incomplete(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, matrix, cell, world, script = synthetic_capture(root)
            selected, _ = write_evidence(root, cell, data)
            result = runner.inspect_cell(root, matrix, cell, world, [], script)
            self.assertEqual(result['state'], 'complete', result)
            self.assertEqual(result['capture_sha256'], budget.digest(selected['capture'].read_bytes()))
            self.assertEqual(set(result['sources']), {'capture', 'retained', 'receipt'})
            selected['receipt'].unlink()
            result = runner.inspect_cell(root, matrix, cell, world, [], script)
            self.assertEqual(result['state'], 'incomplete')
            self.assertIn('missing receipt', result['reasons'])

    def test_custody_refuses_every_missing_or_mismatched_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, matrix, cell, world, script = synthetic_capture(root)
            selected, receipt = write_evidence(root, cell, data)
            mutations = [lambda r: r.update(outcome='timed_out'), lambda r: r.update(return_code=1),
                lambda r: r.update(evidence_errors=['failed']), lambda r: r.update(schema='unknown'),
                lambda r: r.update(capture_status='missing'), lambda r: r.update(command=[]),
                lambda r: r.update(capture_path='other'), lambda r: r.update(capture_sha256='0' * 64),
                lambda r: r.update(binary_identity_status='changed'), lambda r: r.pop('client'),
                lambda r: r['client'].update(kind='script'), lambda r: r['client'].update(native_build=False),
                lambda r: r['client'].update(sha256_verified=False),
                lambda r: r['client'].update(build_manifest_client_sha256='0' * 64),
                lambda r: r['client'].update(build_manifest_sha256='bad'),
                lambda r: r.update(binary_sha256='0' * 64), lambda r: r.pop('systems_config'),
                lambda r: r['systems_config'].update(sha256_verified=False),
                lambda r: r['systems_config'].update(sha256='bad')]
            for mutate in mutations:
                changed = copy.deepcopy(receipt)
                mutate(changed)
                selected['receipt'].write_bytes(budget.canonical(changed))
                result = runner.inspect_cell(root, matrix, cell, world, [], script)
                with self.subTest(mutation=mutations.index(mutate)):
                    self.assertEqual(result['state'], 'invalid', result)
            selected['receipt'].write_bytes(budget.canonical(receipt))
            selected['retained'].write_bytes(b'{}')
            self.assertEqual(runner.inspect_cell(root, matrix, cell, world, [], script)['state'], 'invalid')

    def test_partial_wrong_type_empty_and_unreadable_evidence_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, matrix, cell, world, script = synthetic_capture(root)
            selected, _ = write_evidence(root, cell, data)
            for value in (b'{', b'[]', b'', b'{"x":NaN}', b'[' * 65 + b']' * 65):
                selected['receipt'].write_bytes(value)
                self.assertEqual(runner.inspect_cell(root, matrix, cell, world, [], script)['state'], 'invalid')
            with mock.patch.object(budget, 'read', side_effect=PermissionError('denied')):
                result = runner.inspect_cell(root, matrix, cell, world, [], script)
            self.assertEqual(result['state'], 'invalid')
            selected['capture'].unlink()
            selected['capture'].mkdir()
            self.assertEqual(runner.inspect_cell(root, matrix, cell, world, [], script)['state'], 'invalid')

    def test_dependency_stays_incomplete_with_valid_owned_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, matrix, cell, world, script = synthetic_capture(root)
            write_evidence(root, cell, data)
            result = runner.inspect_cell(root, matrix, cell, world, ['missing commit/bake'], script)
            self.assertEqual(result['state'], 'incomplete')
            self.assertIsNotNone(result['capture_sha256'])
            self.assertNotIn('metrics', result)

    def test_index_requires_complete_ordered_matrix_and_has_no_invented_caps(self):
        matrix = budget.load_matrix()
        results = [dict(cell, state='complete') for cell in budget.cells(matrix)]
        self.assertEqual(runner.build_index(matrix, results)['state'], 'complete')
        for bad in ([], results[:-1], results + results[:1], list(reversed(results))):
            with self.assertRaises(ValueError):
                runner.build_index(matrix, bad)
        for state in ('incomplete', 'invalid', 'fail'):
            broken = copy.deepcopy(results)
            broken[0]['state'] = state
            index = runner.build_index(matrix, broken)
            self.assertEqual(index['state'], 'incomplete')
            self.assertIsNone(index['candidate_caps']['separately_measured_reserve_bytes'])
            self.assertTrue(all(tier['resident_bytes'] is None for tier in index['candidate_caps']['tiers']))

    def test_isolated_cli_reports_all_missing_cells_outside_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'report'
            result = subprocess.run([sys.executable, '-I', '-B', str(Path(runner.__file__)),
                                     '--out-dir', str(out)], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 3, result.stderr)
            index = json.loads((out / 'index.json').read_bytes())
            self.assertEqual(index['group_count'], 16)
            self.assertEqual(len(index['cells']), 32)
            self.assertTrue(all(cell['state'] == 'incomplete' for cell in index['cells']))
            self.assertIn('unavailable', (out / 'review.md').read_text())
            self.assertEqual(index['evidence_root'], str(out.resolve()))
            self.assertEqual(index['matrix_sha256'], budget.digest(budget.canonical(index['matrix'])))

    def test_invalid_inputs_leave_existing_output_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out = root / 'report'
            out.mkdir()
            marker = out / 'marker'
            marker.write_bytes(b'keep')
            cases = [[], ['--timeout-seconds', 'nan'], ['--timeout-seconds', '-1'],
                     ['--client', str(root / 'client')], ['--matrix', str(root / 'absent')],
                     ['--evidence-root', str(out)]]
            for arguments in cases:
                with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(runner.main(['--out-dir', str(out), *arguments]), 2)
                self.assertEqual(list(out.iterdir()), [marker])
                self.assertEqual(marker.read_bytes(), b'keep')
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.main(['--out-dir', str(budget.ROOT / 'forbidden-output')]), 2)

    def test_index_is_last_publication_and_write_failure_never_publishes_it(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'report'
            calls = []
            atomic_write = process.atomic_write
            def fail_review(path, payload):
                if Path(path).name != 'review.md':
                    return atomic_write(path, payload)
                calls.append(Path(path).name)
                raise OSError('full')
            with mock.patch.object(process, 'atomic_write', side_effect=fail_review), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.main(['--out-dir', str(out)]), 2)
            self.assertEqual(calls, ['review.md'])
            self.assertFalse((out / 'index.json').exists())

    def test_command_contains_every_contracted_capture_flag(self):
        matrix = budget.load_matrix()
        for cell in budget.cells(matrix):
            world = next(world for world in matrix['worlds'] if world['id'] == cell['world'])
            local = copy.deepcopy(world)
            local['pose'] = dict(x=1.0, y=2.0, z=3.0, yaw=4.0, pitch=5.0)
            argv = runner.command(cell, local, matrix, 'client', 'capture', 'script')
            for flag, expected in (('--capture-size', cell['display']), ('--perf-profile', cell['profile']),
                                    ('--render-benchmark-schema', 'v3'), ('--render-benchmark-warmup', '400'),
                                    ('--world-preset', world['preset'])):
                self.assertEqual(argv[argv.index(flag) + 1], expected)
            if cell['workload'] == 'traversal':
                self.assertEqual(argv[argv.index('--traversal') + 1], 'script')
            else:
                self.assertEqual(argv[argv.index('--cam-pos') + 1], '1.0,2.0,3.0')
                self.assertEqual(argv[argv.index('--render-benchmark-frames') + 1], '240')

    def test_inspection_and_launch_share_crlf_normalized_script_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source'
            source.mkdir()
            work = root / 'work'
            work.mkdir()
            text = 'luminumbra.traversal.v1\nfixture\n'
            (source / 'script').write_bytes(text.replace('\n', '\r\n').encode('utf-8'))
            world = dict(script='script', script_sha256=budget.digest(text.encode('utf-8')))
            script = runner.snapshot_script(work, world, source)
            self.assertEqual(script.read_bytes(), text.encode('utf-8'))
            before = script.read_bytes()
            (source / 'script').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'traversal changed'):
                runner.snapshot_script(work, world, source)
            self.assertEqual(script.read_bytes(), before)

    def test_fake_process_reuses_real_custody_but_cannot_be_native_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / 'runtime'
            (runtime / 'data/common').mkdir(parents=True)
            (runtime / 'data/common/systems.json').write_text('{}')
            client = root / 'client.py'
            client.write_text('import pathlib,sys\np=pathlib.Path(sys.argv[sys.argv.index("--render-benchmark")+1])\np.write_text("{}")\n')
            out = root / 'out'
            (out / 'cells').mkdir(parents=True)
            matrix = budget.load_matrix()
            cell = budget.cells(matrix)[1]
            errors = runner.launch_cell(out, matrix, cell, matrix['worlds'][0], client=client,
                runtime_root=runtime, build_manifest=None, timeout=2)
            self.assertEqual(errors, [])
            receipt = budget.decode(runner.paths(out, cell)['receipt'].read_bytes())
            self.assertEqual(receipt['capture_status'], 'retained')
            self.assertEqual(receipt['client']['kind'], 'script')
            result = runner.inspect_cell(out, matrix, cell, matrix['worlds'][0], [])
            self.assertEqual(result['state'], 'invalid')
            self.assertIn('native build', result['reasons'][0])

    def test_launch_deadline_spawn_and_nonzero_exit_are_incomplete(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / 'runtime'
            matrix = budget.load_matrix()
            cell = budget.cells(matrix)[1]
            for position, failure in enumerate((subprocess.TimeoutExpired('client', 1), OSError('denied'))):
                out = root / str(position)
                (out / 'cells').mkdir(parents=True)
                with mock.patch.object(runner.qualification, 'run_client_process', side_effect=failure):
                    errors = runner.launch_cell(out, matrix, cell, matrix['worlds'][0],
                        client=root / 'client', runtime_root=runtime, build_manifest=None, timeout=1)
                self.assertTrue(errors)
                result = runner.inspect_cell(out, matrix, cell, matrix['worlds'][0], errors)
                self.assertEqual(result['state'], 'incomplete')
            out = root / 'exit'
            (out / 'cells').mkdir(parents=True)
            with mock.patch.object(runner.qualification, 'run_client_process', return_value=(subprocess.CompletedProcess([], 3), [], {})):
                errors = runner.launch_cell(out, matrix, cell, matrix['worlds'][0],
                    client=root / 'client', runtime_root=runtime, build_manifest=None, timeout=1)
            self.assertEqual(errors, ['process exit 3'])


if __name__ == '__main__':
    unittest.main()
