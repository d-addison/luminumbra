"""Handle-owned project lock for the service's single-platform session profile."""
import ctypes
from ctypes import wintypes
import errno
import hashlib
import os
import threading


class WindowsMigrationLock:
    """Keep the legacy mutex name present without thread-owned mutex ownership."""
    def __init__(self, project):
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
        self.kernel.CreateMutexW.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        identity = hashlib.sha256(os.path.normcase(str(project.resolve())).encode('utf-8')).hexdigest()
        self.name = 'Local\\LuminumbraAuthorBuild_' + identity
        self.handle = None

    def acquire(self):
        # Older services refuse any existing name. Do the same, but request no
        # ownership so CloseHandle can release the marker from any thread.
        handle = self.kernel.CreateMutexW(None, False, self.name)
        error = ctypes.get_last_error()
        if not handle:
            raise OSError(error, 'Cannot create the authoring project mutex')
        self.handle = handle
        if error == 183:  # ERROR_ALREADY_EXISTS
            self.close()
            raise BlockingIOError(errno.EAGAIN, 'Another authoring process owns this project')

    def close(self):
        if self.handle is not None:
            if not self.kernel.CloseHandle(self.handle):
                raise OSError(ctypes.get_last_error(), 'Cannot close the authoring project mutex')
            self.handle = None


class WindowsFileLock:
    def __init__(self, project):
        self.migration = WindowsMigrationLock(project)
        self.file_released = False

    def acquire(self, stream):
        import msvcrt
        # The same file coordinates all logon sessions with project access.
        # Lock byte zero, even in an empty file; never use the retrying LK_LOCK.
        stream.seek(0)
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            if error.errno == errno.EACCES:
                raise BlockingIOError(errno.EAGAIN, 'Another authoring process owns this project') from error
            raise
        self.file_released = False
        try:
            self.migration.acquire()
        except OSError:
            self.migration.close()
            raise

    def release(self, stream):
        import msvcrt
        if not self.file_released:
            stream.seek(0)
            # Keep the legacy marker until the file lock has been released.
            # Both handles can be released on a different thread.
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            self.file_released = True
        self.migration.close()


class PosixFileLock:
    def acquire(self, stream):
        import fcntl
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)

    def release(self, stream):
        # Preserve flock's existing release-on-close behavior.
        pass


class ProjectLock:
    def __init__(self, project, lockfile, *, backend=None):
        self.closed = False
        self.mutex = threading.Lock()
        self.released = False
        self.backend = backend if backend is not None else (
            WindowsFileLock(project) if os.name == 'nt' else PosixFileLock())
        self.stream = lockfile.open('a+b')
        try:
            self.backend.acquire(self.stream)
        except OSError:
            self.stream.close()
            raise

    def close(self):
        with self.mutex:
            if self.closed:
                return
            if not self.released:
                self.backend.release(self.stream)
                self.released = True
            try:
                self.stream.close()
            finally:
                self.closed = self.stream.closed
