#!/usr/bin/env python3
"""Device-free run_client controls with real and injected process timeouts."""
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


parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--runner', type=Path, default=Path(__file__).with_name('test_render_runner.py'))
options, remaining = parser.parse_known_args()
spec = importlib.util.spec_from_file_location('runner_under_test', options.runner)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

CHILD = r'''
import base64, pathlib, sys, time
artifact, payload, code, loaders, ready, startup_delay = sys.argv[1:]
time.sleep(float(startup_delay))
if loaders == '1':
    print('Static-model textures registered (3 models).', flush=True)
    for part in ('trunk', 'branches', 'leaves'):
        print(f"tree_small_02_{part}.lmesh' (3 verts, 3 indices)", flush=True)
if payload != '-':
    pathlib.Path(artifact).write_bytes(base64.b64decode(payload))
if ready != '-':
    print('A' * 20000, flush=True)
    print('EnsureSurfaceReadyNear: draining generation jobs (near 0,0)...', flush=True)
    print('EnsureSurfaceReadyNear/surface-build batch 3', flush=True)
    print('unrelated trailing line', flush=True)
    pathlib.Path(ready).touch()
    while True:
        time.sleep(1)
sys.exit(int(code))
'''


TIMEOUT_LOG = (b'A' * 20000 + b'\n'
               b'EnsureSurfaceReadyNear: draining generation jobs (near 0,0)...\n'
               b'EnsureSurfaceReadyNear/surface-build batch 3\n'
               b'unrelated trailing line\n')


SLOW_CHILD = r'''
import pathlib, sys, time
print('A' * 20000, flush=True)
print('EnsureSurfaceReadyNear: draining generation jobs (near 0,0)...', flush=True)
print('EnsureSurfaceReadyNear/surface-build batch 3', flush=True)
print('unrelated trailing line', flush=True)
pathlib.Path(sys.argv[1]).touch()
time.sleep(30)
'''


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
        self.real_timeout = True
        self.children = []
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
        if self.wait and not self.real_timeout:
            if self.payload is not None:
                Path(artifact).write_bytes(self.payload)
            kwargs['stdout'].write(TIMEOUT_LOG)
            kwargs['stdout'].flush()
            error = subprocess.TimeoutExpired(command, kwargs['timeout'])
            self.process_exception = error
            self.process_exception_attributes = (error.cmd, error.timeout)
            raise error
        ready = self.root / 'ready'
        waiting = self.wait and self.real_timeout
        child_command = [sys.executable, '-I', '-B', str(self.child), artifact, payload,
                         str(self.code), str(int(self.loaders)),
                         str(ready) if waiting else '-', str(self.startup_delay)]
        if waiting:
            ready.unlink(missing_ok=True)
            popen = subprocess.Popen

            def start(*args, **options):
                child = popen(*args, **options)
                self.children.append(child)
                wait = child.wait

                def wait_for_timeout(timeout=None):
                    if timeout is not None:
                        # Only a deadlock detector: wait for the real child to
                        # finish writing before requesting an immediate timeout.
                        deadline = time.monotonic() + 60
                        while not ready.exists():
                            self.assertIsNone(child.poll(), 'child exited before capture')
                            self.assertLess(time.monotonic(), deadline,
                                            'child never became ready')
                            time.sleep(0.01)
                        self.assertIsNone(child.poll(), 'child must still be running')
                        return wait(timeout=0)
                    return wait()

                child.wait = wait_for_timeout
                child.kill = mock.Mock(wraps=child.kill)
                return child

            with mock.patch.object(subprocess, 'Popen', side_effect=start):
                try:
                    return self.original_run(child_command, **kwargs)
                except subprocess.TimeoutExpired as error:
                    self.process_exception = error
                    self.process_exception_attributes = (error.cmd, error.timeout)
                    raise
        return self.original_run(child_command, **dict(kwargs, timeout=5))

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
        if self.real_timeout:
            self.children[-1].kill.assert_called_once_with()
            self.assertIsNotNone(self.children[-1].returncode, 'timed out child was not reaped')

    def test_atomic_publication_replaces_existing_files_without_rename(self):
        paths = self.paths()
        for path in paths:
            path.write_bytes(b'previous evidence')
        with mock.patch.object(runner.os, 'rename', side_effect=FileExistsError(
                'destination already exists')), \
                mock.patch.object(runner.os, 'replace', wraps=os.replace) as replace:
            for path in paths:
                runner._atomic_write(path, b'current evidence')
                self.assertEqual(path.read_bytes(), b'current evidence')
        self.assertEqual(replace.call_count, 2)
        self.assertFalse(list(self.artifacts.glob('*.tmp')))

    def test_timeout_retains_complete_capture_after_runtime_cleanup(self):
        self.real_timeout = True
        self.assert_retained_timeout_capture()
        self.assertEqual(len(self.children), 1)
        self.children[0].kill.assert_called_once_with()
        self.assertIsNotNone(self.children[0].returncode, 'timed out child was not reaped')
        self.assertEqual(self.receipt()['last_observed_phase'],
                         'EnsureSurfaceReadyNear/surface-build batch 3')
        self.assertEqual(len(self.receipt()['log_tail'].encode('utf-8')),
                         runner.TIMEOUT_LOG_TAIL_BYTES)

    def test_injected_timeout_retains_complete_capture_after_runtime_cleanup(self):
        self.real_timeout = False
        self.assert_retained_timeout_capture()

    def assert_retained_timeout_capture(self):
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
        self.assertEqual(receipt['binary_path'], str(self.client.resolve(strict=True)))
        self.assertEqual(receipt['binary_sha256'], hashlib.sha256(self.client.read_bytes()).hexdigest())
        self.assertEqual(receipt['binary_identity_status'], 'verified')
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
        clock.perf_counter.side_effect = [10.0, 12.5]
        with mock.patch.object(runner, 'time', clock):
            self.case.run_client('capture')
        self.assertEqual(self.receipt()['elapsed_seconds'], 2.5)
        self.assertEqual(clock.perf_counter.call_count, 2)
        self.assertFalse(list(self.artifacts.glob('*.tmp')))

    def test_executable_change_invalidates_receipt_on_return_and_timeout(self):
        executable = self.root / Path(sys.executable).name
        runner.OPTIONS.client = executable
        for timed_out in (False, True):
            with self.subTest(timed_out=timed_out):
                shutil.copy2(sys.executable, executable)
                error = subprocess.TimeoutExpired([str(executable)], 180)

                def launch(command, **kwargs):
                    metadata = executable.stat()
                    os.utime(executable, ns=(metadata.st_atime_ns,
                                            metadata.st_mtime_ns + 2000000000))
                    Path(command[command.index('--render-benchmark') + 1]).write_bytes(self.payload)
                    if timed_out:
                        raise error
                    return subprocess.CompletedProcess(command, 0)

                with mock.patch.object(runner.subprocess, 'run', side_effect=launch), \
                        redirect_stderr(io.StringIO()), \
                        self.assertRaises(subprocess.TimeoutExpired if timed_out else OSError) as caught:
                    self.case.run_client('capture')
                if timed_out:
                    self.assertIs(caught.exception, error)
                receipt = self.receipt()
                self.assertIsNone(receipt['binary_sha256'])
                self.assertEqual(receipt['binary_identity_status'], 'changed')
                self.assertTrue(receipt['evidence_errors'])
                self.assertEqual(self.paths()[0].read_bytes(), self.payload)

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
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            self.assert_timeout()
        self.assert_timeout_diagnostics(self.receipt(), stderr.getvalue())

    def test_injected_timeout_reports_log_tail_and_last_observed_phase(self):
        log = self.root / 'timeout.log'
        artifact = self.root / 'timeout-capture.json'
        retained = self.root / 'timeout-retained.json'
        receipt_path = self.root / 'timeout-process.json'
        error = subprocess.TimeoutExpired([sys.executable], 180)

        def timeout(command, **kwargs):
            self.assertEqual(kwargs['timeout'], 180)
            kwargs['stdout'].write(TIMEOUT_LOG)
            kwargs['stdout'].flush()
            raise error

        stderr = io.StringIO()
        with mock.patch.object(runner.subprocess, 'run', side_effect=timeout), \
                redirect_stderr(stderr):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                runner._run_process([sys.executable], cwd=self.root,
                                    environment=os.environ.copy(), log=log,
                                    artifact=artifact, retained=retained,
                                    receipt_path=receipt_path)
        self.assertIs(caught.exception, error)
        receipt = json.loads(receipt_path.read_text())
        self.assert_timeout_diagnostics(receipt, stderr.getvalue())

    def test_ready_child_timeout_reports_log_tail_and_last_observed_phase(self):
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
                runner._run_process([sys.executable, '-I', '-B', str(slow),
                                     str(self.root / 'slow.ready')], cwd=self.root,
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

    def assert_timeout_diagnostics(self, receipt, message):
        self.assertEqual(receipt['outcome'], 'timed_out')
        self.assertEqual(receipt['last_observed_phase'],
                         'EnsureSurfaceReadyNear/surface-build batch 3')
        self.assertEqual(len(receipt['log_tail'].encode('utf-8')), 8 * 1024)
        self.assertIn('unrelated trailing line', receipt['log_tail'])
        self.assertIn('render runner: timed out after', message)
        self.assertIn('unrelated trailing line', message)
        self.assertIn('EnsureSurfaceReadyNear/surface-build batch 3', message)


class RenderRunnerRegistrationTests(unittest.TestCase):
    def test_real_client_registrations_share_native_graphics_admission(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        # Evaluate only the actual registration lists/loop, never the native
        # target definitions or engine project. Discovery records properties on
        # sentinel tests instead of launching GoogleTest executables.
        start = source.index('set(LUMINUMBRA_GTEST_TARGETS\n')
        end = source.index('\nendforeach()', source.index('foreach(_lum_gtest_target'))
        native = source[start:end + len('\nendforeach()')]
        integrations = source[source.index('add_test(NAME RenderBenchmarkAssets'):]
        manual = source[source.index('add_test(NAME ShieldRtTracerProfileGpu'):
                        source.index('#  per-flag fixtures')]
        cpu = source[source.index('add_test(NAME ShieldRtSpike'):
                     source.index('# Simulation telemetry captures')]
        gpu_cases = {
            'ShieldRtTracerProfileGpu': 'shieldrt_tracer_profile_gpu',
            'ShieldRtFarFieldParityGpu': 'shieldrt_far_field_parity_gpu',
            'ShieldRtFarFieldGbufferGpu': 'shieldrt_far_field_gbuffer_gpu',
            'ShieldRtFarFieldMaxMipGpu': 'shieldrt_far_field_maxmip_gpu',
        }
        cases = {
            'RenderTraversalIntegrated': 'test_traversal_twice',
            'RenderBenchmarkImplicitCreationFailure': 'test_implicit_creation_failure',
            'RenderBenchmarkOverrideCollection': 'test_gl_debug_override_collection',
            'RenderTraversalEmbeddedPresetRefusal': 'test_embedded_preset_refusal',
        }
        with tempfile.TemporaryDirectory(prefix='render registration ') as temporary:
            fixture = Path(temporary)
            for optional in ('OFF', 'ON'):
                with self.subTest(optional_graphics=optional):
                    project = fixture / optional
                    project.mkdir()
                    build = project / 'build with spaces'
                    # Five inert imported targets; metadata inspection never executes them.
                    targets = (*gpu_cases.values(), 'shieldrt_spike_bench')
                    executables = {}
                    imported = []
                    for target in targets:
                        executable = project / (target + ' sentinel.exe')
                        executable.write_text('metadata only\n', encoding='utf-8')
                        executable.chmod(0o700)
                        executables[target] = executable.as_posix()
                        imported.append(
                            f'add_executable({target} IMPORTED)\n'
                            f'set_target_properties({target} PROPERTIES '
                            f'IMPORTED_LOCATION [=[{executable.as_posix()}]=])')
                    imported_targets = '\n'.join(imported)
                    cmake = f"""cmake_minimum_required(VERSION 3.20)
project(RenderRegistration LANGUAGES NONE)
enable_testing()
set(CMAKE_SOURCE_DIR [=[{root.as_posix()}]=])
set(Python3_EXECUTABLE [=[{Path(sys.executable).as_posix()}]=])
add_executable(luminumbra_client_app IMPORTED)
set_target_properties(luminumbra_client_app PROPERTIES
    IMPORTED_LOCATION [=[{Path(sys.executable).as_posix()}]=])
function(gtest_discover_tests target)
    cmake_parse_arguments(discovery "NO_PRETTY_VALUES" "DISCOVERY_TIMEOUT;TEST_LIST" "PROPERTIES" ${{ARGN}})
    if(NOT discovery_DISCOVERY_TIMEOUT EQUAL 120 OR discovery_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unexpected discovery arguments")
    endif()
    add_test(NAME native.${{target}} COMMAND "${{CMAKE_COMMAND}}" -E false)
    if(discovery_TEST_LIST)
        set(${{discovery_TEST_LIST}} native.${{target}} PARENT_SCOPE)
    endif()
    if(discovery_PROPERTIES)
        set_tests_properties(native.${{target}} PROPERTIES ${{discovery_PROPERTIES}})
    endif()
endfunction()
set(LUMINUMBRA_ENABLE_DILIGENT {optional})
set(LUMINUMBRA_PILOT_SHADER_TEST_AVAILABLE {optional})
{native}
file(WRITE "${{CMAKE_BINARY_DIR}}/gl-targets.txt" "${{LUMINUMBRA_GL_GTEST_TARGETS}}")
{imported_targets}
{manual}
{cpu}
{integrations}
"""
                    (project / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
                    subprocess.run(['cmake', '-S', str(project), '-B', str(build),
                                    '-G', 'Ninja'], check=True, capture_output=True, text=True)
                    result = subprocess.run(
                        ['ctest', '--test-dir', str(build), '--show-only=json-v1'],
                        check=True, capture_output=True, text=True)
                    tests = {test['name']: test for test in json.loads(result.stdout)['tests']}
                    properties = {
                        name: {item['name']: item['value'] for item in test['properties']}
                        for name, test in tests.items()
                    }
                    self.assertEqual({name for name in tests if not name.startswith('native.')},
                                     set(cases) | set(gpu_cases) | {
                                          'RenderBenchmarkAssets', 'ShieldRtSpike'})
                    gl_targets = (build / 'gl-targets.txt').read_text(encoding='utf-8').split(';')
                    self.assertGreaterEqual(len(gl_targets), 17)
                    self.assertIn('render_smoke_test', gl_targets)
                    optional_targets = {
                        'rhi_device_bringup_test', 'rhi_pilot_flip_test',
                        'pilot_shader_reflection_test',
                    }
                    required_gl_targets = {
                        'render_smoke_test', 'render_capture_test',
                        'async_readback_ring_test', 'dual_backend_flip_test',
                        'registry_ownership_test', 'environment_brdf_test',
                        'pass_context_test', 'reversed_z_depth_test',
                        'shader_reflection_test', 'waterfall_visual_test',
                        'particle_determinism_test', 'far_lod_worker_test',
                        'ui_smoke_test', 'ui_page_load_test', 'worldgen_preview_test',
                        'runtime_world_visual_validation_test',
                    }
                    # Membership includes optional targets even when discovery omits them.
                    expected_gl = required_gl_targets | optional_targets
                    self.assertCountEqual(gl_targets, expected_gl)
                    expected_registered_gl = {'OFF': required_gl_targets, 'ON': expected_gl}[optional]
                    self.assertEqual({name[7:] for name in tests
                                      if name.startswith('native.') and name[7:] in expected_gl},
                                     expected_registered_gl)
                    self.assertEqual({name[7:] for name, props in properties.items()
                                      if name.startswith('native.') and
                                      props.get('RESOURCE_LOCK') == ['luminumbra_gl_context']},
                                     expected_registered_gl)
                    active_gl = set(gl_targets)
                    if optional == 'OFF':
                        active_gl -= optional_targets
                    for target in sorted(active_gl):
                        name = 'native.' + target
                        self.assertIn(name, tests)
                        self.assertEqual(properties[name].get('RESOURCE_LOCK'),
                                         ['luminumbra_gl_context'], name)
                    self.assertNotIn('RESOURCE_LOCK', properties['native.common_tests'])
                    for target in ('rhi_device_bringup_test', 'rhi_pilot_flip_test',
                                   'pilot_shader_reflection_test'):
                        self.assertEqual('native.' + target in tests, optional == 'ON')
                    for name, method in cases.items():
                        with self.subTest(test=name):
                            command = tests[name]['command']
                            self.assertEqual(Path(command[0]).resolve(), Path(sys.executable).resolve())
                            self.assertEqual(Path(command[1]), root / 'tools/perf/test_render_runner.py')
                            self.assertEqual(command[2], '--client')
                            self.assertEqual(Path(command[3]).resolve(), Path(sys.executable).resolve())
                            self.assertEqual(command[4:], [
                                '--source', root.as_posix(),
                                '--assets', (build / 'test-artifacts/render-synthetic-content').as_posix(),
                                '--artifacts', (build / 'test-artifacts/render-runner').as_posix(),
                                'RenderRunnerTests.' + method])
                            self.assertEqual(properties[name]['TIMEOUT'], 600)
                            self.assertEqual(properties[name]['FIXTURES_REQUIRED'],
                                             ['render_benchmark_assets'])
                            self.assertEqual(properties[name].get('RESOURCE_LOCK'),
                                             properties['native.render_smoke_test']['RESOURCE_LOCK'])
                            for forbidden in ('DISABLED', 'SKIP_RETURN_CODE', 'SKIP_REGULAR_EXPRESSION',
                                              'LABELS', 'RUN_SERIAL'):
                                self.assertNotIn(forbidden, properties[name])
                    for name, target in gpu_cases.items():
                        with self.subTest(manual_gpu=name):
                            self.assertEqual(tests[name]['command'], [executables[target]])
                            self.assertEqual(properties[name]['TIMEOUT'], 600)
                            self.assertEqual(properties[name]['LABELS'], ['gpu', 'manual', 'perf'])
                            self.assertEqual(properties[name].get('RESOURCE_LOCK'),
                                             properties['native.render_smoke_test']['RESOURCE_LOCK'])
                            for forbidden in ('DISABLED', 'SKIP_RETURN_CODE', 'SKIP_REGULAR_EXPRESSION',
                                              'FIXTURES_REQUIRED', 'RUN_SERIAL'):
                                self.assertNotIn(forbidden, properties[name])
                    self.assertEqual(tests['ShieldRtSpike']['command'],
                                     [executables['shieldrt_spike_bench']])
                    self.assertEqual(properties['ShieldRtSpike']['TIMEOUT'], 600)
                    self.assertEqual(properties['ShieldRtSpike']['LABELS'], ['manual', 'perf', 'spike'])
                    self.assertNotIn('RESOURCE_LOCK', properties['ShieldRtSpike'])
                    assets = tests['RenderBenchmarkAssets']['command']
                    self.assertEqual(Path(assets[0]).resolve(), Path(sys.executable).resolve())
                    self.assertEqual(Path(assets[1]), root / 'tools/perf/synthetic_render_content.py')
                    self.assertEqual(assets[2:], [
                        '--root', (build / 'test-artifacts/render-synthetic-content').as_posix()])
                    self.assertEqual(properties['RenderBenchmarkAssets']['FIXTURES_SETUP'],
                                     ['render_benchmark_assets'])
                    self.assertEqual(properties['RenderBenchmarkAssets']['TIMEOUT'], 30)
                    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
                    self.assertNotRegex(cache, r'(?m)^CMAKE_(?:C|CXX)_COMPILER:')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
