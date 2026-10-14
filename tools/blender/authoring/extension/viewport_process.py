"""Private session directories and owned broker process trees. No Blender API."""
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import uuid


class _Windows:
    """Bindings are initialized only on Windows; every handle has an owner."""
    def __init__(self):
        from ctypes import wintypes as w
        self.w = w
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.advapi = ctypes.WinDLL('advapi32', use_last_error=True)
        self.bind(self.kernel, 'CloseHandle', w.BOOL, [w.HANDLE])
        self.bind(self.kernel, 'GetCurrentProcess', w.HANDLE, [])
        self.bind(self.kernel, 'LocalFree', w.HLOCAL, [w.HLOCAL])
        self.bind(self.advapi, 'OpenProcessToken', w.BOOL, [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)])
        self.bind(self.advapi, 'GetTokenInformation', w.BOOL,
                  [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD)])
        self.bind(self.advapi, 'ConvertSidToStringSidW', w.BOOL, [w.LPVOID, ctypes.POINTER(w.LPWSTR)])
        self.bind(self.advapi, 'ConvertStringSecurityDescriptorToSecurityDescriptorW', w.BOOL,
                  [w.LPCWSTR, w.DWORD, ctypes.POINTER(w.LPVOID), ctypes.POINTER(w.DWORD)])
        class Security(ctypes.Structure):
            _fields_ = [('length', w.DWORD), ('descriptor', w.LPVOID), ('inherit', w.BOOL)]
        self.Security = Security
        self.bind(self.kernel, 'CreateDirectoryW', w.BOOL, [w.LPCWSTR, ctypes.POINTER(Security)])

    @staticmethod
    def bind(library, name, result, arguments):
        function = getattr(library, name)
        function.restype, function.argtypes = result, arguments
        return function

    @staticmethod
    def check(value):
        if not value:
            raise ctypes.WinError(ctypes.get_last_error())
        return value

    def private_directory(self):
        w, k, a = self.w, self.kernel, self.advapi
        token, sid, descriptor = w.HANDLE(), w.LPWSTR(), w.LPVOID()
        try:
            self.check(a.OpenProcessToken(k.GetCurrentProcess(), 0x0008, ctypes.byref(token)))
            needed = w.DWORD()
            a.GetTokenInformation(token, 1, None, 0, ctypes.byref(needed))
            information = ctypes.create_string_buffer(needed.value)
            self.check(a.GetTokenInformation(token, 1, information, needed.value, ctypes.byref(needed)))
            user_sid = ctypes.cast(information, ctypes.POINTER(w.LPVOID))[0]
            self.check(a.ConvertSidToStringSidW(user_sid, ctypes.byref(sid)))
            # Protected DACL, one current-user ACE, inherited by files/subdirs.
            self.check(a.ConvertStringSecurityDescriptorToSecurityDescriptorW(
                'D:P(A;OICI;FA;;;' + sid.value + ')', 1, ctypes.byref(descriptor), None))
            security = self.Security(ctypes.sizeof(self.Security), descriptor, False)
            for _ in range(10):
                root = Path(tempfile.gettempdir()) / ('luminumbra-viewport-' + uuid.uuid4().hex)
                if k.CreateDirectoryW(str(root), ctypes.byref(security)):
                    return root
                if ctypes.get_last_error() != 183:
                    self.check(False)
            raise FileExistsError('Unable to allocate private viewport session')
        finally:
            if descriptor:
                k.LocalFree(descriptor)
            if sid:
                k.LocalFree(ctypes.cast(sid, w.LPVOID))
            if token:
                k.CloseHandle(token)


def private_directory():
    if os.name == 'nt':
        return _Windows().private_directory()
    root = Path(tempfile.mkdtemp(prefix='luminumbra-viewport-'))
    os.chmod(root, 0o700)
    return root


class OwnedProcess:
    """Close always terminates descendants, including after broker failure."""
    def __init__(self, command, env, cwd):
        self.child = self.job = self.handle = None
        self.closed = False
        if os.name != 'nt':
            self.child = subprocess.Popen(command, env=env, cwd=cwd, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True, start_new_session=True)
            self.pid = self.child.pid
            return
        api = self.api = _Windows()
        w, k = api.w, api.kernel
        class Basic(ctypes.Structure):
            _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                ('flags', w.DWORD), ('minimum_ws', ctypes.c_size_t), ('maximum_ws', ctypes.c_size_t),
                ('process_limit', w.DWORD), ('affinity', ctypes.c_size_t),
                ('priority', w.DWORD), ('scheduling', w.DWORD)]
        class Extended(ctypes.Structure):
            _fields_ = [('basic', Basic), ('io_counters', ctypes.c_uint64 * 6),
                ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                ('peak_process_memory', ctypes.c_size_t), ('peak_job_memory', ctypes.c_size_t)]
        class Startup(ctypes.Structure):
            _fields_ = [('cb', w.DWORD), ('reserved', w.LPWSTR), ('desktop', w.LPWSTR), ('title', w.LPWSTR),
                ('x', w.DWORD), ('y', w.DWORD), ('xsize', w.DWORD), ('ysize', w.DWORD),
                ('xchars', w.DWORD), ('ychars', w.DWORD), ('fill', w.DWORD), ('flags', w.DWORD),
                ('show', w.WORD), ('reserved_size', w.WORD), ('reserved_data', w.LPVOID),
                ('stdin', w.HANDLE), ('stdout', w.HANDLE), ('stderr', w.HANDLE)]
        class StartupEx(ctypes.Structure):
            _fields_ = [('startup', Startup), ('attributes', w.LPVOID)]
        class ProcessInfo(ctypes.Structure):
            _fields_ = [('process', w.HANDLE), ('thread', w.HANDLE), ('pid', w.DWORD), ('tid', w.DWORD)]
        api.bind(k, 'CreateJobObjectW', w.HANDLE, [w.LPVOID, w.LPCWSTR])
        api.bind(k, 'SetInformationJobObject', w.BOOL, [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD])
        api.bind(k, 'InitializeProcThreadAttributeList', w.BOOL,
                 [w.LPVOID, w.DWORD, w.DWORD, ctypes.POINTER(ctypes.c_size_t)])
        api.bind(k, 'UpdateProcThreadAttribute', w.BOOL,
                 [w.LPVOID, w.DWORD, ctypes.c_size_t, w.LPVOID, ctypes.c_size_t, w.LPVOID, w.LPVOID])
        api.bind(k, 'DeleteProcThreadAttributeList', None, [w.LPVOID])
        api.bind(k, 'CreateProcessW', w.BOOL, [w.LPCWSTR, w.LPWSTR, w.LPVOID, w.LPVOID, w.BOOL,
                 w.DWORD, w.LPVOID, w.LPCWSTR, ctypes.POINTER(StartupEx), ctypes.POINTER(ProcessInfo)])
        api.bind(k, 'GetExitCodeProcess', w.BOOL, [w.HANDLE, ctypes.POINTER(w.DWORD)])
        api.bind(k, 'WaitForSingleObject', w.DWORD, [w.HANDLE, w.DWORD])
        info, attributes, attributes_ready = ProcessInfo(), None, False
        try:
            self.job = api.check(k.CreateJobObjectW(None, None))
            limits = Extended()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, no breakaway.
            api.check(k.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)))
            size = ctypes.c_size_t()
            k.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
            storage = ctypes.create_string_buffer(size.value)
            attributes = ctypes.cast(storage, w.LPVOID)
            api.check(k.InitializeProcThreadAttributeList(attributes, 1, 0, ctypes.byref(size)))
            attributes_ready = True
            jobs = (w.HANDLE * 1)(self.job)
            # Windows 10+ creates directly in the job, avoiding even the
            # suspended-create / assign orphan window. No inherited job handle.
            api.check(k.UpdateProcThreadAttribute(attributes, 0, 0x0002000D,
                jobs, ctypes.sizeof(jobs), None, None))
            startup = StartupEx()
            startup.startup.cb, startup.attributes = ctypes.sizeof(startup), attributes
            command_line = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(x) for x in command]))
            environment = ctypes.create_unicode_buffer('\0'.join(
                key + '=' + value for key, value in sorted(env.items(), key=lambda x: x[0].upper())) + '\0\0')
            api.check(k.CreateProcessW(str(command[0]), command_line, None, None, False,
                0x08000000 | 0x00080000 | 0x00000400, environment, str(cwd), ctypes.byref(startup), ctypes.byref(info)))
            self.handle, self.pid = info.process, info.pid
        except BaseException:
            if self.job:
                k.CloseHandle(self.job)
                self.job = None
            if info.process:
                k.WaitForSingleObject(info.process, 2000)
                k.CloseHandle(info.process)
            raise
        finally:
            if info.thread:
                k.CloseHandle(info.thread)
            if attributes_ready:
                k.DeleteProcThreadAttributeList(attributes)

    def poll(self):
        if self.child is not None:
            return self.child.poll()
        if self.api.kernel.WaitForSingleObject(self.handle, 0) == 258:
            return None
        value = self.api.w.DWORD()
        self.api.check(self.api.kernel.GetExitCodeProcess(self.handle, ctypes.byref(value)))
        return value.value

    def close(self):
        if self.closed:
            return
        if self.child is not None:
            # Send to our group even if the direct child already exited.
            for signum in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(self.pid, signum)
                except ProcessLookupError:
                    pass
                if signum == signal.SIGTERM:
                    time.sleep(0.02)
            self.child.wait(timeout=2)
            self.closed = True
            return
        if self.job:
            self.api.kernel.CloseHandle(self.job)
            self.job = None
        if self.handle:
            result = self.api.kernel.WaitForSingleObject(self.handle, 2000)
            self.api.kernel.CloseHandle(self.handle)
            self.handle = None
            if result != 0:
                raise TimeoutError('Owned Windows broker did not terminate')
        self.closed = True
