"""Process-owned project lock for the service's single-platform session profile."""
import ctypes
from ctypes import wintypes
import hashlib
import os


class ProjectLock:
    def __init__(self, project, lockfile):
        self.closed = False
        if os.name == 'nt':
            self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            self.kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
            self.kernel.CreateMutexW.restype = wintypes.HANDLE
            self.kernel.ReleaseMutex.argtypes = [wintypes.HANDLE]
            self.kernel.ReleaseMutex.restype = wintypes.BOOL
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel.CloseHandle.restype = wintypes.BOOL
            # Windows byte-range locks are unsupported by some WSL UNC shares.
            # A kernel mutex survives filesystem variations and dies with its owner.
            identity = hashlib.sha256(os.path.normcase(str(project.resolve())).encode('utf-8')).hexdigest()
            self.handle = self.kernel.CreateMutexW(None, True, 'Local\\LuminumbraAuthorBuild_' + identity)
            error = ctypes.get_last_error()
            if not self.handle:
                raise OSError(error, 'Cannot create the authoring project mutex')
            if error == 183:  # ERROR_ALREADY_EXISTS
                self.kernel.CloseHandle(self.handle)
                raise BlockingIOError('A Windows authoring process already owns this project')
        else:
            import fcntl
            self.stream = lockfile.open('a+b')
            try:
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                self.stream.close()
                raise

    def close(self):
        if not self.closed:
            if os.name == 'nt':
                self.kernel.ReleaseMutex(self.handle)
                self.kernel.CloseHandle(self.handle)
            else:
                self.stream.close()
            self.closed = True
