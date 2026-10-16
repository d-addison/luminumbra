"""Own the compiler until it exits, times out, or its service pipe closes."""
from __future__ import annotations

import os
from pathlib import Path
import signal
import subprocess
import sys
import threading

from .contracts import atomic_json

LOG_LIMIT = 64 * 1024
_SUPERVISOR_PID = None
WINDOWS_JOB_LIMITS = 0x2000  # KILL_ON_JOB_CLOSE; neither BREAKAWAY_OK nor SILENT_BREAKAWAY_OK
MAX_PROC_ENTRIES = 65536
MAX_OWNED_PROCESSES = 4096


def _require_pidfds():
    """Probe enforcement before launching any owned Linux worker."""
    if not sys.platform.startswith('linux') or not callable(getattr(os, 'pidfd_open', None)) or not callable(getattr(signal, 'pidfd_send_signal', None)):
        raise OSError('Linux descendant ownership requires pidfd support')
    try:
        fd = os.pidfd_open(os.getpid())
        try: signal.pidfd_send_signal(fd, 0)
        finally: os.close(fd)
    except OSError as error:
        raise OSError('Linux descendant ownership requires working pidfd enforcement') from error


def _proc_entries():
    with os.scandir('/proc') as entries:
        for entry in entries: yield entry.name


def _proc_identity(pid):
    """Return parent PID and creation ticks, or skip an unavailable stat record."""
    try:
        # comm is arbitrary bytes and may contain spaces or closing parentheses.
        fields = (Path('/proc')/str(pid)/'stat').read_bytes().rsplit(b')', 1)[1].split()
        return int(fields[1]), int(fields[19])  # fields 4 (ppid) and 22 (starttime)
    except (OSError, ValueError, IndexError): return None


def _cleanup_remaining(deadline, clock):
    remaining = deadline-clock()
    if remaining <= 0:
        raise OSError('Owned descendants did not disappear before deadline')
    return remaining


def supervise(command, result_path, timeout):
    """Internal child entry point. EOF on stdin also handles service crashes."""
    log = bytearray()
    cancelled = threading.Event()
    timed_out = False
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=os.name != "nt")

    def stop():
        if process.poll() is None:
            try:
                if os.name == "nt":
                    # The registered asset_processor is a single native process.
                    process.kill()
                else:
                    os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def watch_parent():
        # Parent sends no payload: retaining this pipe is the ownership signal.
        while os.read(sys.stdin.fileno(), 1):
            pass
        cancelled.set()
        stop()

    total = 0

    def drain():
        nonlocal total
        for block in iter(lambda: process.stdout.read(8192), b""):
            total += len(block)
            log.extend(block[:max(0, LOG_LIMIT - len(log))])

    watcher = threading.Thread(target=watch_parent, daemon=True)
    reader = threading.Thread(target=drain, daemon=True)
    watcher.start()
    reader.start()
    try:
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            stop()
            process.wait()
    finally:
        stop()
        reader.join(timeout=5)
        process.stdout.close()
    atomic_json(Path(result_path), {
        "returncode": process.returncode, "cancelled": cancelled.is_set(), "timed_out": timed_out,
        "log": log.decode("utf-8", errors="replace"), "log_truncated": total > LOG_LIMIT,
        "log_bytes": total,
    })


def _enable_subreaper():
    global _SUPERVISOR_PID
    if os.name == 'nt': return
    # Called exclusively by dedicated ownership supervisor entry points.
    if not sys.platform.startswith('linux'):
        raise OSError('Owned descendant reaping requires Linux or Windows')
    _require_pidfds()
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'Cannot enable descendant adoption')
    _SUPERVISOR_PID = os.getpid()


class ProcessTree:
    """Own a trusted child tree, including Windows descendants in a Job object.

    The child must wait for the parent's stdin handshake before starting work;
    assignment to the Job happens before that handshake. This is not a launcher
    for untrusted executable code (raw bpy remains unavailable).
    """
    def __init__(self, process, memory_mib=None, *, same_group=False,
                 proc_entries=None, proc_reader=None, pidfd_open=None, pidfd_signal=None,
                 pidfd_close=None):
        self.process, self.job = process, None
        if os.name != 'nt':
            if same_group and _SUPERVISOR_PID != os.getpid():
                raise OSError('Descendant ownership requires a dedicated supervisor')
            self.control = getattr(process, '_ownership_control', None)
            self.supervised = self.control is not None
            self.group = os.getpgrp() if same_group else process.pid
            self.same_group = same_group
            self._pidfds = {}
            if same_group:
                _require_pidfds()
                self._proc_entries = proc_entries or _proc_entries
                self._proc_reader = proc_reader or _proc_identity
                self._pidfd_open = pidfd_open or os.pidfd_open
                self._pidfd_signal = pidfd_signal or signal.pidfd_send_signal
                self._pidfd_close = pidfd_close or os.close
                self._root_pid = os.getpid()
                self._root_identity = self._proc_reader(self._root_pid)
                if self._root_identity is None:
                    raise OSError('Cannot establish supervisor creation identity')
            return
        import ctypes
        from ctypes import wintypes as w
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        for name, args, result in (
            ('CreateJobObjectW', [w.LPVOID, w.LPCWSTR], w.HANDLE),
            ('SetInformationJobObject', [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD], w.BOOL),
            ('AssignProcessToJobObject', [w.HANDLE, w.HANDLE], w.BOOL),
            ('TerminateJobObject', [w.HANDLE, w.UINT], w.BOOL),
            ('QueryInformationJobObject', [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD, w.LPVOID], w.BOOL),
            ('CloseHandle', [w.HANDLE], w.BOOL)):
            function = getattr(kernel, name); function.argtypes, function.restype = args, result
        class Basic(ctypes.Structure):
            _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64), ('flags', w.DWORD),
                       ('minimum', ctypes.c_size_t), ('maximum', ctypes.c_size_t), ('active', w.DWORD),
                       ('affinity', ctypes.c_size_t), ('priority', w.DWORD), ('scheduling', w.DWORD)]
        class IO(ctypes.Structure):
            _fields_ = [(n, ctypes.c_uint64) for n in ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]
        class Extended(ctypes.Structure):
            _fields_ = [('basic', Basic), ('io', IO), ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                       ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]
        self.kernel = kernel
        self.job = kernel.CreateJobObjectW(None, None)
        if not self.job: raise ctypes.WinError(ctypes.get_last_error())
        try:
            limits = Extended()
            limits.basic.flags = WINDOWS_JOB_LIMITS
            if memory_mib is not None:
                limits.basic.flags |= 0x200  # JOB_MEMORY: aggregate descendants
                limits.job_memory = memory_mib*1024**2
            if not kernel.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                raise ctypes.WinError(ctypes.get_last_error())
            if not kernel.AssignProcessToJobObject(self.job, w.HANDLE(process._handle)):
                raise ctypes.WinError(ctypes.get_last_error())
        except BaseException:
            kernel.CloseHandle(self.job); self.job = None
            raise

    def _descendants(self, *, deadline, clock):
        """Retain handles only for a creation-identity-validated parent chain.

        One proc scan builds adjacency lists, so frontier expansion is linear.
        A changed/exited parent or child is deferred to the next scan, where
        subreaper adoption can establish a new chain to this supervisor.
        """
        children = {}
        entries = iter(self._proc_entries())
        count = 0
        try:
            while True:
                _cleanup_remaining(deadline, clock)
                try: name = next(entries)
                except StopIteration: break
                _cleanup_remaining(deadline, clock)
                count += 1
                if count > MAX_PROC_ENTRIES:
                    raise OSError('Linux process scan exceeds traversal limit')
                if not name.isdecimal(): continue
                pid = int(name)
                identity = self._proc_reader(pid)
                _cleanup_remaining(deadline, clock)
                if identity is not None:
                    children.setdefault(identity[0], []).append((pid, identity))
        finally:
            if hasattr(entries, 'close'): entries.close()
        frontier = [(self._root_pid, self._root_identity)]
        visited = {self._root_pid}
        while frontier:
            _cleanup_remaining(deadline, clock)
            parent, parent_identity = frontier.pop()
            for pid, identity in children.get(parent, ()):
                _cleanup_remaining(deadline, clock)
                if pid in visited: continue
                visited.add(pid)
                if len(visited) > MAX_OWNED_PROCESSES:
                    raise OSError('Linux descendant tree exceeds traversal limit')
                key = (pid, identity[1])
                fd = self._pidfds.get(key)
                new_fd = fd is None
                if new_fd:
                    if len(self._pidfds) >= MAX_OWNED_PROCESSES:
                        raise OSError('Linux retained process handles exceed traversal limit')
                    try: fd = self._pidfd_open(pid)
                    except ProcessLookupError: continue
                    except OSError as error:
                        raise OSError('Linux descendant pidfd acquisition is unavailable') from error
                try:
                    _cleanup_remaining(deadline, clock)
                    current_parent = self._proc_reader(parent)
                    _cleanup_remaining(deadline, clock)
                    current = self._proc_reader(pid)
                    _cleanup_remaining(deadline, clock)
                    if current_parent is None or current_parent[1] != parent_identity[1] or current != identity:
                        continue
                    # Opening before revalidation binds this handle to the
                    # captured child, even if its numeric PID is reused later.
                    self._pidfds[key] = fd
                    new_fd = False
                    frontier.append((pid, identity))
                finally:
                    if new_fd: self._pidfd_close(fd)

    def stop(self, *, deadline=None, clock=None):
        if os.name == 'nt':
            if self.job and not self.kernel.TerminateJobObject(self.job, 1):
                import ctypes
                raise ctypes.WinError(ctypes.get_last_error())
        elif self.supervised:
            # EOF asks the dedicated subreaper to kill/reap its descendants.
            if self.control is not None:
                os.close(self.control); self.control = None
        elif self.same_group:
            if deadline is None or clock is None:
                raise OSError('Linux descendant termination requires an absolute deadline and clock')
            self._descendants(deadline=deadline, clock=clock)
            for key, fd in list(self._pidfds.items()):
                _cleanup_remaining(deadline, clock)
                try: self._pidfd_signal(fd, signal.SIGKILL)
                except ProcessLookupError:
                    self._pidfd_close(fd); del self._pidfds[key]
                except OSError as error:
                    raise OSError('Linux descendant pidfd signalling is unavailable') from error
                _cleanup_remaining(deadline, clock)
        else:
            try: os.killpg(self.group, signal.SIGKILL)
            except ProcessLookupError: pass

    def _job_active(self):
        import ctypes
        from ctypes import wintypes as w
        class Accounting(ctypes.Structure):
            _fields_ = [('user', ctypes.c_int64), ('kernel', ctypes.c_int64),
                        ('period_user', ctypes.c_int64), ('period_kernel', ctypes.c_int64),
                        ('faults', w.DWORD), ('total', w.DWORD), ('active', w.DWORD), ('terminated', w.DWORD)]
        info = Accounting()
        if not self.kernel.QueryInformationJobObject(self.job, 1, ctypes.byref(info), ctypes.sizeof(info), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return info.active

    def reap(self, *, deadline, clock):
        if os.name == 'nt':
            while self.job and self._job_active():
                remaining = deadline-clock()
                if remaining <= 0: raise OSError('Owned Windows job did not drain before deadline')
                threading.Event().wait(min(.005, remaining))
            return
        if not self.same_group: return  # Caller owns only the supervisor root.
        while True:
            self.stop(deadline=deadline, clock=clock)  # Include forks during a scan.
            reaped, no_children = set(), False
            try:
                for _ in range(MAX_OWNED_PROCESSES):
                    _cleanup_remaining(deadline, clock)
                    pid, _ = os.waitpid(-1, os.WNOHANG)
                    _cleanup_remaining(deadline, clock)
                    if not pid: break
                    reaped.add(pid)
            except ChildProcessError:
                no_children = True
            for key in list(self._pidfds):
                _cleanup_remaining(deadline, clock)
                if no_children or key[0] in reaped:
                    self._pidfd_close(self._pidfds.pop(key))
            if no_children:
                _cleanup_remaining(deadline, clock)
                return
            remaining = _cleanup_remaining(deadline, clock)
            threading.Event().wait(min(.005, remaining))

    def close(self):
        for key in list(getattr(self, '_pidfds', {})):
            self._pidfd_close(self._pidfds.pop(key))
        if os.name != 'nt' and getattr(self, 'control', None) is not None:
            os.close(self.control); self.control = None
        if self.job:
            self.kernel.CloseHandle(self.job); self.job = None


def _linux_supervisor(command, control_fd, deadline, memory_mib=None):
    """Dedicated child entry point. Inherit worker pipes; own no caller children."""
    import json
    import select
    import time
    start_read = start_write = None
    cleanup_errors = []
    def cleanup(action):
        try: action()
        except BaseException as error: cleanup_errors.append(error)
    def close_start_read():
        nonlocal start_read
        fd, start_read = start_read, None
        if fd is not None: os.close(fd)
    def close_start_write():
        nonlocal start_write
        fd, start_write = start_write, None
        if fd is not None: os.close(fd)
    try:
        _enable_subreaper()
        if memory_mib is not None:
            import resource
            cap = memory_mib*1024**2
            resource.setrlimit(resource.RLIMIT_AS, (cap, cap))
        # The caller first owns this supervisor. A separate pipe holds the
        # trusted launcher until this supervisor also owns the worker tree.
        if os.read(control_fd, 1) != b'S': return 1
        start_read, start_write = os.pipe()
        bootstrap = ('import json,os,sys; fd=int(sys.argv[1]); start=os.read(fd,1); os.close(fd); '
                     'command=json.loads(sys.argv[2]);\n'
                     'if start != b"S": raise SystemExit(1)\n'
                     'os.execvp(command[0],command)')
        wrapped = [sys.executable, '-I', '-S', '-B', '-c', bootstrap, str(start_read), json.dumps(command)]
        tree = None
        process = subprocess.Popen(wrapped, close_fds=True, pass_fds=(start_read,))
        try:
            close_start_read()
            tree = ProcessTree(process, same_group=True)
            tree._descendants(deadline=deadline, clock=time.monotonic)
            if not any(pid == process.pid for pid, _ in tree._pidfds):
                raise OSError('Cannot establish held launcher creation identity')
            os.write(start_write, b'S')
            close_start_write()
            work_deadline = deadline-min(.5, max(0, deadline-time.monotonic())/2)
            while process.poll() is None:
                remaining = work_deadline-time.monotonic()
                if remaining <= 0: break
                readable, _, _ = select.select([control_fd], [], [], min(.01, remaining))
                if readable and not os.read(control_fd, 1): break
        finally:
            # EOF releases a held launcher to exit even if termination fails.
            # Clear each saved fd before closing so outer finalization is safe.
            cleanup(close_start_read)
            cleanup(close_start_write)
            direct_fd = next((fd for (pid, _), fd in tree._pidfds.items()
                              if pid == process.pid), None) if tree is not None else None
            def stop_direct():
                try:
                    if direct_fd is not None:
                        tree._pidfd_signal(direct_fd, signal.SIGKILL)
                    else:
                        # Only setup failure before pidfd ownership permits
                        # Popen.kill(): this launcher has not been released.
                        process.kill()
                except ProcessLookupError: pass
            cleanup(stop_direct)
            if tree is not None:
                cleanup(lambda: tree.stop(deadline=deadline, clock=time.monotonic))
            cleanup(lambda: process.wait(timeout=max(0, deadline-time.monotonic())))
            # waitpid(-1) includes adopted descendants; reap the root first.
            if tree is not None:
                cleanup(lambda: tree.reap(deadline=deadline, clock=time.monotonic))
                cleanup(tree.close)
        return process.returncode if process.returncode is not None else 1
    finally:
        primary_error = sys.exc_info()[0] is not None
        cleanup(close_start_read)
        cleanup(close_start_write)
        cleanup(lambda: os.close(control_fd))
        if cleanup_errors and not primary_error: raise cleanup_errors[0]


def _owned_command(command, remaining, memory_mib):
    import json
    import time
    from .bootstrap import isolated_path
    control_read, control_write = os.pipe()
    bootstrap = ('import sys,json; sys.path[:]=json.loads(sys.argv[1]); '
                 'from luminumbra_author.process import _linux_supervisor; '
                 'raise SystemExit(_linux_supervisor(json.loads(sys.argv[2]),int(sys.argv[3]),float(sys.argv[4]),json.loads(sys.argv[5])))')
    wrapped = [sys.executable, '-I', '-S', '-B', '-c', bootstrap,
               json.dumps(isolated_path(str(Path(__file__).resolve().parent.parent))),
               json.dumps(command), str(control_read), str(time.monotonic()+remaining), json.dumps(memory_mib)]
    return wrapped, control_read, control_write


def run_owned(command, payload, *, timeout, readiness_timeout=5, cancel=None,
              clock=None, environment=None, cwd=None, memory_mib=None,
              require_ready=True, retain_stdin=False, own_tree=True, ready_callback=None):
    """Bound readiness, writes, reads and lifetime without select on Windows pipes.

    All streams are drained in threads with bounded retained bytes. Completion
    always kills descendants, reaps the root and closes streams before returning.
    retain_stdin keeps the ownership pipe open until final cleanup.
    """
    import math
    import time
    from .contracts import require, Refusal
    clock = clock or time.monotonic
    require(type(timeout) in (int, float) and math.isfinite(timeout) and 0 < timeout <= 86400
            and type(readiness_timeout) in (int, float) and math.isfinite(readiness_timeout) and readiness_timeout > 0,
            'worker.limit', 'Invalid worker deadline.')
    require(type(payload) is bytes and len(payload) <= 65536, 'worker.limit', 'Worker stdin exceeds 64 KiB.')
    require(memory_mib is None or type(memory_mib) is int and 512 <= memory_mib <= 16384,
            'worker.limit', 'Invalid worker memory cap.')
    cancel = cancel or threading.Event()
    require(os.name == 'nt' or sys.platform.startswith('linux'), 'worker.unavailable', 'Descendant adoption is unavailable.')
    if os.name != 'nt':
        try: _require_pidfds()
        except OSError as error:
            raise Refusal('worker.unavailable', str(error)) from error
    start = clock()
    deadline = start+timeout
    # Leave cleanup time inside the original deadline, never add a new window.
    work_deadline = deadline-min(.5, timeout/2)
    control_read = control_write = None
    if own_tree and os.name == 'posix':
        command, control_read, control_write = _owned_command(command, max(0, deadline-clock()), memory_mib)
    try:
        extra = {'pass_fds': (control_read,)} if control_read is not None else {}
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=own_tree and os.name != 'nt', close_fds=True, env=environment, cwd=cwd, bufsize=0, **extra)
        if control_write is not None: process._ownership_control = control_write
    except BaseException:
        if control_write is not None: os.close(control_write)
        raise
    finally:
        if control_read is not None: os.close(control_read)
    tree = None
    output, log = bytearray(), bytearray()
    ready, eof, stderr_done, writer_done, overflow = (threading.Event() for _ in range(5))
    failures, counts, threads = [], [0, 0], []
    ready_size = 0
    def drain(stream, destination, index):
        nonlocal ready_size
        prefix = bytearray()
        try:
            while True:
                block = stream.read(8192)
                if not block: break
                counts[index] += len(block)
                limit = 1024**2 if index == 0 else LOG_LIMIT
                destination.extend(block[:max(0, limit-len(destination))])
                if index == 0:
                    if len(prefix) < 7: prefix.extend(block[:7-len(prefix)])
                    # Text workers may emit either native line ending. Keep
                    # the binary response intact after the exact marker.
                    if not ready.is_set():
                        if prefix[:6] == b'READY\n': ready_size = 6
                        elif prefix == b'READY\r\n': ready_size = 7
                        if ready_size: ready.set()
                    if counts[index] > limit: overflow.set()
        except (OSError, ValueError) as error: failures.append(error)
        finally:
            (eof if index == 0 else stderr_done).set()
    def send():
        try:
            view = memoryview(payload)
            while view:
                written = process.stdin.write(view[:8192])
                if not written: raise BrokenPipeError('Worker input closed')
                view = view[written:]
            if not retain_stdin: process.stdin.close()
        except (OSError, ValueError) as error: failures.append(error)
        finally: writer_done.set()

    try:
        try:
            tree = (ProcessTree(process, memory_mib) if own_tree else
                    ProcessTree(process, memory_mib, same_group=True) if os.name == 'posix' else None)
        except OSError as error: raise Refusal('worker.unavailable', 'Process-tree ownership is unavailable.') from error
        if control_write is not None: os.write(control_write, b'S')
        for function, args in ((drain, (process.stdout, output, 0)), (drain, (process.stderr, log, 1)), (send, ())):
            thread = threading.Thread(target=function, args=args, daemon=True)
            thread.start(); threads.append(thread)
        announced = False
        while True:
            # Observe completion before checking reader state: once all I/O
            # is done, the final readiness and overflow flags are stable.
            completed = process.poll() is not None and eof.is_set() and stderr_done.is_set() and writer_done.is_set()
            if ready.is_set() and not announced:
                announced = True
                if ready_callback is not None: ready_callback()
            now = clock()
            if cancel.is_set(): raise Refusal('cancelled', 'Worker owner cancelled the child.')
            if now >= work_deadline: raise Refusal('worker.timeout', 'Worker deadline expired.')
            if require_ready and not ready.is_set() and now-start >= min(timeout, readiness_timeout):
                raise Refusal('worker.readiness', 'Worker did not become ready before the deadline.')
            if overflow.is_set(): raise Refusal('worker.limit', 'Worker response exceeds its cap.')
            if completed: break
            eof.wait(0.01) if process.poll() is not None else cancel.wait(0.01)
        require(not require_ready or ready.is_set(), 'worker.readiness', 'Worker exited before readiness.')
        require(process.returncode == 0 and not failures, 'worker.failed', 'Worker exited unsuccessfully or closed stdin.')
        return {'stdout': bytes(output)[ready_size:] if require_ready else bytes(output), 'log': bytes(log),
                'log_bytes': counts[1], 'log_truncated': counts[1] > LOG_LIMIT, 'returncode': process.returncode}
    finally:
        primary_error = sys.exc_info()[0] is not None
        cleanup_errors = []
        def cleanup(action):
            try: action()
            except BaseException as error: cleanup_errors.append(error)
        try:
            if tree is not None: cleanup(lambda: tree.stop(deadline=deadline, clock=clock))
            else:
                if control_write is not None:
                    cleanup(lambda: os.close(control_write))
                cleanup(process.kill)
            try:
                process.wait(timeout=max(0, deadline-clock()))
            except BaseException as error:
                cleanup_errors.append(error)
                # A Linux supervisor must survive to reap its adopted children.
                if os.name == 'nt' or control_write is None:
                    cleanup(process.kill)
                cleanup(lambda: process.wait(timeout=max(0, deadline-clock())))
            if tree is not None:
                cleanup(lambda: tree.reap(deadline=deadline, clock=clock))
        finally:
            # Every close and join is attempted even if kill, wait or another
            # finalizer fails. All blocking cleanup shares the injected clock.
            if tree is not None: cleanup(tree.close)
            for stream in (process.stdin, process.stdout, process.stderr):
                cleanup(stream.close)
            for thread in threads:
                cleanup(lambda thread=thread: thread.join(timeout=max(0, deadline-clock())))
            if os.name == 'nt' and hasattr(process, '_handle'):
                cleanup(process._handle.Close)
        if cleanup_errors and not primary_error: raise cleanup_errors[0]
