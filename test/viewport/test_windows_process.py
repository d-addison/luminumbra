"""Windows process/ACL checks using real descendants; no renderer acceptance."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from test_client import client_module, running, wait_for


@unittest.skipUnless(os.name == 'nt', 'Windows Job Object and protected DACL qualification')
class WindowsProcessTests(unittest.TestCase):
    def test_private_directory_has_protected_current_user_only_dacl(self):
        from ctypes import wintypes as w
        process_module = sys.modules[client_module.__package__ + '.viewport_process']
        api = process_module._Windows()
        a, k = api.advapi, api.kernel
        api.bind(a, 'GetNamedSecurityInfoW', w.DWORD, [w.LPWSTR, ctypes.c_int, w.DWORD,
            w.LPVOID, w.LPVOID, w.LPVOID, w.LPVOID, ctypes.POINTER(w.LPVOID)])
        api.bind(a, 'ConvertSecurityDescriptorToStringSecurityDescriptorW', w.BOOL,
            [w.LPVOID, w.DWORD, w.DWORD, ctypes.POINTER(w.LPWSTR), w.LPVOID])
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
            self.assertEqual(rendered.value, 'D:P(A;OICI;FA;;;' + sid.value + ')')
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
Path(sys.argv[1]).write_text(json.dumps({'broker':os.getpid(),'descendant':descendant.pid}))
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
                if owner.poll() is None:
                    owner.kill()
                owner.wait(timeout=3)


if __name__ == '__main__':
    unittest.main()
