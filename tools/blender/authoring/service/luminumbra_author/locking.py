"""Handle-owned project lock for the service's single-platform session profile."""
import errno
import os
import threading


class WindowsFileLock:
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

    def release(self, stream):
        import msvcrt
        stream.seek(0)
        # Unlocking uses the file handle and is safe on a different thread.
        # An OSError leaves the handle available for an explicit close retry.
        msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


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
            WindowsFileLock() if os.name == 'nt' else PosixFileLock())
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
