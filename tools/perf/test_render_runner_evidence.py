#!/usr/bin/env python3
"""Device-free run_client controls, including a real child that outlives capture."""
import argparse
import base64
from contextlib import redirect_stderr
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

import test_render_contract as contract_fixtures


parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--runner', type=Path, default=Path(__file__).with_name('test_render_runner.py'))
options, remaining = parser.parse_known_args()
spec = importlib.util.spec_from_file_location('runner_under_test', options.runner)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

CHILD = r'''
import base64, pathlib, sys, time
artifact, payload, code, wait, loaders, ready, startup_delay = sys.argv[1:]
time.sleep(float(startup_delay))
if loaders == '1':
    print('Static-model textures registered (3 models).', flush=True)
    for part in ('trunk', 'branches', 'leaves'):
        print(f"tree_small_02_{part}.lmesh' (3 verts, 3 indices)", flush=True)
if payload != '-':
    pathlib.Path(artifact).write_bytes(base64.b64decode(payload))
pathlib.Path(ready).touch()
if wait == '1':
    time.sleep(30)
sys.exit(int(code))
'''


SLOW_CHILD = r'''
import pathlib, sys, time
print('A' * 20000, flush=True)
print('EnsureSurfaceReadyNear: draining generation jobs (near 0,0)...', flush=True)
print('EnsureSurfaceReadyNear/surface-build batch 3', flush=True)
print('unrelated trailing line', flush=True)
pathlib.Path(sys.argv[1]).touch()
time.sleep(30)
'''


class RenderRunnerRefusalInputsTests(unittest.TestCase):
    def check_texture_inputs(self, method):
        # Validate the actual integration case's argv and script before substituting
        # the native content loader's refusal. An option error must fail this check.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / 'data/models/trees/tree_textures.json'
            manifest.parent.mkdir(parents=True)
            texture = root / 'data/texture.ltex'
            texture.write_bytes(b'texture bytes')
            manifest.write_text(json.dumps({'models': [{'albedo': 'data/texture.ltex'}]}),
                                encoding='utf-8')
            case = runner.RenderRunnerTests(method)
            case.root = root
            def content_refusal(name, extra, *, expected_exit, schema):
                self.assertEqual((name, expected_exit, schema), ('refused', 2, 'v4'))
                self.assertEqual(extra[0], '--traversal')
                self.assertEqual(len(extra), 2, 'content refusal must not have a duration override')
                script = runner.contract.traversal_manifest(Path(extra[1]).read_text(encoding='utf-8'))
                self.assertEqual(script['kind'], 'save_load')
                self.assertEqual(script['expected_ticks'], 4)
                self.assertEqual([action['tick'] for action in script['actions']], [1, 2])
                document = json.loads(manifest.read_bytes())
                reference = document['models'][0]['albedo']
                self.assertEqual((root / reference).resolve(), (root / 'external.ltex').resolve())
                self.assertEqual((root / 'external.ltex').read_bytes(), texture.read_bytes())
                return 'Benchmark content reference escapes inventoried trees'
            with mock.patch.object(runner, 'OPTIONS', types.SimpleNamespace(
                    source=Path(__file__).resolve().parents[2]), create=True), \
                    mock.patch.object(case, 'run_client', side_effect=content_refusal) as process:
                getattr(case, method)()
                process.assert_called_once()

    def test_absolute_texture_case_reaches_content_gate(self):
        self.check_texture_inputs('test_absolute_texture_refusal')

    def test_parent_relative_texture_case_reaches_content_gate(self):
        self.check_texture_inputs('test_parent_relative_texture_refusal')


class RenderRunnerEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.runtime = self.root / 'runtime'
        self.artifacts = self.root / 'artifacts'
        self.runtime.mkdir()
        self.artifacts.mkdir()
        self.child = self.root / 'child.py'
        self.child.write_text(CHILD, encoding="utf-8")
        self.case = runner.RenderRunnerTests('test_traversal_twice')
        self.case.root = self.runtime
        self.case.environment = os.environ.copy()
        self.client = Path(sys.executable)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(runner, 'OPTIONS', types.SimpleNamespace(
            client=self.client, artifacts=self.artifacts), create=True).start()
        self.payload = b'{\n  "schema": "diagnostic_fixture", "frames": 4\n}\n'
        self.code = 0
        self.wait = False
        self.startup_delay = 0
        self.loaders = True
        self.process_exception = None
        self.original_run = subprocess.run
        self.calls = []
        mock.patch.object(runner.subprocess, 'run', side_effect=self.run_child).start()

    def run_child(self, command, **kwargs):
        self.calls.append((command, kwargs.copy()))
        self.assertEqual(kwargs['timeout'], 180)
        self.assertFalse(kwargs['check'])
        artifact = command[command.index('--render-benchmark') + 1]
        payload = '-' if self.payload is None else base64.b64encode(self.payload).decode('ascii')
        child_command = [sys.executable, str(self.child), artifact, payload,
                         str(self.code), str(int(self.wait)), str(int(self.loaders)),
                         str(self.root / 'child.ready'), str(self.startup_delay)]
        # Production argv and its 180-second limit are observed unchanged.
        # Time only the fixture's intentional wait after its writes are complete.
        try:
            if self.wait:
                return self.run_ready_child(child_command, self.root / 'child.ready', **kwargs)
            return self.original_run(child_command, **dict(kwargs, timeout=5))
        except subprocess.TimeoutExpired as error:
            self.process_exception = error
            self.process_exception_attributes = (error.cmd, error.timeout)
            raise

    def run_ready_child(self, command, ready, **kwargs):
        ready.unlink(missing_ok=True)
        kwargs.pop('timeout')
        kwargs.pop('check')
        with subprocess.Popen(command, **kwargs) as process:
            try:
                deadline = time.monotonic() + 5
                while not ready.exists():
                    self.assertIsNone(process.poll(), 'fixture exited before becoming ready')
                    self.assertLess(time.monotonic(), deadline, 'fixture did not become ready')
                    time.sleep(0.01)
                code = process.wait(timeout=0.05)
                return subprocess.CompletedProcess(command, code)
            finally:
                # Reap the child before reading evidence or deleting its runtime.
                if process.poll() is None:
                    process.kill()
                process.wait()

    def paths(self, name='capture'):
        stem = 'test_traversal_twice-' + name
        return self.artifacts / (stem + '.json'), self.artifacts / (stem + '.process.json')

    def receipt(self, name='capture'):
        return json.loads(self.paths(name)[1].read_bytes())

    def assert_timeout(self):
        self.wait = True
        with self.assertRaises(subprocess.TimeoutExpired) as caught:
            self.case.run_client('capture')
        self.assertIs(caught.exception, self.process_exception)
        self.assertEqual((caught.exception.cmd, caught.exception.timeout),
                         self.process_exception_attributes)

    def test_successful_v4_workload_runs_through_report_validation(self):
        source = Path(__file__).resolve().parents[2]
        runner.OPTIONS.source = source
        script = self.case.short_save_load_script()
        data = contract_fixtures.workload_artifact('save_load', script)
        self.payload = (json.dumps(data) + '\n').encode('utf-8')
        self.case.test_v4_save_load_success()
        self.assertEqual(len(self.calls), 1)
        command = self.calls[0][0]
        self.assertEqual(command[command.index('--render-benchmark-schema') + 1], 'v4')
        self.assertNotIn('--no-audio', command)
        capture, _ = self.paths('save-load')
        self.assertEqual(json.loads(capture.read_bytes()), data)
        receipt = self.receipt('save-load')
        self.assertEqual(receipt['outcome'], 'returned')
        self.assertEqual(receipt['return_code'], 0)
        self.assertEqual(receipt['capture_status'], 'retained')
        self.assertEqual(receipt['capture_sha256'], hashlib.sha256(self.payload).hexdigest())

    def test_successful_v4_without_audio_runs_through_report_validation(self):
        runner.OPTIONS.source = Path(__file__).resolve().parents[2]
        script = self.case.short_save_load_script()
        data = contract_fixtures.workload_artifact('save_load', script)
        content = data['settings']['content_files']
        for path in list(content):
            if path.startswith('assets/audio/'):
                del content[path]
        content['assets/audio'] = None
        data['render_identity'] = runner.contract.render_identity(data['settings'])
        self.payload = (json.dumps(data) + '\n').encode('utf-8')
        self.case.test_v4_no_audio_success()
        self.assertEqual(len(self.calls), 1)
        command = self.calls[0][0]
        self.assertIn('--no-audio', command)
        self.assertEqual(command[command.index('--render-benchmark-schema') + 1], 'v4')
        self.assertFalse((self.runtime / 'assets/audio').exists())
        self.assertEqual(json.loads(self.paths('save-load')[0].read_bytes()), data)
        self.assertEqual(self.receipt('save-load')['return_code'], 0)

    def test_timeout_retains_complete_capture_after_runtime_cleanup(self):
        self.assert_timeout()
        shutil.rmtree(self.runtime)
        capture, receipt_path = self.paths()
        self.assertTrue(capture.is_file(), 'completed capture was lost on timeout')
        self.assertEqual(capture.read_bytes(), self.payload)
        self.assertTrue(receipt_path.is_file())
        receipt = self.receipt()
        self.assertEqual(receipt['schema'], 'luminumbra.render_runner_process.v1')
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['exception_type'], 'TimeoutExpired')
        self.assertIsNone(receipt['return_code'])
        self.assertEqual(receipt['timeout_seconds'], 180)
        self.assertEqual(receipt['command'], self.calls[0][0])
        self.assertEqual(receipt['cwd'], str(self.runtime))
        self.assertEqual(receipt['binary_path'], str(self.client))
        self.assertEqual(receipt['binary_sha256'], hashlib.sha256(self.client.read_bytes()).hexdigest())
        self.assertGreater(receipt['elapsed_seconds'], 0)
        self.assertEqual(receipt['capture_status'], 'retained')
        self.assertEqual(receipt['capture_path'], str(capture))
        self.assertEqual(receipt['capture_sha256'], hashlib.sha256(self.payload).hexdigest())
        self.assertNotIn('environment', receipt)
        self.assertEqual(receipt['evidence_errors'], [])

    def test_timeout_waits_for_capture_even_after_slow_interpreter_startup(self):
        self.startup_delay = 1
        self.assert_timeout()
        self.assertEqual(self.paths()[0].read_bytes(), self.payload)
        self.assertEqual(self.receipt()['capture_status'], 'retained')

    def test_timeout_missing_and_partial_captures_cannot_reuse_old_outputs(self):
        for payload, status in ((None, 'missing'), (b'{"incomplete":', 'invalid_json')):
            with self.subTest(status=status):
                self.payload = payload
                for path in (*self.paths(), self.runtime / 'capture.json'):
                    path.write_text('{"stale":true}', encoding="utf-8")
                unrelated = self.artifacts / 'other-case.json'
                unrelated.write_text('untouched', encoding="utf-8")
                self.assert_timeout()
                self.assertFalse(self.paths()[0].exists())
                self.assertEqual(self.receipt()['capture_status'], status)
                self.assertIsNone(self.receipt()['capture_sha256'])
                self.assertEqual(unrelated.read_text(encoding="utf-8"), 'untouched')

    def test_success_retains_bytes_and_existing_return_value(self):
        self.assertEqual(self.case.run_client('capture'), json.loads(self.payload))
        self.assertEqual(self.paths()[0].read_bytes(), self.payload)
        receipt = self.receipt()
        self.assertEqual((receipt['outcome'], receipt['return_code']), ('returned', 0))
        self.assertNotIn('exception_type', receipt)

    def test_success_still_requires_capture_and_loader_evidence(self):
        for payload, loaders, error in ((None, True, FileNotFoundError),
                                        (b'{', True, ValueError),
                                        (self.payload, False, AssertionError)):
            with self.subTest(payload=payload, loaders=loaders):
                self.payload, self.loaders = payload, loaders
                with self.assertRaises(error):
                    self.case.run_client('capture')
                self.assertEqual(self.receipt()['outcome'], 'returned')

    def test_expected_refusal_remains_success_without_capture(self):
        self.payload, self.code = None, 2
        self.assertIsInstance(self.case.run_client('capture', expected_exit=2), str)
        self.assertEqual(self.receipt()['return_code'], 2)
        self.assertEqual(self.receipt()['capture_status'], 'missing')

    def test_refusal_with_capture_and_unexpected_exit_still_fail(self):
        self.code = 2
        for expected in (0, 2):
            with self.subTest(expected=expected):
                with self.assertRaises(AssertionError):
                    self.case.run_client('capture', expected_exit=expected)
                self.assertEqual(self.paths()[0].read_bytes(), self.payload)
                self.assertEqual(self.receipt()['return_code'], 2)

    def test_spawn_failure_keeps_original_exception_and_receipt(self):
        error = OSError('fixture launch refused')
        with mock.patch.object(runner.subprocess, 'run', side_effect=error):
            with self.assertRaises(OSError) as caught:
                self.case.run_client('capture')
        self.assertIs(caught.exception, error)
        self.assertEqual(self.receipt()['outcome'], 'spawn_error')
        self.assertEqual(self.receipt()['exception_message'], str(error))
        self.assertIsNone(self.receipt()['return_code'])

    def test_missing_executable_is_not_launched_or_given_a_fabricated_hash(self):
        runner.OPTIONS.client = self.root / 'no-such-client'
        with self.assertRaises(FileNotFoundError):
            self.case.run_client('capture')
        self.assertEqual(self.calls, [])
        self.assertEqual(self.receipt()['outcome'], 'spawn_error')
        self.assertIsNone(self.receipt()['binary_sha256'])
        self.assertIsNone(self.receipt()['elapsed_seconds'])

    def write_failure(self, fail_receipt=False):
        original = runner._atomic_write
        def write(path, payload):
            if (path == self.paths()[1]) == fail_receipt:
                raise OSError('fixture evidence write refused')
            original(path, payload)
        return mock.patch.object(runner, '_atomic_write', side_effect=write)

    def test_capture_write_failure_cannot_mask_timeout(self):
        with self.write_failure(), redirect_stderr(io.StringIO()):
            self.assert_timeout()
        receipt = self.receipt()
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['capture_status'], 'write_error')
        self.assertTrue(receipt['evidence_errors'])
        self.assertFalse(self.paths()[0].exists())

    def test_receipt_write_failure_cannot_mask_timeout(self):
        output = io.StringIO()
        with self.write_failure(fail_receipt=True), redirect_stderr(output):
            self.assert_timeout()
        self.assertIn('process receipt:', output.getvalue())
        self.assertEqual(self.paths()[0].read_bytes(), self.payload)

    def test_capture_read_failure_cannot_mask_timeout(self):
        read_bytes = Path.read_bytes
        def read(path):
            if path == self.runtime / 'capture.json':
                raise OSError('fixture capture read refused')
            return read_bytes(path)
        with mock.patch.object(Path, 'read_bytes', read), redirect_stderr(io.StringIO()):
            self.assert_timeout()
        self.assertEqual(self.receipt()['capture_status'], 'read_error')
        self.assertTrue(self.receipt()['evidence_errors'])

    def test_parser_recursion_failure_cannot_mask_timeout(self):
        loads = json.loads
        def parse(payload):
            if payload == self.payload:
                raise RecursionError('fixture JSON parser recursion limit')
            return loads(payload)
        with mock.patch.object(runner.json, 'loads', side_effect=parse):
            self.assert_timeout()
        self.assertEqual(self.receipt()['capture_status'], 'invalid_json')
        self.assertFalse(self.paths()[0].exists())

    def test_receipt_write_failure_cannot_mask_spawn_error(self):
        error = OSError('fixture launch failure')
        with self.write_failure(fail_receipt=True), redirect_stderr(io.StringIO()), \
                mock.patch.object(runner.subprocess, 'run', side_effect=error):
            with self.assertRaises(OSError) as caught:
                self.case.run_client('capture')
        self.assertIs(caught.exception, error)

    def test_evidence_write_failure_after_normal_exit_is_a_failure(self):
        for receipt in (False, True):
            with self.subTest(receipt=receipt), self.write_failure(receipt), \
                    redirect_stderr(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, 'could not be retained'):
                    self.case.run_client('capture')

    def test_evidence_error_cannot_mask_existing_loader_assertion(self):
        self.loaders = False
        with self.write_failure(), redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(AssertionError, 'Static-model textures'):
                self.case.run_client('capture')

    def test_closed_or_failing_stderr_cannot_mask_timeout(self):
        closed = io.StringIO()
        closed.close()

        class FailingStream(io.StringIO):
            def write(self, text):
                raise OSError('fixture stderr write refused')

        for stream in (closed, FailingStream()):
            with self.subTest(stream=type(stream).__name__), redirect_stderr(stream):
                self.assert_timeout()
            receipt = self.receipt()
            self.assertEqual(receipt['outcome'], 'timed_out')
            self.assertEqual(receipt['capture_status'], 'retained')
            self.assertEqual(receipt['evidence_errors'], [])
            self.assertIn('log_tail', receipt)
            self.assertTrue(self.paths()[0].is_file())

    def test_process_elapsed_excludes_evidence_finalization(self):
        # Use a private clock object so subprocess's real deadline clock stays real.
        clock = mock.Mock()
        clock.monotonic.side_effect = [10.0, 12.5]
        with mock.patch.object(runner, 'time', clock):
            self.case.run_client('capture')
        self.assertEqual(self.receipt()['elapsed_seconds'], 2.5)
        self.assertEqual(clock.monotonic.call_count, 2)
        self.assertFalse(list(self.artifacts.glob('*.tmp')))

    def test_timeout_log_read_is_bounded_and_old_phase_is_not_retained(self):
        payload = b'EnsureSurfaceReadyNear: outside retained window\n' + b'x' * 20000
        reads = []
        class BoundedReader(io.BytesIO):
            def read(self, size=-1):
                reads.append(size)
                self_outer.assertGreaterEqual(size, 0)
                self_outer.assertLessEqual(size, runner.TIMEOUT_LOG_TAIL_BYTES)
                return super().read(size)
        self_outer = self
        log = mock.Mock()
        log.open.return_value = BoundedReader(payload)
        receipt = {'timeout_seconds': 180, 'evidence_errors': []}
        with redirect_stderr(io.StringIO()):
            runner._report_timeout(receipt, log)
        self.assertEqual(reads, [runner.TIMEOUT_LOG_TAIL_BYTES])
        self.assertEqual(receipt['log_tail'], 'x' * runner.TIMEOUT_LOG_TAIL_BYTES)
        self.assertIsNone(receipt['last_observed_phase'])

    def test_timeout_partial_first_line_cannot_supply_phase(self):
        fragment = b'EnsureSurfaceReadyNear: partial first line\n'
        payload = b'x' * 10000 + fragment + b'q' * (runner.TIMEOUT_LOG_TAIL_BYTES - len(fragment))
        log = self.root / 'partial-timeout.log'
        log.write_bytes(payload)
        receipt = {'timeout_seconds': 180, 'evidence_errors': []}
        with redirect_stderr(io.StringIO()):
            runner._report_timeout(receipt, log)
        self.assertTrue(receipt['log_tail'].startswith(fragment.decode('ascii')))
        self.assertIsNone(receipt['last_observed_phase'])

    def test_timeout_unfinished_final_phase_preserves_last_complete_phase(self):
        log = self.root / 'unfinished-timeout.log'
        log.write_bytes(b'EnsureSurfaceReadyNear: completed phase\n'
                        b'EnsureSurfaceReadyNear: unfinished phase')
        receipt = {'timeout_seconds': 180, 'evidence_errors': []}
        with redirect_stderr(io.StringIO()):
            runner._report_timeout(receipt, log)
        self.assertTrue(receipt['log_tail'].endswith('unfinished phase'))
        self.assertEqual(receipt['last_observed_phase'], 'EnsureSurfaceReadyNear: completed phase')

    def test_timeout_tail_invalid_utf8_and_read_failure_are_best_effort(self):
        log = self.root / 'invalid-timeout.log'
        log.write_bytes(b'\xff\nEnsureSurfaceReadyNear: final complete phase\n')
        receipt = {'timeout_seconds': 180, 'evidence_errors': []}
        with redirect_stderr(io.StringIO()):
            runner._report_timeout(receipt, log)
        self.assertIn('\ufffd', receipt['log_tail'])
        self.assertEqual(receipt['last_observed_phase'], 'EnsureSurfaceReadyNear: final complete phase')
        with mock.patch.object(Path, 'open', side_effect=OSError('fixture read refused')), \
                redirect_stderr(io.StringIO()):
            runner._report_timeout(receipt, log)
        self.assertEqual(receipt['log_tail'], '')
        self.assertIsNone(receipt['last_observed_phase'])
        self.assertIn('timeout log read: fixture read refused', receipt['evidence_errors'])

    def test_timeout_reports_log_tail_and_last_observed_phase(self):
        slow = self.root / 'slow_child.py'
        slow.write_text(SLOW_CHILD, encoding="utf-8")
        log = self.root / 'timeout.log'
        artifact = self.root / 'timeout-capture.json'
        retained = self.root / 'timeout-retained.json'
        receipt_path = self.root / 'timeout-process.json'
        def short_run(command, **kwargs):
            return self.run_ready_child(command, self.root / 'slow.ready', **kwargs)

        stderr = io.StringIO()
        with mock.patch.object(runner.subprocess, 'run', side_effect=short_run), \
                redirect_stderr(stderr):
            with self.assertRaises(subprocess.TimeoutExpired):
                runner._run_process([sys.executable, str(slow), str(self.root / 'slow.ready')], cwd=self.root,
                                    environment=os.environ.copy(), log=log,
                                    artifact=artifact, retained=retained,
                                    receipt_path=receipt_path)
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['last_observed_phase'],
                         'EnsureSurfaceReadyNear/surface-build batch 3')
        self.assertEqual(len(receipt['log_tail'].encode('utf-8')), 8 * 1024)
        self.assertIn('unrelated trailing line', receipt['log_tail'])
        message = stderr.getvalue()
        self.assertIn('render runner: timed out after', message)
        self.assertIn('unrelated trailing line', message)
        self.assertIn('EnsureSurfaceReadyNear/surface-build batch 3', message)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
