"""Windows process/ACL checks using real descendants; no renderer acceptance."""
import ctypes
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest import mock
import unittest

from test_client import client_module, running, state, wait_for, wire


class OwnedProcessCompletion(unittest.TestCase):
    def test_job_drain_precedes_handle_release_including_failure_paths(self):
        module = sys.modules[client_module.OwnedProcess.__module__]
        for failure in (None, 'terminate', 'query', 'deadline', 'root'):
            with self.subTest(failure=failure):
                calls = []
                active = iter((2, 1, 0))
                def terminate(job, code):
                    calls.append(('terminate', job))
                    if failure == 'terminate': raise OSError('terminate failed')
                    return True
                def query(job, kind, pointer, size, needed):
                    calls.append(('query', job))
                    if failure == 'query': raise OSError('query failed')
                    pointer._obj.active = 1 if failure == 'deadline' else next(active)
                    return True
                def close(handle): calls.append(('close', handle))
                api = SimpleNamespace(check=lambda value: value, kernel=SimpleNamespace(
                    TerminateJobObject=terminate, QueryInformationJobObject=query,
                    CloseHandle=close, WaitForSingleObject=lambda *args: 258 if failure == 'root' else 0))
                owner = module.OwnedProcess.__new__(module.OwnedProcess)
                owner.child, owner.job, owner.handle, owner.api, owner.closed = None, 11, 22, api, False
                values = iter((0., 0., 2.))
                sample = (lambda: next(values)) if failure == 'deadline' else (lambda: 0.)
                with mock.patch.object(module, 'time', SimpleNamespace(monotonic=sample)), \
                        mock.patch.object(module.threading, 'Event'):
                    if failure:
                        with self.assertRaises((OSError, TimeoutError)):
                            owner.close()
                    else:
                        owner.close()
                self.assertEqual(calls[0], ('terminate', 11))
                self.assertEqual(calls[-2:], [('close', 11), ('close', 22)])
                if failure is None:
                    self.assertEqual(calls.count(('query', 11)), 3)
                self.assertTrue(owner.closed)
                self.assertIsNone(owner.job)
                self.assertIsNone(owner.handle)
                before = calls.copy()
                owner.close()
                self.assertEqual(calls, before)

    def test_empty_job_and_absent_handles_close_without_waiting(self):
        module = sys.modules[client_module.OwnedProcess.__module__]
        for job, handle in ((None, None), (None, 22), (11, None), (11, 22)):
            with self.subTest(job=job, handle=handle):
                owner = module.OwnedProcess.__new__(module.OwnedProcess)
                owner.child, owner.job, owner.handle, owner.closed = None, job, handle, False
                owner.api = mock.Mock()
                def empty(job, kind, pointer, size, needed):
                    pointer._obj.active = 0
                    return True
                owner.api.kernel.QueryInformationJobObject.side_effect = empty
                owner.api.kernel.WaitForSingleObject.return_value = 0
                owner.close()
                self.assertEqual(owner.api.kernel.TerminateJobObject.call_count, int(job is not None))
                self.assertEqual(owner.api.kernel.QueryInformationJobObject.call_count, int(job is not None))
                self.assertEqual(owner.api.kernel.WaitForSingleObject.call_count, int(handle is not None))
                self.assertTrue(owner.closed)


@unittest.skipUnless(os.name == 'nt', 'Windows Job Object and protected DACL qualification')
class WindowsProcessTests(unittest.TestCase):
    def test_real_reader_contention_preserves_atomic_complete_mailbox_records(self):
        with tempfile.TemporaryDirectory(prefix='viewport-sharing-test-') as directory:
            root = Path(directory)
            key = bytes(range(32))
            first = {'kind': 'state', 'session': 'a'*32, 'sequence': 1, 'state': state(1)}
            second = {'kind': 'state', 'session': 'a'*32, 'sequence': 2, 'state': state(2)}
            records = (wire.encode(first, b'', key), wire.encode(second, b'', key))
            for name, before, after in (
                    ('desired.bin', records[0], records[1]),
                    ('frame-0.json', b'{"sequence":1,"length":128}', b'{"sequence":2,"length":128}')):
                with self.subTest(mailbox=name):
                    path = root / name
                    wire.atomic_write(path, before)
                    # Negative control: real CRT handle, no mocked Windows API.
                    with path.open('rb') as legacy:
                        with self.assertRaises(OSError) as denied:
                            wire.atomic_write(path, after)
                        self.assertIn(denied.exception.winerror, (5, 32))
                        self.assertEqual(legacy.read(), before)
                    with wire.open_record_reader(path) as reader:
                        wire.atomic_write(path, after)
                        self.assertEqual(reader.read(), before)
                        self.assertEqual(wire.read_record_file(path, wire.CAPACITY), after)
                    self.assertFalse(list(root.glob('.pending-*')))
            self.assertEqual(wire.read_record(io.BytesIO(wire.read_record_file(root/'desired.bin', wire.CAPACITY)), key),
                             (second, b''))

    def test_private_directory_has_protected_current_user_only_dacl(self):
        from ctypes import wintypes as w
        process_module = sys.modules[client_module.__package__ + '.viewport_process']
        api = process_module._Windows()
        a, k = api.advapi, api.kernel
        api.bind(a, 'GetNamedSecurityInfoW', w.DWORD, [w.LPWSTR, ctypes.c_int, w.DWORD,
            w.LPVOID, w.LPVOID, w.LPVOID, w.LPVOID, ctypes.POINTER(w.LPVOID)])
        api.bind(a, 'ConvertSecurityDescriptorToStringSecurityDescriptorW', w.BOOL,
            [w.LPVOID, w.DWORD, w.DWORD, ctypes.POINTER(w.LPWSTR), w.LPVOID])
        api.bind(a, 'ConvertStringSecurityDescriptorToSecurityDescriptorW', w.BOOL,
            [w.LPWSTR, w.DWORD, ctypes.POINTER(w.LPVOID), w.LPVOID])
        root = process_module.private_directory()
        token, sid, descriptor, rendered = w.HANDLE(), w.LPWSTR(), w.LPVOID(), w.LPWSTR()
        try:
            api.check(a.OpenProcessToken(k.GetCurrentProcess(), 8, ctypes.byref(token)))
            needed = w.DWORD()
            a.GetTokenInformation(token, 1, None, 0, ctypes.byref(needed))
            information = ctypes.create_string_buffer(needed.value)
            api.check(a.GetTokenInformation(token, 1, information, needed.value, ctypes.byref(needed)))
            api.check(a.ConvertSidToStringSidW(ctypes.cast(information, ctypes.POINTER(w.LPVOID))[0],
                                              ctypes.byref(sid)))
            self.assertEqual(a.GetNamedSecurityInfoW(str(root), 1, 4, None, None, None, None,
                                                     ctypes.byref(descriptor)), 0)
            api.check(a.ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, 1, 4,
                                                                              ctypes.byref(rendered), None))
            # Canonicalize the expectation through the same Windows rendering: SDDL prints well-known
            # accounts as aliases (the hosted runner is the built-in Administrator, rendered as LA),
            # so compare against the expected descriptor parsed from the current user's SID string.
            expected_descriptor, expected = w.LPVOID(), w.LPWSTR()
            try:
                api.check(a.ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    'D:P(A;OICI;FA;;;' + sid.value + ')', 1, ctypes.byref(expected_descriptor), None))
                api.check(a.ConvertSecurityDescriptorToStringSecurityDescriptorW(
                    expected_descriptor, 1, 4, ctypes.byref(expected), None))
                self.assertEqual(rendered.value, expected.value)
            finally:
                for allocation in (expected_descriptor, expected):
                    if allocation:
                        k.LocalFree(ctypes.cast(allocation, w.LPVOID))
        finally:
            for allocation in (sid, descriptor, rendered):
                if allocation:
                    k.LocalFree(ctypes.cast(allocation, w.LPVOID))
            if token:
                k.CloseHandle(token)
            root.rmdir()

    def test_abrupt_owner_death_kills_broker_and_descendants(self):
        with tempfile.TemporaryDirectory(prefix='viewport-job-test-') as temporary:
            root = Path(temporary)
            process_module = Path(client_module.__file__).with_name('viewport_process.py')
            child = root / 'child.py'
            child.write_text('''import json,os,subprocess,sys,time
from pathlib import Path
descendant=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'],
    stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,close_fds=True)
witness=Path(sys.argv[1])
temporary=witness.with_suffix('.publishing')
temporary.write_text(json.dumps({'broker':os.getpid(),'descendant':descendant.pid}))
os.replace(temporary,witness)
time.sleep(60)
''')
            owner_script = root / 'owner.py'
            owner_script.write_text('''import importlib.util,os,sys,time
from pathlib import Path
spec=importlib.util.spec_from_file_location('owned',sys.argv[1])
owned=importlib.util.module_from_spec(spec);spec.loader.exec_module(owned)
process=owned.OwnedProcess([sys.executable,sys.argv[2],sys.argv[3]],os.environ.copy(),Path(sys.argv[3]).parent)
time.sleep(60)
''')
            receipt = root / 'pids.json'
            owner = subprocess.Popen([sys.executable, '-B', str(owner_script), str(process_module),
                str(child), str(receipt)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, close_fds=True)
            try:
                wait_for(receipt.exists)
                pids = json.loads(receipt.read_text())
                self.assertTrue(all(running(pid) for pid in pids.values()))
                owner.kill()  # No cleanup callback: only the OS-owned job remains.
                owner.wait(timeout=3)
                wait_for(lambda: all(not running(pid) for pid in pids.values()), timeout=3)
            finally:
                try:
                    if owner.poll() is None:
                        owner.kill()
                    owner.wait(timeout=3)
                finally:
                    owner._handle.Close()


if __name__ == '__main__':
    unittest.main()
