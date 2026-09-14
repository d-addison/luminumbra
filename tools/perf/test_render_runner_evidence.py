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
artifact, payload, code, wait, loaders = sys.argv[1:]
if loaders == '1':
    print('Static-model textures registered (3 models).', flush=True)
    for part in ('trunk', 'branches', 'leaves'):
        print(f"tree_small_02_{part}.lmesh' (3 verts, 3 indices)", flush=True)
if payload != '-':
    pathlib.Path(artifact).write_bytes(base64.b64decode(payload))
if wait == '1':
    time.sleep(30)
sys.exit(int(code))
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
        self.child.write_text(CHILD)
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
                         str(self.code), str(int(self.wait)), str(int(self.loaders))]
        # Only the tiny fixture child gets a shorter timeout. Production argv,
        # cwd, environment and the 180-second limit above are observed unchanged.
        try:
            return self.original_run(child_command, **dict(kwargs, timeout=0.75 if self.wait else 5))
        except subprocess.TimeoutExpired as error:
            self.process_exception = error
            self.process_exception_attributes = (error.cmd, error.timeout)
            raise

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

    def test_timeout_missing_and_partial_captures_cannot_reuse_old_outputs(self):
        for payload, status in ((None, 'missing'), (b'{"incomplete":', 'invalid_json')):
            with self.subTest(status=status):
                self.payload = payload
                for path in (*self.paths(), self.runtime / 'capture.json'):
                    path.write_text('{"stale":true}')
                unrelated = self.artifacts / 'other-case.json'
                unrelated.write_text('untouched')
                self.assert_timeout()
                self.assertFalse(self.paths()[0].exists())
                self.assertEqual(self.receipt()['capture_status'], status)
                self.assertIsNone(self.receipt()['capture_sha256'])
                self.assertEqual(unrelated.read_text(), 'untouched')

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

    def test_process_elapsed_excludes_evidence_finalization(self):
        # Use a private clock object so subprocess's real deadline clock stays real.
        clock = mock.Mock()
        clock.monotonic.side_effect = [10.0, 12.5]
        with mock.patch.object(runner, 'time', clock):
            self.case.run_client('capture')
        self.assertEqual(self.receipt()['elapsed_seconds'], 2.5)
        self.assertEqual(clock.monotonic.call_count, 2)
        self.assertFalse(list(self.artifacts.glob('*.tmp')))


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
