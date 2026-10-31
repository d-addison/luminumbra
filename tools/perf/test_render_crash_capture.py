#!/usr/bin/env python3
"""Device-free Windows debug-event, evidence and process-outcome contracts."""
import argparse
import ctypes as C
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_crash_capture as capture
import render_runner_process as runner

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--runner', type=Path, default=Path(__file__).with_name('test_render_runner.py'))
parser.add_argument('--native-windows-smoke', action='store_true')
parser.add_argument('--smoke-artifacts', type=Path)
options, remaining = parser.parse_known_args()
if options.native_windows_smoke and (os.name != 'nt' or options.smoke_artifacts is None):
    parser.error('native Windows smoke requires Windows and --smoke-artifacts')
spec = importlib.util.spec_from_file_location('device_runner', options.runner)
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)

CRASH = 0xC0000005


def event(kind, *, code=0, first=0):
    result = capture.DebugEvent(kind=kind, pid=42, tid=17)
    if kind == 1:
        result.data.exception.first_chance = first
        result.data.exception.record.code = code
        result.data.exception.record.address = 0x12345678
        result.data.exception.record.count = 2
        result.data.exception.record.information[0] = 1
        result.data.exception.record.information[1] = 0x98765432
    elif kind == 3:
        result.data.create.file, result.data.create.base = 11, 0x140000000
    elif kind == 6:
        result.data.load.file, result.data.load.base = 12, 0x180000000
    elif kind == 7:
        # Win32 UNLOAD_DLL_DEBUG_INFO is one pointer at payload offset zero.
        # Write raw ABI bytes so the fixture cannot repeat a ctypes layout bug.
        C.memmove(C.addressof(result.data), struct.pack('<Q', 0x180000000), 8)
    elif kind == 5:
        result.data.exit_code = code
    return result


def dump_bytes(code=CRASH, tid=17):
    # An AMD64 minidump with one thread stack/context, one module and exception.
    threads_offset = 32 + 3 * 12
    modules_offset = threads_offset + 52
    exception_offset = modules_offset + 112
    context_offset = exception_offset + 168
    stack_offset = context_offset + 1232
    header = struct.pack('<8I', 0x504d444d, 0xa793, 3, 32, 0, 0, 0, 0)
    directory = b''.join(struct.pack('<3I', *row) for row in (
        (3, 52, threads_offset), (4, 112, modules_offset), (6, 168, exception_offset)))
    thread = bytearray(48)
    struct.pack_into('<I', thread, 0, tid)
    struct.pack_into('<4I', thread, 32, 8, stack_offset, 1232, context_offset)
    exception = bytearray(168)
    struct.pack_into('<I', exception, 0, tid)
    struct.pack_into('<I', exception, 8, code)
    struct.pack_into('<2I', exception, 160, 1232, context_offset)
    return (header + directory + struct.pack('<I', 1) + thread
            + struct.pack('<I', 1) + bytes(108) + exception + bytes(1232) + b'stack123')


class Backend:
    def __init__(self, images, events):
        self.images, self.events = images, list(events)
        self.streams, self.resumed, self.closed, self.dumps = [], [], [], []
        self.failure = None
        self.dump_payload = dump_bytes()
        self.child = None
        self.timeouts = []

    def wait(self, milliseconds):
        self.timeouts.append(milliseconds)
        if self.failure:
            raise self.failure
        return self.events.pop(0) if self.events else None

    def image(self, handle):
        name = self.images[handle]
        source = name.open('rb')
        self.streams.append(source)
        return name, source

    def close(self, handle):
        self.closed.append(handle)

    def resume(self, current, status):
        self.resumed.append((current.kind, status))
        if current.kind == 5:
            self.child.returncode = current.data.exit_code

    def detach(self, pid):
        if pid != 42:
            raise AssertionError('wrong child detached')

    def dump(self, child, current, output, deadline):
        if child is not self.child or current.kind != 1:
            raise AssertionError('dump must describe the currently stopped child')
        self.dumps.append(Path(output.name))
        if isinstance(self.dump_payload, Exception):
            raise self.dump_payload
        if self.dump_payload is not None:
            output.write(self.dump_payload)
        return {'rip': current.data.exception.record.address, 'rsp': 0x1234}


class Child:
    def __init__(self):
        self.pid = 42
        self.returncode = None
        self.kills, self.waits = 0, []

    def poll(self):
        return self.returncode

    def kill(self):
        self.kills += 1
        self.returncode = 1

    def wait(self, timeout=None):
        self.waits.append(timeout)
        if self.returncode is None:
            raise AssertionError('waiting on a stopped debug child')
        return self.returncode


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.runtime, self.artifacts = self.root / 'runtime', self.root / 'artifacts'
        self.runtime.mkdir()
        self.artifacts.mkdir()
        self.exe, self.mesa = self.runtime / 'client.exe', self.runtime / 'libgallium_wgl.dll'
        self.exe.write_bytes(b'current executable bytes')
        self.mesa.write_bytes(b'loaded Mesa bytes')
        self.exe.with_suffix('.pdb').write_bytes(b'available PDB')
        self.images = {11: self.exe, 12: self.mesa}
        self.receipt = dict(binary_path=str(self.exe),
                            binary_sha256=hashlib.sha256(self.exe.read_bytes()).hexdigest(),
                            evidence_errors=[])

    def launch(self, events, *, timeout=180, expected=0, backend=None):
        self.backend = backend or Backend(self.images, events)
        self.child = Child()
        self.backend.child = self.child
        with mock.patch.object(capture.subprocess, 'Popen', return_value=self.child) as spawn:
            try:
                return capture.monitored_run([str(self.exe), '--no-audio'], cwd=self.runtime,
                    env={}, stdout=io.BytesIO(), stderr=subprocess.STDOUT, timeout=timeout,
                    check=False, receipt=self.receipt, evidence_parent=self.artifacts,
                    backend=self.backend, expected_exit=expected)
            finally:
                self.assertEqual(spawn.call_count, 1, 'must never duplicate execution')
                self.assertEqual(spawn.call_args.kwargs['creationflags'], 2)
                self.assertTrue(all(source.closed for source in self.backend.streams))

    def crash_events(self):
        return [event(3), event(6), event(1, code=0x80000003, first=1),
                event(1, code=CRASH, first=1), event(1, code=CRASH), event(5, code=CRASH)]

    def test_success_and_expected_refusal_do_not_dump_copy_or_create_directories(self):
        for code in (0, 2):
            with self.subTest(code=code), mock.patch.object(capture.Evidence, 'copy') as copy:
                result = self.launch([event(3), event(6), event(5, code=code)], expected=code)
                self.assertEqual(result.returncode, code)
                self.assertEqual(self.backend.dumps, [])
                copy.assert_not_called()
                self.assertEqual(list(self.artifacts.iterdir()), [])
                self.assertEqual(self.receipt['evidence_errors'], [])
                self.assertEqual(self.child.kills, 0)

    def test_crash_retains_matching_images_symbols_dump_and_access_before_cleanup(self):
        result = self.launch(self.crash_events())
        self.assertEqual(result.returncode, CRASH)
        data = self.receipt['native_diagnostics']
        self.assertEqual((data['status'], data['dump_status']), ('retained', 'retained'))
        self.assertEqual((data['access_type'], data['accessed_address']), ('write', 0x98765432))
        self.assertEqual(data['registers']['rip'], 0x12345678)
        records = {entry['role']: entry for entry in data['files']}
        self.assertEqual(records['executable']['sha256'], self.receipt['binary_sha256'])
        self.assertEqual(records['module']['sha256'], hashlib.sha256(self.mesa.read_bytes()).hexdigest())
        self.assertIn('symbol_candidate', records)
        self.assertEqual(len(data['modules']), 2)
        self.assertEqual(self.backend.resumed[2:5], [(1, 0x10002), (1, 0x80010001), (1, 0x80010001)])
        for path in self.runtime.iterdir():
            path.unlink()  # Windows also proves no leaked source handle remains open.
        self.runtime.rmdir()
        for record in records.values():
            self.assertEqual(hashlib.sha256(Path(record['path']).read_bytes()).hexdigest(), record['sha256'])
        capture.validate_dump(Path(data['dump_path']), CRASH, 17)

    def test_missing_partial_stale_and_failed_dumps_never_claim_retained(self):
        for payload in (None, b'MDMP', dump_bytes(tid=99), OSError('dump denied')):
            with self.subTest(payload=type(payload).__name__):
                backend = Backend(self.images, self.crash_events())
                backend.dump_payload = payload
                self.assertEqual(self.launch([], backend=backend).returncode, CRASH)
                data = self.receipt['native_diagnostics']
                self.assertEqual(data['status'], 'incomplete')
                self.assertEqual(data['dump_status'], 'failed')
                self.assertNotIn('dump_path', data)
                self.assertFalse((Path(data['directory']) / 'exception.dmp').exists())
                self.assertTrue(data['executable_retained'])

    def test_wrong_head_image_is_never_published_as_matching(self):
        self.receipt['binary_sha256'] = '0' * 64
        self.assertEqual(self.launch(self.crash_events()).returncode, CRASH)
        data = self.receipt['native_diagnostics']
        self.assertEqual(data['status'], 'incomplete')
        self.assertFalse(data['executable_retained'])
        self.assertFalse(any(item['role'] == 'executable' for item in data['files']))
        self.assertTrue(any('does not match' in error for error in self.receipt['evidence_errors']))

    def test_dump_rollback_only_removes_the_file_created_by_this_capture(self):
        target = self.artifacts / 'exception.dmp'
        target.write_bytes(b'previous evidence')
        metadata = target.lstat()
        identity = (metadata.st_dev, metadata.st_ino)
        capture.remove_owned(target, None)
        capture.remove_owned(target, (metadata.st_dev, metadata.st_ino + 1))
        self.assertEqual(target.read_bytes(), b'previous evidence')
        capture.remove_owned(target, identity)
        self.assertFalse(target.exists())
        capture.remove_owned(target, identity)

        backend = Backend(self.images, self.crash_events())
        original_dump = backend.dump
        original_validate = capture.validate_dump_stream
        owned = []
        def dump(child, current, output, deadline):
            owned.append(output)
            return original_dump(child, current, output, deadline)
        def validate(source, code, tid):
            self.assertIs(source, owned[0])
            original_validate(source, code, tid)
        backend.dump = dump
        with mock.patch.object(capture, 'validate_dump_stream', side_effect=validate) as checked, \
                mock.patch.object(capture, 'validate_dump', side_effect=AssertionError('reopened dump')):
            self.assertEqual(self.launch([], backend=backend).returncode, CRASH)
            checked.assert_called_once()
        self.assertEqual(self.receipt['native_diagnostics']['dump_status'], 'retained')

    def test_unexpected_nonzero_retains_images_without_fabricating_exception_dump(self):
        self.assertEqual(self.launch([event(3), event(6), event(5, code=2)]).returncode, 2)
        self.assertTrue(self.receipt['native_diagnostics']['executable_retained'])
        self.assertEqual(self.backend.dumps, [])
        self.assertEqual(self.receipt['native_diagnostics']['dump_status'], 'missing')

    def test_timeout_kills_and_reaps_before_copy_and_preserves_original_timeout(self):
        backend = Backend(self.images, [event(3), event(6)])
        clock_values = iter([0, 0, 0, 181])
        real_clock = capture.time.monotonic
        def clock():
            return next(clock_values, real_clock())
        original = capture.Evidence.retain_images
        def retain(evidence):
            self.assertEqual(self.child.kills, 1)
            self.assertEqual(self.child.waits, [2])
            original(evidence)
        with mock.patch.object(capture.time, 'monotonic', side_effect=clock), \
                mock.patch.object(capture.Evidence, 'retain_images', retain):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                self.launch([], backend=backend)
        self.assertEqual(caught.exception.timeout, 180)
        self.assertEqual(self.receipt['native_diagnostics']['dump_status'], 'not_captured_timeout')
        self.assertTrue(self.receipt['native_diagnostics']['executable_retained'])
        self.assertEqual(backend.dumps, [])

    def test_monitor_failure_kills_child_without_relaunch_and_fails_closed(self):
        backend = Backend(self.images, [])
        backend.failure = OSError('debug events unavailable')
        with self.assertRaisesRegex(OSError, 'debug events unavailable'):
            self.launch([], backend=backend)
        self.assertEqual(self.child.kills, 1)
        self.assertEqual(self.child.waits, [2])
        self.assertTrue(self.receipt['evidence_errors'])

    def test_retention_failure_preserves_crash_and_partial_files_are_removed(self):
        original = capture.Evidence.copy
        def fail(evidence, name, source, role, expected_hash=None):
            if role == 'executable':
                raise OSError('image storage denied')
            return original(evidence, name, source, role, expected_hash)
        with mock.patch.object(capture.Evidence, 'copy', fail):
            self.assertEqual(self.launch(self.crash_events()).returncode, CRASH)
        self.assertFalse(self.receipt['native_diagnostics']['executable_retained'])
        self.assertEqual(self.receipt['native_diagnostics']['status'], 'incomplete')

    def test_unique_case_evidence_does_not_reuse_stale_dump(self):
        self.launch(self.crash_events())
        old = self.receipt['native_diagnostics']['directory']
        (Path(old) / 'exception.dmp').write_bytes(b'stale')
        self.launch(self.crash_events())
        current = self.receipt['native_diagnostics']['directory']
        self.assertNotEqual(old, current)
        self.assertEqual((Path(old) / 'exception.dmp').read_bytes(), b'stale')
        capture.validate_dump(Path(current) / 'exception.dmp', CRASH, 17)

    def test_file_module_and_byte_budgets_fail_closed(self):
        for constant in ('MAX_FILES', 'MAX_IMAGE_BYTES'):
            with self.subTest(bound=constant), mock.patch.object(capture, constant, 0):
                self.launch(self.crash_events())
                self.assertFalse(self.receipt['native_diagnostics']['executable_retained'])
        with mock.patch.object(capture, 'MAX_MODULES', 0):
            with self.assertRaisesRegex(OSError, 'module handle bound'):
                self.launch([event(3)])
        self.assertEqual(self.backend.closed, [11])

    def test_loaded_images_take_priority_over_large_optional_symbols(self):
        bound = self.exe.stat().st_size + self.mesa.stat().st_size
        with mock.patch.object(capture, 'MAX_IMAGE_BYTES', bound):
            self.launch(self.crash_events())
        data = self.receipt['native_diagnostics']
        self.assertTrue(data['executable_retained'])
        self.assertEqual({item['role'] for item in data['files']}, {'executable', 'module'})
        self.assertEqual(data['status'], 'incomplete', 'unretained symbols must remain explicit')

    def test_unknown_or_unloaded_module_metadata_is_explicit(self):
        missing = event(6)
        missing.data.load.file = 0
        self.launch([event(3), missing, event(1, code=CRASH), event(5, code=CRASH)])
        self.assertEqual(self.receipt['native_diagnostics']['status'], 'incomplete')
        unload = event(7)
        self.launch([event(3), event(6), unload, event(1, code=CRASH), event(5, code=CRASH)])
        self.assertFalse(any(item['role'] == 'module' for item in self.receipt['native_diagnostics']['files']))
        self.assertEqual([item['base'] for item in self.receipt['native_diagnostics']['modules']],
                         [0x140000000])

    def test_module_churn_releases_handles_before_the_next_load(self):
        events = [event(3)]
        for _ in range(3):
            events.extend((event(6), event(7)))
        # Unknown and repeated unloads have no owned stream to release.
        events.extend((event(7), event(5)))
        backend = Backend(self.images, events)
        resume = backend.resume

        def observe_resume(current, status):
            if current.kind == 7:
                self.assertTrue(all(source.closed for source in backend.streams[1:]))
                self.assertFalse(backend.streams[0].closed)
            resume(current, status)

        backend.resume = observe_resume
        with mock.patch.object(capture, 'MAX_MODULES', 2):
            self.assertEqual(self.launch(events, backend=backend).returncode, 0)
        self.assertEqual(self.receipt['native_diagnostics']['status'], 'normal_exit')
        self.assertEqual(len(backend.streams), 4)

    def test_empty_image_set_never_claims_matching_executable(self):
        self.launch([event(1, code=CRASH), event(5, code=CRASH)])
        self.assertFalse(self.receipt['native_diagnostics']['executable_retained'])
        self.assertEqual(self.receipt['native_diagnostics']['status'], 'incomplete')

    def test_both_runners_publish_crash_receipt_before_runtime_cleanup(self):
        for implementation in (device, runner):
            with self.subTest(runner=implementation.__name__):
                def launch(command, *, receipt, evidence_parent, **kwargs):
                    backend = Backend(self.images, self.crash_events())
                    child = Child()
                    backend.child = child
                    with mock.patch.object(capture.subprocess, 'Popen', return_value=child):
                        return capture.monitored_run(command, receipt=receipt,
                            evidence_parent=evidence_parent, backend=backend, **kwargs)
                receipt_path = self.artifacts / 'case.process.json'
                arguments = dict(cwd=self.runtime, environment={}, log=self.artifacts / 'case.log',
                    artifact=self.runtime / 'capture.json', retained=self.artifacts / 'capture.json',
                    receipt_path=receipt_path)
                if implementation is runner:
                    arguments['timeout_seconds'] = 180
                with mock.patch.object(capture, 'run', side_effect=launch), \
                        mock.patch.object(subprocess, 'run', side_effect=AssertionError('unmonitored launch')):
                    result, errors = implementation._run_process([str(self.exe)], **arguments)
                self.assertEqual(result.returncode, CRASH)
                self.assertEqual(errors, [])
                receipt = json.loads(receipt_path.read_bytes())
                self.assertEqual(receipt['return_code'], CRASH)
                self.assertEqual(receipt['binary_identity_status'], 'verified')
                self.assertTrue(receipt['native_diagnostics']['executable_retained'])
                self.assertTrue(Path(receipt['native_diagnostics']['dump_path']).is_file())

    def test_case_names_are_unique_for_parallel_fixtures(self):
        from concurrent.futures import ThreadPoolExecutor
        roots = [self.runtime / str(index) for index in range(8)]
        for root in roots:
            root.mkdir()
        paths = []
        def process(command, **kwargs):
            paths.append(kwargs['receipt_path'])
            kwargs['log'].write_text('injected crash', encoding='utf-8')
            return subprocess.CompletedProcess(command, CRASH), []
        def run_case(root):
            case = device.RenderRunnerTests('test_traversal_twice')
            case.root, case.environment = root, {}
            with self.assertRaises(AssertionError):
                case.run_client('capture')
        settings = types.SimpleNamespace(client=self.exe, artifacts=self.artifacts)
        with mock.patch.object(device, 'OPTIONS', settings, create=True), \
                mock.patch.object(device, '_run_process', side_effect=process):
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(run_case, roots))
        self.assertEqual(len(paths), 8)
        self.assertEqual(len(set(paths)), 8)

    def test_identity_guard_refusal_retains_actual_crash_code_in_both_runners(self):
        for implementation in (device, runner):
            with self.subTest(runner=implementation.__name__):
                def changed(command, **kwargs):
                    kwargs['receipt']['native_diagnostics'] = dict(status='retained', executable_retained=True)
                    metadata = self.exe.stat()
                    os.utime(self.exe, ns=(metadata.st_atime_ns, metadata.st_mtime_ns + 2000000000))
                    return subprocess.CompletedProcess(command, CRASH)
                receipt_path = self.artifacts / 'changed.process.json'
                arguments = dict(cwd=self.runtime, environment={}, log=self.artifacts / 'changed.log',
                    artifact=self.runtime / 'capture.json', retained=self.artifacts / 'capture.json',
                    receipt_path=receipt_path)
                if implementation is runner:
                    arguments['timeout_seconds'] = 180
                with mock.patch.object(capture, 'run', side_effect=changed), self.assertRaises(OSError):
                    implementation._run_process([str(self.exe)], **arguments)
                receipt = json.loads(receipt_path.read_bytes())
                self.assertEqual(receipt['return_code'], CRASH)
                self.assertEqual(receipt['outcome'], 'returned')
                self.assertEqual(receipt['binary_identity_status'], 'changed')
                self.assertIsNone(receipt['binary_sha256'])
                self.assertFalse(receipt['native_diagnostics']['executable_retained'])
                self.assertEqual(receipt['native_diagnostics']['status'], 'incomplete')

    def test_failed_directory_creation_preserves_native_crash_code(self):
        with mock.patch.object(capture.tempfile, 'mkdtemp', side_effect=OSError('storage denied')):
            self.assertEqual(self.launch(self.crash_events()).returncode, CRASH)
        self.assertEqual(self.receipt['native_diagnostics']['status'], 'incomplete')
        self.assertEqual(self.receipt['native_diagnostics']['dump_status'], 'failed')
        self.assertFalse(self.receipt['native_diagnostics']['executable_retained'])

    def test_partial_input_deadline_and_existing_output_refuse_without_false_identity(self):
        evidence = capture.Evidence(self.receipt, self.artifacts)
        class Partial(io.BytesIO):
            def fileno(inner):
                return source.fileno()
        with self.exe.open('rb') as source:
            with self.assertRaisesRegex(OSError, 'partial diagnostic input'):
                evidence.copy(self.exe, Partial(b'part'), 'executable')
            self.assertEqual(evidence.data['files'], [])
            self.assertEqual(list(evidence.directory.rglob('*.exe')), [])
            evidence.copy_deadline = 0
            with self.assertRaisesRegex(OSError, 'deadline reached'):
                evidence.copy(self.exe, source, 'executable')
            evidence.copy_deadline = float('inf')
            evidence.copy(self.exe, source, 'executable')
            retained = Path(evidence.data['files'][0]['path'])
            original = retained.read_bytes()
            with self.assertRaises(FileExistsError):
                evidence.copy(self.exe, source, 'executable')
            self.assertEqual(retained.read_bytes(), original, 'rollback deleted another completed copy')

    def test_unreadable_symbols_empty_files_and_nonregular_inputs_fail_explicitly(self):
        with mock.patch.object(capture, 'open_symbol', side_effect=PermissionError('symbol denied')):
            self.assertEqual(self.launch(self.crash_events()).returncode, CRASH)
        self.assertEqual(self.receipt['native_diagnostics']['status'], 'incomplete')
        self.assertTrue(self.receipt['native_diagnostics']['executable_retained'])
        empty = self.runtime / 'empty.pdb'
        empty.touch()
        evidence = capture.Evidence(self.receipt, self.artifacts)
        with empty.open('rb') as source, self.assertRaisesRegex(OSError, 'regular file'):
            evidence.copy(empty, source, 'symbol_candidate')
        with self.assertRaisesRegex(OSError, 'regular file'):
            with capture.open_symbol(self.runtime):
                self.fail('directory accepted as symbol')

    def test_symbol_short_or_long_spelling_uses_handle_identity_and_refuses_replacement(self):
        symbol = self.exe.with_suffix('.pdb')
        with capture.open_symbol(symbol) as source:
            self.assertEqual(source.read(), b'available PDB')
        # No symlink privilege is needed to exercise the same identity refusal.
        fstat = capture.os.fstat
        def other_identity(fd):
            value = fstat(fd)
            return types.SimpleNamespace(st_dev=value.st_dev, st_ino=value.st_ino + 1,
                st_mode=value.st_mode, st_nlink=value.st_nlink)
        with mock.patch.object(capture.os, 'fstat', side_effect=other_identity), \
                self.assertRaisesRegex(OSError, 'changed during open'):
            with capture.open_symbol(symbol):
                self.fail('replaced symbol accepted')

    def test_bad_dump_streams_and_missing_stack_context_or_modules_are_rejected(self):
        good = dump_bytes()
        variants = [b'', b'MDMP', b'wrong' + good[5:]]
        for offset, value in ((8, 0xffffffff), (12, 0xffffffff), (32, 99), (36, 0xffffffff),
                              (68, 0), (104, 0), (120, 0), (392, 0)):
            data = bytearray(good)
            struct.pack_into('<I', data, offset, value)
            variants.append(bytes(data))
        target = self.artifacts / 'bad.dmp'
        for index, data in enumerate(variants):
            with self.subTest(input=index):
                target.write_bytes(data)
                with self.assertRaises(OSError):
                    capture.validate_dump(target, CRASH, 17)

    def test_cleanup_failure_does_not_mask_timeout(self):
        backend = Backend(self.images, [])
        backend.detach = mock.Mock(side_effect=OSError('detach denied'))
        clock = iter([0, 181])
        with mock.patch.object(capture.time, 'monotonic', side_effect=lambda: next(clock, 181)):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                self.launch([], backend=backend)
        self.assertEqual(caught.exception.timeout, 180)
        self.assertEqual(self.child.kills, 1)
        self.assertEqual(self.child.waits, [2])
        self.assertTrue(any('cleanup failed' in text for text in self.receipt['evidence_errors']))


class AdapterTests(unittest.TestCase):
    def test_win64_structure_sizes_and_offsets(self):
        self.assertEqual((C.sizeof(capture.ExceptionRecord), C.sizeof(capture.DebugEvent)), (152, 176))
        self.assertEqual(capture.DebugEvent.data.offset, 16)
        self.assertEqual((C.sizeof(capture.UnloadEvent), capture.UnloadEvent.base.offset), (8, 0))
        self.assertEqual((C.sizeof(capture.DumpException), capture.DumpException.pointers.offset), (16, 4))
        self.assertEqual((C.sizeof(capture.CallbackInput), capture.CallbackInput.kind.offset), (16, 12))
        self.assertEqual((C.sizeof(capture.IoCallback), capture.IoCallback.buffer.offset), (28, 16))

    def test_dump_io_callbacks_enforce_bound_deadline_and_partial_write_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'exception.dmp'
            adapter = capture.WindowsDebugger.__new__(capture.WindowsDebugger)
            clock = [0]
            requests = []
            def writer(process, pid, file, flags, exception, user, info):
                self.assertEqual(flags, 0x1020, 'full memory must never be enabled')
                self.assertIsNone(file, 'native writes must not bypass the dump byte bound')
                callback_pointer = C.cast(info, C.POINTER(C.c_void_p))[0]
                callback = C.CFUNCTYPE(C.c_int32, C.c_void_p, C.c_void_p, C.c_void_p)(callback_pointer)
                outgoing = (C.c_int32 * 4)()
                start = capture.CallbackInput(pid=42, process=None, kind=11)
                self.assertTrue(callback(None, C.addressof(start), C.addressof(outgoing)))
                self.assertEqual(outgoing[0], 1)
                payload = C.create_string_buffer(b'bytes')
                for offset, size in requests:
                    incoming = C.create_string_buffer(C.sizeof(capture.CallbackInput) + C.sizeof(capture.IoCallback))
                    capture.CallbackInput.from_buffer(incoming).kind = 12
                    data = capture.IoCallback.from_buffer(incoming, C.sizeof(capture.CallbackInput))
                    data.offset, data.size, data.buffer = offset, size, C.addressof(payload)
                    callback(None, C.addressof(incoming), C.addressof(outgoing))
                return True
            adapter.write_dump = writer
            def write_target():
                with target.open('x+b') as output:
                    adapter._dump_with_bounded_io(types.SimpleNamespace(_handle=1), 42, output,
                                                  capture.DumpException(), 10)
            with mock.patch.object(C, 'WINFUNCTYPE', C.CFUNCTYPE, create=True), \
                    mock.patch.dict(sys.modules, msvcrt=types.SimpleNamespace(get_osfhandle=lambda fd: fd)), \
                    mock.patch.object(capture.time, 'monotonic', side_effect=lambda: clock[0]):
                requests[:] = [(0, 5)]
                write_target()
                self.assertEqual(target.read_bytes(), b'bytes')
                target.unlink()
                with mock.patch.object(adapter, 'write_dump', return_value=True), \
                        self.assertRaisesRegex(OSError, 'did not enable bounded callback I/O'):
                    write_target()
                target.unlink()
                requests[:] = [(0, 5)]
                write_target()
                for request, now in (((capture.MAX_DUMP_BYTES, 5), 0), ((0, 5), 10)):
                    target.unlink()
                    requests[:] = [request]
                    clock[0] = now
                    with self.assertRaises(OSError):
                        write_target()
                    self.assertEqual(target.stat().st_size, 0)

    def test_failed_context_and_event_apis_raise_and_close_owned_thread_handle(self):
        adapter = capture.WindowsDebugger.__new__(capture.WindowsDebugger)
        adapter.open_thread = mock.Mock(return_value=9)
        adapter.thread_context = mock.Mock(return_value=False)
        adapter.close = mock.Mock()
        adapter.wait_event = mock.Mock(return_value=False)
        adapter.continue_event = mock.Mock(return_value=False)
        with mock.patch.object(C, 'WinError', side_effect=lambda code: OSError(str(code)), create=True), \
                mock.patch.object(C, 'get_last_error', return_value=5, create=True) as last_error:
            with self.assertRaises(OSError):
                adapter.dump(Child(), event(1, code=CRASH), Path('unused.dmp'), 180)
            adapter.close.assert_called_once_with(9)
            with self.assertRaises(OSError):
                adapter.wait(50)
            with self.assertRaises(OSError):
                adapter.resume(event(1, code=CRASH), 0x80010001)
            last_error.return_value = 121
            self.assertIsNone(adapter.wait(50))

    def test_missing_native_api_fails_before_spawn_without_fallback(self):
        with mock.patch.object(capture.os, 'name', 'nt'), \
                mock.patch.object(capture, 'WindowsDebugger', side_effect=OSError('API unavailable')), \
                mock.patch.object(capture.subprocess, 'Popen') as spawn, \
                mock.patch.object(capture.subprocess, 'run') as ordinary:
            with self.assertRaisesRegex(OSError, 'API unavailable'):
                capture.run(['client.exe'], receipt={}, evidence_parent='evidence')
            spawn.assert_not_called()
            ordinary.assert_not_called()

    def test_windows_launch_uses_one_scoped_monitor_and_preserves_deadline(self):
        receipt = {}
        backend = object()
        with mock.patch.object(capture.os, 'name', 'nt'), \
                mock.patch.object(capture, 'WindowsDebugger', return_value=backend) as create, \
                mock.patch.object(capture, 'monitored_run', return_value='result') as monitor, \
                mock.patch.object(capture.subprocess, 'run') as ordinary:
            self.assertEqual(capture.run(['client.exe'], receipt=receipt, evidence_parent='evidence',
                expected_exit=2, timeout=180, check=False, cwd='runtime'), 'result')
            create.assert_called_once_with()
            monitor.assert_called_once_with(['client.exe'], receipt=receipt, evidence_parent='evidence',
                backend=backend, expected_exit=2, timeout=180, check=False, cwd='runtime')
            ordinary.assert_not_called()

    def test_posix_launch_preserves_subprocess_arguments_without_native_collection(self):
        with mock.patch.object(capture.os, 'name', 'posix'), \
                mock.patch.object(capture, 'WindowsDebugger') as backend, \
                mock.patch.object(capture.subprocess, 'run', return_value='result') as ordinary:
            self.assertEqual(capture.run(['client'], receipt={}, evidence_parent='evidence',
                timeout=180, check=False, cwd='runtime'), 'result')
            ordinary.assert_called_once_with(['client'], timeout=180, check=False, cwd='runtime')
            backend.assert_not_called()


class WiringTests(unittest.TestCase):
    def test_ci_registers_suite_and_uploads_both_windows_lanes_with_symbols(self):
        root = Path(__file__).resolve().parents[2]
        cmake = (root / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('NAME RenderCrashCapturePython', cmake)
        self.assertIn('tools/perf/test_render_crash_capture.py', cmake)
        inventory = (root / 'tools/ci/ci_paths.py').read_text(encoding='utf-8')
        self.assertIn('"tools/perf/test_render_crash_capture.py"', inventory)
        workflow = (root / '.github/workflows/windows-build.yml').read_text(encoding='utf-8')
        self.assertIn('-DCMAKE_CXX_FLAGS=/Z7', workflow)
        self.assertIn('-DCMAKE_EXE_LINKER_FLAGS=/DEBUG /OPT:REF /OPT:ICF', workflow)
        self.assertIn('-DCMAKE_CXX_FLAGS=-g', workflow)
        for lane in ('windows-msvc', 'windows-ucrt64'):
            upload = workflow.split('name: ' + lane + '-test-evidence-', 1)[1].split('retention-days:', 1)[0]
            self.assertIn('build/release/test-artifacts', upload)

    def test_native_smoke_is_registered_only_in_the_windows_native_lane(self):
        root = Path(__file__).resolve().parents[2]
        cmake = (root / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        block = cmake.split('if(WIN32)\n    # Exercise real debug events', 1)[1].split('endif()', 1)[0]
        self.assertIn('RenderCrashCaptureWindows', block)
        self.assertIn('--native-windows-smoke', block)
        self.assertIn('--smoke-artifacts ${CMAKE_BINARY_DIR}/test-artifacts/render-debugger', block)
        self.assertIn('WindowsSmokeTests.${smoke_case}', block)
        for case in ('test_success_and_expected_refusal', 'test_unhandled_access_violation'):
            self.assertIn(case, block)
        self.assertIn('PROPERTIES TIMEOUT 30', block)


if options.native_windows_smoke:
    class WindowsSmokeTests(unittest.TestCase):
        """Real Win32 events and bounded DbgHelp I/O; never selected by POSIX CI."""

        def launch(self, program, expected_exit=0):
            options.smoke_artifacts.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix='debugger-smoke-', dir=options.smoke_artifacts))
            receipt = dict(binary_path=str(Path(sys.executable).resolve()), evidence_errors=[])
            try:
                with runner.verified_binary(receipt), (directory / 'child.log').open('wb') as output:
                    result = capture.run([receipt['binary_path'], '-I', '-B', '-c', program],
                        cwd=directory, env=os.environ.copy(), stdout=output, stderr=subprocess.STDOUT,
                        timeout=8, check=False, receipt=receipt, evidence_parent=directory,
                        expected_exit=expected_exit)
            finally:
                # Keep the smoke's own logs/receipts/dump for both existing
                # Windows evidence uploads, including a failing smoke assertion.
                (directory / 'process.json').write_text(json.dumps(receipt, indent=2) + '\n',
                                                       encoding='utf-8')
            self.assertEqual(receipt['evidence_errors'], [])
            self.assertEqual(receipt['binary_identity_status'], 'verified')
            return result, receipt, directory

        def test_success_and_expected_refusal(self):
            # Two tiny children, each bounded to eight seconds; no rendering,
            # host configuration, module retention or dump on an expected exit.
            for code in (0, 2):
                with self.subTest(exit_code=code):
                    result, receipt, directory = self.launch(f'import os; os._exit({code})', code)
                    self.assertEqual(result.returncode, code)
                    data = receipt['native_diagnostics']
                    self.assertEqual(data['status'], 'normal_exit')
                    self.assertIsNone(data['directory'])
                    self.assertEqual(data['files'], [])
                    self.assertEqual(sorted(path.name for path in directory.iterdir()),
                                     ['child.log', 'process.json'])

        def test_unhandled_access_violation(self):
            # ctypes catches SEH on the calling Python thread. Call the CRT's
            # strlen on address 1 from a native thread instead. The valid DLL
            # entry also works with control-flow protection; no test DLL needed.
            program = '''import ctypes as C
k = C.WinDLL('kernel32', use_last_error=True, winmode=0x800)
k_crt = C.WinDLL('ucrtbase', use_last_error=True, winmode=0x800)
k.SetErrorMode(2)
k.CreateThread.restype = C.c_void_p
k.CreateThread.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, C.c_uint32, C.c_void_p]
k.WaitForSingleObject.argtypes = [C.c_void_p, C.c_uint32]
thread = k.CreateThread(None, 0, C.cast(k_crt.strlen, C.c_void_p), C.c_void_p(1), 0, None)
if not thread:
    raise C.WinError(C.get_last_error())
k.WaitForSingleObject(thread, 5000)
raise RuntimeError('native fault did not terminate the child')
'''
            result, receipt, _directory = self.launch(program)
            self.assertEqual(result.returncode, CRASH)
            data = receipt['native_diagnostics']
            self.assertEqual(data['status'], 'retained')
            self.assertEqual(data['dump_status'], 'retained')
            self.assertEqual(data['exception_code'], CRASH)
            self.assertEqual(data['access_type'], 'read')
            # Optimized strlen can align its first read within the unmapped
            # first page. It must still fault on the supplied invalid pointer.
            self.assertGreaterEqual(data['accessed_address'], 0)
            self.assertLess(data['accessed_address'], 4096)
            self.assertGreater(data['registers']['rip'], 0)
            self.assertEqual(data['registers']['rip'], data['exception_address'])
            self.assertTrue(data['executable_retained'])
            capture.validate_dump(Path(data['dump_path']), CRASH, data['thread_id'])
            self.assertLessEqual(Path(data['dump_path']).stat().st_size, capture.MAX_DUMP_BYTES)
            images = [item for item in data['files'] if item['role'] == 'executable']
            self.assertEqual(len(images), 1)
            self.assertEqual(images[0]['sha256'], receipt['binary_sha256'])
            self.assertEqual(runner.file_sha256(images[0]['path']), receipt['binary_sha256'])
            self.assertLessEqual(len(data['files']), capture.MAX_FILES)
            self.assertLessEqual(sum(item['bytes'] for item in data['files']), capture.MAX_IMAGE_BYTES)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
