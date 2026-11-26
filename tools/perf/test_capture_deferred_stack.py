#!/usr/bin/env python3
"""Fast fixed-budget evidence contracts; all process execution is mocked."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_deferred_stack as capture
from test_sim_budget_gate import write_capture


class DeferredStackEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / 'source'
        for name in ('data/common', 'res', 'config', 'worlds/atlas/presets', 'tools/perf'):
            (self.source / name).mkdir(parents=True)
        for preset in capture.populated.PRESETS:
            (self.source / f'worlds/atlas/presets/{preset}.json').write_text('{}', encoding='utf-8')
        (self.source / 'tools/perf/test_render_runner.py').write_text('fixture', encoding='utf-8')
        self.assets = self.root / 'assets'
        self.assets.mkdir()
        self.binaries = {}
        for role in ('client', 'parity', 'server'):
            binary = self.root / role
            binary.write_bytes(role.encode('ascii'))
            self.binaries[role] = binary
        self.build = self.root / 'build.json'
        self.build.write_bytes(capture.encode(dict(
            source_sha='a' * 40, source_root=str(self.source),
            binaries={role: capture.runner.file_sha256(path) for role, path in self.binaries.items()},
            build_flags=dict(dict.fromkeys(capture.perf.BUILD_CONFIGURATION_FIELDS, 'OFF'),
                             asan='ON', build_type='Debug', effective_flags_sha256='c' * 64))))
        self.evidence = self.root / 'evidence'
        self.deadlines = []
        self.failure = None
        self.phase = 'Shutdown stage: world_save_started\r\n'
        self.source_patch = mock.patch.object(capture, 'source_sha', return_value='a' * 40)
        self.source_patch.start()
        self.addCleanup(self.source_patch.stop)
        location = mock.patch.object(capture, '__file__', str(self.source / 'tools/perf/capture_deferred_stack.py'))
        location.start()
        self.addCleanup(location.stop)

    def execute(self, command, **kwargs):
        self.deadlines.append(kwargs['timeout'])
        kwargs['stdout'].write(self.phase.encode('utf-8'))
        kwargs['stdout'].flush()
        if self.failure == 'timeout':
            raise subprocess.TimeoutExpired(command, kwargs['timeout'])
        if self.failure == 'spawn':
            raise OSError('synthetic spawn refusal')
        if '--gtest_filter=' in command[1]:
            name = command[1].split('=', 1)[1]
            suite, method = name.split('.', 1)
            data = dict(tests=1, failures=0, disabled=0, testsuites=[dict(
                name=suite, testsuite=[dict(name=method, status='RUN', result='COMPLETED')])])
            artifact = Path(command[2].split('json:', 1)[1])
        else:
            artifact = Path(command[1])
            data = {'schema': 'synthetic'}
        expected = {label: code for processes in capture.RENDER_PROCESSES.values()
                    for label, code in processes.items()}
        exit_code = expected.get(artifact.stem, 0)
        if not exit_code:
            artifact.write_bytes(capture.encode(data))
        return subprocess.CompletedProcess(command, exit_code)

    def collect(self):
        def render_process(test):
            folder = capture.render_tests.OPTIONS.artifacts
            case = next(name for name, method in capture.RENDER_CASES.items() if method == test._testMethodName)
            for label, exit_code in capture.RENDER_PROCESSES[case].items():
                capture.render_tests._run_process(
                    [str(self.binaries['client']), str(folder / (label + '.json'))],
                    cwd=self.source, environment={}, log=folder / ('render-' + label + '.log'),
                    artifact=folder / (label + '.json'), retained=folder / ('render-' + label + '.json'),
                    receipt_path=folder / ('render-' + label + '.process.json'), expected_exit=exit_code)

        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(capture.runner.crash_capture, 'run', side_effect=self.execute))
            stack.enter_context(mock.patch.object(capture.render_tests.RenderRunnerTests, 'setUp', lambda test: None))
            for method in capture.RENDER_CASES.values():
                stack.enter_context(mock.patch.object(capture.render_tests.RenderRunnerTests, method, render_process))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            return capture.capture(self.source, self.assets, self.binaries, self.build, self.evidence)

    def test_process_map_matches_the_render_regressions(self):
        # The mocked runs replay RENDER_PROCESSES, so check the map against the
        # real regressions' process labels and expected exits.
        import inspect
        import re
        for case, method in capture.RENDER_CASES.items():
            with self.subTest(case=case):
                source = inspect.getsource(getattr(capture.render_tests.RenderRunnerTests, method))
                labels = {label: 0 for label in re.findall(r"run_client\('([a-z-]+)',", source)}
                looped = re.findall(r"(?<!run_client)\('([a-z-]+)',\s*\[", source)
                if looped:
                    self.assertIn('expected_exit=2', source)
                labels.update({label: 2 for label in looped})
                self.assertEqual(capture.RENDER_PROCESSES[case], labels)

    def test_fixed_deadlines_and_failure_receipts(self):
        for failure in (None, 'timeout', 'spawn'):
            with self.subTest(failure=failure):
                self.evidence = self.root / ('evidence-' + str(failure))
                self.failure = failure
                self.deadlines.clear()
                manifest = self.collect()
                self.assertEqual(self.deadlines, [180] * (11 if failure is None else 4) + [1800] * 2)
                self.assertEqual(capture.render_tests.PROCESS_TIMEOUT_SECONDS, 180)
                self.assertEqual(len(manifest['runs']), 6)
                report = capture.report(self.evidence)
                self.assertEqual(report['passed'], failure is None)
                for row in report['runs']:
                    self.assertGreaterEqual(len(row['processes']), 1)
                    process = row['processes'][0]
                    expected = {None: 'returned', 'timeout': 'timed_out', 'spawn': 'spawn_error'}[failure]
                    self.assertEqual(process['classification'], expected)
                    self.assertEqual(process['phase'], 'save')
                    self.assertIsNone(process['measurement_ms'])
                    self.assertEqual(process['capture_status'] if failure is None else
                                     capture.read_json(Path(process['receipt']['path']))['capture_status'],
                                     'retained' if failure is None and process['return_code'] == 0 else 'missing')
                with self.assertRaises(FileExistsError):
                    self.collect()

    def test_rejects_mixed_binary_or_source_identity(self):
        self.collect()
        path = self.evidence / '0/render-first.process.json'
        original = capture.read_json(path)
        mutations = {'binary_sha256': 'f' * 64, 'source_sha': 'b' * 40,
                     'capture_identity_sha256': 'e' * 64, 'timeout_seconds': 540,
                     'binary_identity_status': 'pending', 'command': ['different']}
        for field, value in mutations.items():
            with self.subTest(field=field):
                path.write_bytes(capture.encode(dict(original, **{field: value})))
                with self.assertRaisesRegex(ValueError, 'identity|deadline'):
                    capture.report(self.evidence)
        path.write_bytes(capture.encode(original))
        for field in ('source_sha', 'source_root', 'binaries', 'build_flags'):
            record = capture.read_json(self.build)
            changed = copy.deepcopy(record)
            changed[field] = str(self.root) if field == 'source_root' else {'wrong': 'identity'}
            self.build.write_bytes(capture.encode(changed))
            with self.subTest(build_field=field), self.assertRaises(ValueError):
                capture.identity(self.source, self.assets, self.binaries, self.build)
            self.build.write_bytes(capture.encode(record))
        record = capture.read_json(self.build)
        for field in capture.perf.BUILD_CONFIGURATION_FIELDS:
            changed = copy.deepcopy(record)
            del changed['build_flags'][field]
            self.build.write_bytes(capture.encode(changed))
            with self.subTest(missing_flag=field), self.assertRaises(ValueError):
                capture.identity(self.source, self.assets, self.binaries, self.build)
        self.build.write_bytes(capture.encode(record))

    def profile(self):
        report = capture.report(self.evidence)
        artifact = self.root / 'stacks.txt'
        artifact.write_text('worker stack\n', encoding='utf-8')
        path = self.root / 'profile.json'
        profile = dict(schema='luminumbra.deferred_stack_profile.v1', identity=report['identity'], runs=[dict(
            case=report['runs'][0]['case'],
            commands_sha256=capture.fingerprint([p['command'] for p in report['runs'][0]['processes']]),
            artifacts=['stacks.txt'], samples=[dict(stack=['worker', capture.TARGETS[0], capture.TARGETS[1]], samples=7),
                                             dict(stack=[capture.TARGETS[2]], samples=3)])])
        path.write_bytes(capture.encode(profile))
        return path, profile

    def test_synthetic_profile_report_is_deterministic(self):
        self.collect()
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(capture.main(['report', '--evidence-dir', str(self.evidence)]), 0)
        self.assertTrue(json.loads(output.getvalue())['passed'])
        path, profile = self.profile()
        first = capture.encode(capture.report(self.evidence, path))
        profile['runs'][0]['samples'].reverse()
        path.write_bytes(capture.encode(profile))
        self.assertEqual(first, capture.encode(capture.report(self.evidence, path)))
        self.assertEqual(first, capture.encode(capture.report(
            Path(os.path.relpath(self.evidence)), Path(os.path.relpath(path)))))
        result = json.loads(first)
        self.assertEqual(result['runs'][0]['profile']['inclusive_samples'][capture.TARGETS[0]], 7)
        self.assertEqual(result['runs'][0]['profile']['inclusive_samples'][capture.TARGETS[3]], 0)
        self.assertIsNone(result['runs'][1]['profile'])
        self.assertEqual(result['native_qualification'], 'pending')
        profile['runs'][0]['samples'] = []
        path.write_bytes(capture.encode(profile))
        self.assertTrue(all(value is None for value in capture.report(self.evidence, path)[
            'runs'][0]['profile']['inclusive_samples'].values()))

    def test_profile_refusals_and_missing_inputs(self):
        self.collect()
        path, profile = self.profile()
        for field, value in (('commands_sha256', '0' * 64), ('artifacts', []),
                             ('artifacts', ['stacks.txt', 'stacks.txt']), ('artifacts', [None]),
                             ('artifacts', ['stacks.txt'] * (capture.MAX_ARTIFACTS + 1)),
                             ('artifacts', ['../escape']), ('artifacts', ['C:\\escape']),
                             ('artifacts', ['missing']), ('samples', [{'stack': [], 'samples': 1}]),
                             ('samples', [{'stack': ['worker'] * (capture.MAX_FRAMES + 1), 'samples': 1}]),
                             ('samples', [{'stack': ['worker'], 'samples': True}]),
                             ('samples', [{'stack': ['worker'], 'samples': 0}]),
                             ('samples', [{'stack': ['worker'], 'samples': 1e38}]),
                             ('samples', [{}]), ('samples', [{}] * (capture.MAX_STACKS + 1))):
            changed = copy.deepcopy(profile)
            changed['runs'][0][field] = value
            path.write_bytes(capture.encode(changed))
            with self.subTest(field=field, value=str(value)[:60]), self.assertRaises(
                    (ValueError, OSError, capture.receipts.InputError)):
                capture.report(self.evidence, path)
        changed = copy.deepcopy(profile)
        changed['identity']['source_sha'] = 'b' * 40
        path.write_bytes(capture.encode(changed))
        with self.assertRaisesRegex(ValueError, 'identity'):
            capture.report(self.evidence, path)
        changed = dict(profile, schema='luminumbra.deferred_stack_profile.future')
        path.write_bytes(capture.encode(changed))
        with self.assertRaises(ValueError):
            capture.report(self.evidence, path)
        path.write_text('{', encoding='utf-8')
        with self.assertRaises(ValueError):
            capture.report(self.evidence, path)
        path.write_text('{"schema":1,"schema":2}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            capture.read_json(path)

    def test_phase_and_missing_log_classification(self):
        for index, (phase, expected) in enumerate((
                ('Shutdown stage: world_readers_drain_started\n', 'streaming_wait'),
                ('EnsureSurfaceReadyNear/generation-drain\n', 'boot'), ('unrelated\n', 'unknown'))):
            self.evidence = self.root / f'phase-{index}'
            self.phase, self.failure = phase, 'timeout'
            self.collect()
            result = capture.report(self.evidence)
            self.assertEqual(result['runs'][0]['processes'][0]['phase'], expected)
        (self.evidence / '0/render-first.log').unlink()
        result = capture.report(self.evidence)
        self.assertTrue(result['runs'][0]['processes'][0]['log_missing'])
        (self.evidence / '0/render-first.log').write_bytes(b'\xff')
        with self.assertRaises(capture.receipts.InputError):
            capture.report(self.evidence)

    def test_empty_wrong_parity_and_incomplete_capture_refuse(self):
        self.collect()
        path = self.evidence / '4/parity.json'
        data = capture.read_json(path)
        self.assertTrue(capture.parity_completed(data, capture.CASES[4]))
        for field, value in (('tests', 0), ('testsuites', []), ('disabled', 1)):
            self.assertFalse(capture.parity_completed(dict(data, **{field: value}), capture.CASES[4]))
        data['testsuites'][0]['testsuite'][0]['name'] = 'different'
        self.assertFalse(capture.parity_completed(data, capture.CASES[4]))
        path.write_text('{}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'digest'):
            capture.report(self.evidence)
        manifest_path = self.evidence / 'capture.json'
        manifest = capture.read_json(manifest_path)
        for field, value in (('schema', 'luminumbra.deferred_stack_capture.future'),
                             ('complete', False), ('runs', [])):
            manifest_path.write_bytes(capture.encode(dict(manifest, **{field: value})))
            with self.assertRaises(ValueError):
                capture.report(self.evidence)
        manifest_path.write_bytes(capture.encode(manifest))

    def test_populated_path_preserves_measurements_and_identity(self):
        self.collect()
        bound = capture.report(self.evidence)['identity']
        directory = write_capture(self.root / 'populated', overrides={
            'binary_sha256': bound['binaries']['server'], 'fixture_hash': bound['fixtures']['populated']})
        path = directory / 'capture.json'
        result = capture.report(self.evidence, populated_capture=path)
        self.assertEqual(len(result['populated']), 3)
        self.assertIn('sim_budget', result['populated'][0])
        manifest = capture.read_json(path)
        for field, value in (('schema', 'luminumbra.sim_budget_capture.future'),
                             ('revision', 'b' * 40), ('binary_sha256', '0' * 64),
                             ('fixture_hash', '0' * 64), ('source_dirty', True), ('runs', [])):
            path.write_bytes(capture.encode(dict(manifest, **{field: value})))
            with self.subTest(field=field), self.assertRaises(ValueError):
                capture.report(self.evidence, populated_capture=path)

    def test_identity_change_retains_incomplete_bundle_and_cli_refuses(self):
        with mock.patch.object(capture, 'identity', side_effect=[{'source_sha': 'a' * 40}, {'source_sha': 'b' * 40}]), \
                mock.patch.object(capture, 'render_case', return_value=dict(passed=False)), \
                mock.patch.object(capture, 'parity_case', return_value=dict(passed=False)):
            with self.assertRaisesRegex(ValueError, 'changed during capture'):
                capture.capture(self.source, self.assets, self.binaries, self.build, self.evidence)
        self.assertFalse(capture.read_json(self.evidence / 'capture.json')['complete'])
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(capture.main(['report', '--evidence-dir', str(self.evidence)]), 2)

    def test_source_requires_clean_sha_and_safe_artifacts(self):
        self.source_patch.stop()
        for outputs in (['not-a-sha', ''], ['a' * 40, ' M tracked']):
            with mock.patch.object(capture.subprocess, 'check_output', side_effect=outputs), self.assertRaises(ValueError):
                capture.source_sha(self.source)
        path = self.root / 'partial.json'
        path.write_text('{', encoding='utf-8')
        with self.assertRaises(ValueError):
            capture.read_json(path)
        directory = self.root / 'directory'
        directory.mkdir()
        with self.assertRaises(capture.receipts.InputError):
            capture.read_json(directory)

    def test_missing_receipts_deadline_expiry_and_failure_verdicts(self):
        self.collect()
        path = self.evidence / '0/render-first.process.json'
        original = capture.read_json(path)
        for field, value in (('elapsed_seconds', 181), ('return_code', 9),
                             ('evidence_errors', ['partial capture']), ('capture_status', 'missing')):
            path.write_bytes(capture.encode(dict(original, **{field: value})))
            with self.subTest(field=field):
                self.assertFalse(capture.report(self.evidence)['passed'])
        path.write_bytes(capture.encode(original))
        manifest_path = self.evidence / 'capture.json'
        manifest = capture.read_json(manifest_path)
        changed = copy.deepcopy(manifest)
        changed['runs'][0]['receipts'].pop()
        manifest_path.write_bytes(capture.encode(changed))
        self.assertFalse(capture.report(self.evidence)['passed'])
        changed['runs'][0]['receipts'] = []
        manifest_path.write_bytes(capture.encode(changed))
        self.assertFalse(capture.report(self.evidence)['passed'])
        manifest_path.write_bytes(capture.encode(manifest))
        original['binary_sha256'] = None
        original.pop('binary_identity_status')
        original.update(outcome='spawn_error', return_code=None, elapsed_seconds=None,
                        exception_type='OSError', exception_message='unreadable binary')
        path.write_bytes(capture.encode(original))
        result = capture.report(self.evidence)
        self.assertFalse(result['passed'])
        first = next(item for item in result['runs'][0]['processes'] if item['label'] == 'first')
        self.assertFalse(first['binary_verified'])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(capture.main(['report', '--evidence-dir', str(self.evidence)]), 1)

    def test_source_output_and_byte_bounds_refuse_before_execution(self):
        with self.assertRaisesRegex(ValueError, 'outside'):
            capture.capture(self.source, self.assets, self.binaries, self.build, self.source / 'evidence')
        self.assertFalse((self.source / 'evidence').exists())
        with self.assertRaisesRegex(ValueError, 'containing'):
            capture.identity(self.root, self.assets, self.binaries, self.build)
        with mock.patch.object(capture.receipts, 'MAX_FILE_BYTES', 2):
            with self.assertRaises(capture.receipts.InputError):
                capture.file_record(self.build)
        # No symlink privileges or platform-specific device files are needed.
        with mock.patch.object(capture.receipts, 'open_regular_file', return_value=contextlib.nullcontext(
                io.BytesIO(b'large'))), mock.patch.object(capture.receipts, 'MAX_FILE_BYTES', 2):
            with self.assertRaisesRegex(ValueError, 'byte cap'):
                capture.file_record(self.build)


if __name__ == '__main__':
    unittest.main()
