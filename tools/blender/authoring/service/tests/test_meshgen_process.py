"""Deterministic Linux ownership races and cleanup budgets; no PID reuse needed."""
import os
from contextlib import contextmanager, ExitStack
import io
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

from luminumbra_author import process as owner
from luminumbra_author.meshgen_jobs import process_start
from luminumbra_author.contracts import Refusal


def stat_bytes(pid, parent, start, comm=b'worker', state=b'S'):
    # Fields 3..22: state, ppid, seventeen intervening numbers, starttime.
    fields = [state, str(parent).encode('ascii')] + [b'0']*17 + [str(start).encode('ascii')]
    return str(pid).encode('ascii')+b' ('+comm+b') '+b' '.join(fields)+b'\n'


class ProcessStatReaders(unittest.TestCase):
    def setUp(self):
        context = patch.object(Path, 'read_text', side_effect=AssertionError('Process identity must read bytes'))
        context.start(); self.addCleanup(context.stop)

    def test_creation_identity_ignores_comm_encoding_and_delimiters(self):
        path_type = type(Path())
        for comm in (b'worker\xff', b'worker\xe2\x82', b'worker ) with spaces)'):
            with self.subTest(comm=comm), patch.object(Path, 'read_bytes',
                    return_value=stat_bytes(123, 45, 678, comm)), patch.object(os, 'name', 'posix'), \
                    patch.object(sys, 'platform', 'linux'), \
                    patch.object(owner, 'Path', path_type), \
                    patch('luminumbra_author.meshgen_jobs.Path', path_type):
                self.assertEqual(owner._proc_identity(123), (45, 678))
                self.assertEqual(process_start(123), '678')

    def test_empty_and_malformed_scan_entries_are_skipped(self):
        for record in (b'', b'123 missing comm', b'123 (worker) S',
                       stat_bytes(123, 45, 678).replace(b'S 45', b'S invalid'),
                       stat_bytes(123, 45, 678).replace(b'678', b'invalid')):
            with self.subTest(record=record), patch.object(Path, 'read_bytes', return_value=record):
                self.assertIsNone(owner._proc_identity(123))

    def test_unreadable_and_vanished_scan_entries_are_skipped(self):
        for error in (FileNotFoundError(), ProcessLookupError(), PermissionError(), OSError('read failed')):
            with self.subTest(error=type(error).__name__), patch.object(Path, 'read_bytes', side_effect=error):
                self.assertIsNone(owner._proc_identity(123))

    def test_lock_identity_preserves_zombie_and_unreadable_owner_policy(self):
        path_type = type(Path())
        with patch.object(os, 'name', 'posix'), patch.object(sys, 'platform', 'linux'), \
                patch('luminumbra_author.meshgen_jobs.Path', path_type):
            for state in (b'Z', b'X'):
                with self.subTest(state=state), patch.object(path_type, 'read_bytes',
                        return_value=stat_bytes(123, 45, 678, b'worker\xff', state)):
                    self.assertIsNone(process_start(123))
            with patch.object(path_type, 'read_bytes', side_effect=FileNotFoundError()):
                self.assertIsNone(process_start(123))
            for result in (b'', b'123 (worker) S', stat_bytes(123, 45, 678).replace(b'678', b'\xff')):
                with self.subTest(record=result), patch.object(path_type, 'read_bytes', return_value=result):
                    with self.assertRaises(Refusal) as caught: process_start(123)
                    self.assertEqual(caught.exception.finding['rule_id'], 'busy')
            with patch.object(path_type, 'read_bytes', side_effect=PermissionError()):
                with self.assertRaises(Refusal) as caught: process_start(123)
                self.assertEqual(caught.exception.finding['rule_id'], 'busy')


class WorkerCompletion(unittest.TestCase):
    @contextmanager
    def complete_during_poll(self, stdout, *, stderr=b'', ready_callback=None, require_ready=True, cancel=None):
        process = Mock(returncode=0)
        process.stdin, process.stdout, process.stderr = io.BytesIO(), io.BytesIO(stdout), io.BytesIO(stderr)
        threads = []

        class Thread:
            def __init__(self, target, args=(), **kwargs):
                self.target, self.args = target, args
                self.join = Mock()

            def start(self): threads.append(self)

        def poll():
            # Deliver all bytes at the process completion observation. Checking
            # readiness and overflow before that observation loses these bytes.
            for thread in threads: thread.target(*thread.args)
            return 0

        process.poll.side_effect = poll
        tree = Mock()
        with ExitStack() as stack:
            stack.enter_context(patch.object(owner, '_require_pidfds'))
            stack.enter_context(patch.object(owner, '_owned_command', return_value=(['unused'], None, None)))
            stack.enter_context(patch.object(owner.subprocess, 'Popen', return_value=process))
            stack.enter_context(patch.object(owner, 'ProcessTree', return_value=tree))
            stack.enter_context(patch.object(owner.threading, 'Thread', Thread))
            try:
                yield lambda: owner.run_owned(['unused'], b'{}\n', timeout=2, clock=lambda: 10,
                    require_ready=require_ready, ready_callback=ready_callback, cancel=cancel)
            finally:
                self.assertTrue(all(stream.closed for stream in (process.stdin, process.stdout, process.stderr)))
                tree.stop.assert_called_once()
                tree.reap.assert_called_once()
                tree.close.assert_called_once()
                self.assertEqual(len(threads), 3)
                for thread in threads: thread.join.assert_called_once_with(timeout=2)

    def test_completion_rejects_stdout_overflow_after_last_loop_check(self):
        for require_ready in (False, True):
            with self.subTest(require_ready=require_ready):
                response = b'READY\n'+b'x'*(1024**2+1-6)
                with self.complete_during_poll(response, require_ready=require_ready) as run:
                    with self.assertRaises(Refusal) as caught: run()
                    self.assertEqual(caught.exception.finding['rule_id'], 'worker.limit')

    def test_completion_delivers_pending_readiness_once_and_retains_bounded_output(self):
        for marker in (b'READY\n', b'READY\r\n'):
            with self.subTest(marker=marker):
                callback = Mock()
                payload = b'x'*(1024**2-len(marker))
                log = b'y'*(owner.LOG_LIMIT+1)
                with self.complete_during_poll(marker+payload, stderr=log, ready_callback=callback) as run:
                    result = run()
                    callback.assert_called_once_with()
                    self.assertEqual(result['stdout'], payload)
                    self.assertEqual(result['log'], log[:owner.LOG_LIMIT])
                    self.assertEqual(result['log_bytes'], len(log))
                    self.assertTrue(result['log_truncated'])
                    self.assertEqual(result['returncode'], 0)

    def test_completion_readiness_callback_cancellation_and_error_still_abort(self):
        cancel = owner.threading.Event()
        callback = Mock(side_effect=cancel.set)
        with self.complete_during_poll(b'READY\nresult', ready_callback=callback, cancel=cancel) as run:
            with self.assertRaises(Refusal) as caught: run()
            self.assertEqual(caught.exception.finding['rule_id'], 'cancelled')
            callback.assert_called_once_with()
        callback = Mock(side_effect=RuntimeError('callback failure'))
        with self.complete_during_poll(b'READY\nresult', ready_callback=callback) as run:
            with self.assertRaisesRegex(RuntimeError, 'callback failure'): run()
            callback.assert_called_once_with()

    def test_root_success_stops_descendants_holding_either_pipe_before_draining(self):
        for platform in ('nt', 'posix'):
            for held in ('stdout', 'stderr', 'both'):
                with self.subTest(platform=platform, held=held), ExitStack() as stack:
                    released = owner.threading.Event()
                    waiting = []
                    class Pipe(io.BytesIO):
                        def __init__(self, data, inherited):
                            super().__init__(data)
                            self.inherited = inherited
                            self.waiting = owner.threading.Event()
                            if inherited: waiting.append(self.waiting)

                        def read(self, size):
                            data = super().read(size)
                            if not data and self.inherited:
                                self.waiting.set()
                                if not released.wait(2): raise OSError('Descendant still holds pipe')
                            return data

                    process = Mock(returncode=0)
                    process.stdin = io.BytesIO()
                    process.stdout = Pipe(b'READY\r\nresponse\x00\xff', held in ('stdout', 'both'))
                    process.stderr = Pipe(b'diagnostic\r\n', held in ('stderr', 'both'))
                    def poll():
                        for event in waiting: self.assertTrue(event.wait(1), 'Reader did not reach inherited pipe')
                        return 0
                    process.poll.side_effect = poll
                    tree = Mock()
                    tree.stop.side_effect = lambda **kwargs: released.set()
                    stack.enter_context(patch.object(owner.os, 'name', platform))
                    stack.enter_context(patch.object(owner.sys, 'platform', 'linux' if platform == 'posix' else 'win32'))
                    stack.enter_context(patch.object(owner, '_require_pidfds'))
                    stack.enter_context(patch.object(owner, '_owned_command', return_value=(['unused'], None, None)))
                    stack.enter_context(patch.object(owner.subprocess, 'Popen', return_value=process))
                    stack.enter_context(patch.object(owner, 'ProcessTree', return_value=tree))
                    result = owner.run_owned(['unused'], b'{}\n', timeout=2)
                    self.assertEqual(result['stdout'], b'response\x00\xff')
                    self.assertEqual(result['log'], b'diagnostic\r\n')
                    self.assertEqual(result['log_bytes'], len(b'diagnostic\r\n'))
                    self.assertEqual(result['returncode'], 0)
                    self.assertEqual(tree.stop.call_count, 2)
                    self.assertEqual(tree.stop.call_args_list[0], tree.stop.call_args_list[1])
                    tree.reap.assert_called_once_with(**tree.stop.call_args.kwargs)
                    tree.close.assert_called_once_with()
                    self.assertTrue(all(stream.closed for stream in (process.stdin, process.stdout, process.stderr)))


class LinuxSupervisorStartup(unittest.TestCase):
    def setUp(self):
        for target, attribute, value in ((owner.os, 'name', 'posix'),
                                         (owner.sys, 'platform', 'linux'),
                                         (owner.signal, 'SIGKILL', 9)):
            context = patch.object(target, attribute, value, create=True)
            context.start(); self.addCleanup(context.stop)

    def setup_failure(self, stack, phase, failure):
        stack.enter_context(patch.object(owner, '_enable_subreaper'))
        stack.enter_context(patch.object(owner, '_SUPERVISOR_PID', os.getpid()))
        stack.enter_context(patch.object(owner.os, 'getpgrp', return_value=os.getpid(), create=True))
        stack.enter_context(patch.object(owner.os, 'pidfd_open', create=True))
        stack.enter_context(patch.object(owner.signal, 'pidfd_send_signal', create=True))
        probe = stack.enter_context(patch.object(owner, '_require_pidfds'))
        identity = stack.enter_context(patch.object(owner, '_proc_identity'))
        if phase == 'constructor':
            stack.enter_context(patch.object(owner, 'ProcessTree', side_effect=failure))
        elif phase == 'probe': probe.side_effect = failure
        else: identity.side_effect = failure

    def test_setup_failures_kill_and_reap_held_worker_and_surface_original_error(self):
        for phase in ('constructor', 'probe', 'identity'):
            with self.subTest(phase=phase), ExitStack() as stack:
                failure = OSError('injected '+phase+' failure')
                self.setup_failure(stack, phase, failure)
                stack.enter_context(patch.object(owner.os, 'pipe', return_value=(11, 12)))
                stack.enter_context(patch.object(owner.os, 'read', return_value=b'S'))
                close = stack.enter_context(patch.object(owner.os, 'close'))
                release = stack.enter_context(patch.object(owner.os, 'write'))
                process = Mock()
                spawn = stack.enter_context(patch.object(owner.subprocess, 'Popen', return_value=process))
                stack.enter_context(patch('time.monotonic', return_value=10))
                with self.assertRaises(OSError) as caught:
                    owner._linux_supervisor(['unused'], 10, 15)
                self.assertIs(caught.exception, failure)
                self.assertEqual(spawn.call_args.kwargs['pass_fds'], (11,))
                release.assert_not_called()
                process.kill.assert_called_once_with()
                process.wait.assert_called_once_with(timeout=5)
                self.assertCountEqual([call.args[0] for call in close.call_args_list], [10, 11, 12])

    def test_setup_and_kill_failure_closes_start_pipe_before_bounded_wait(self):
        for phase in ('constructor', 'probe', 'identity'):
            with self.subTest(phase=phase), ExitStack() as stack:
                failure = OSError('injected '+phase+' failure')
                self.setup_failure(stack, phase, failure)
                stack.enter_context(patch.object(owner.os, 'pipe', return_value=(11, 12)))
                stack.enter_context(patch.object(owner.os, 'read', return_value=b'S'))
                closed, now = [], [10]
                stack.enter_context(patch.object(owner.os, 'close', side_effect=closed.append))
                stack.enter_context(patch('time.monotonic', side_effect=lambda: now[0]))
                process = Mock()
                process.kill.side_effect = OSError('injected kill failure')
                def wait(*, timeout):
                    self.assertCountEqual(closed, [11, 12])
                    self.assertEqual(timeout, 2)
                    now[0] += timeout
                    raise subprocess.TimeoutExpired('held launcher', timeout)
                process.wait.side_effect = wait
                stack.enter_context(patch.object(owner.subprocess, 'Popen', return_value=process))
                with self.assertRaises(OSError) as caught:
                    owner._linux_supervisor(['unused'], 10, 12)
                self.assertIs(caught.exception, failure)
                self.assertEqual(now[0], 12)
                process.kill.assert_called_once_with()
                process.wait.assert_called_once_with(timeout=2)
                self.assertCountEqual(closed, [10, 11, 12])

    def test_release_follows_ownership_and_release_error_still_cleans_up(self):
        for fail_release in (False, True):
            with self.subTest(fail_release=fail_release), ExitStack() as stack:
                events, tree = [], Mock()
                stack.enter_context(patch.object(owner, '_enable_subreaper'))
                stack.enter_context(patch.object(owner.os, 'pipe', return_value=(11, 12)))
                stack.enter_context(patch.object(owner.os, 'read', return_value=b'S'))
                stack.enter_context(patch.object(owner.os, 'close'))
                process = Mock(pid=101, returncode=0)
                process.poll.return_value = 0
                tree._pidfds = {(101, 20): 1001}
                stack.enter_context(patch.object(owner.subprocess, 'Popen', return_value=process))
                stack.enter_context(patch('time.monotonic', return_value=10))
                def owned(*args, **kwargs): events.append('owned'); return tree
                tree._descendants.side_effect = lambda **kwargs: events.append('validated')
                def release(fd, token):
                    self.assertEqual(events, ['owned', 'validated'])
                    self.assertEqual((fd, token), (12, b'S'))
                    events.append('released')
                    if fail_release: raise OSError('injected release failure')
                    return 1
                stack.enter_context(patch.object(owner, 'ProcessTree', side_effect=owned))
                stack.enter_context(patch.object(owner.os, 'write', side_effect=release))
                if fail_release:
                    with self.assertRaisesRegex(OSError, 'injected release failure'):
                        owner._linux_supervisor(['unused'], 10, 15)
                else: self.assertEqual(owner._linux_supervisor(['unused'], 10, 15), 0)
                self.assertEqual(events, ['owned', 'validated', 'released'])
                tree._descendants.assert_called_once()
                tree._pidfd_signal.assert_called_once_with(1001, 9)
                process.kill.assert_not_called()
                process.wait.assert_called_once_with(timeout=5)
                tree.stop.assert_called_once(); tree.reap.assert_called_once(); tree.close.assert_called_once()

    def test_retained_pidfd_failure_never_falls_back_and_all_finalizers_preserve_setup_error(self):
        with ExitStack() as stack:
            failure = OSError('injected ownership scan failure')
            process = Mock(pid=101)
            tree = Mock(_pidfds={(101, 20): 1001})
            tree._descendants.side_effect = failure
            for action in (tree._pidfd_signal, tree.stop, tree.reap, tree.close):
                action.side_effect = OSError('injected cleanup failure')
            process.wait.side_effect = subprocess.TimeoutExpired('held launcher', 0)
            stack.enter_context(patch.object(owner, '_enable_subreaper'))
            stack.enter_context(patch.object(owner.os, 'pipe', return_value=(11, 12)))
            stack.enter_context(patch.object(owner.os, 'read', return_value=b'S'))
            closed = []
            def close(fd):
                closed.append(fd)
                if fd == 10: raise OSError('injected control close failure')
            stack.enter_context(patch.object(owner.os, 'close', side_effect=close))
            stack.enter_context(patch.object(owner.subprocess, 'Popen', return_value=process))
            stack.enter_context(patch.object(owner, 'ProcessTree', return_value=tree))
            stack.enter_context(patch('time.monotonic', return_value=15))
            with self.assertRaises(OSError) as caught:
                owner._linux_supervisor(['unused'], 10, 15)
            self.assertIs(caught.exception, failure)
            tree._pidfd_signal.assert_called_once_with(1001, 9)
            process.kill.assert_not_called()
            process.wait.assert_called_once_with(timeout=0)
            tree.stop.assert_called_once(); tree.reap.assert_called_once(); tree.close.assert_called_once()
            self.assertCountEqual(closed, [10, 11, 12])

    @unittest.skipUnless(sys.platform.startswith('linux'),
                         'pidfd held-launcher is Linux-only; Windows Job path covered by '
                         'test_windows_job_setup_failure_kills_reaps_and_closes_without_handshake')
    def test_real_setup_failures_prevent_execution_and_leave_no_worker_or_zombie(self):
        original_spawn = subprocess.Popen
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary)/'executed'
            command = [sys.executable, '-I', '-S', '-B', '-c',
                       'import pathlib,sys,time; pathlib.Path(sys.argv[1]).touch(); time.sleep(60)', str(marker)]
            for phase in ('constructor', 'probe', 'identity'):
                with self.subTest(phase=phase), ExitStack() as stack:
                    failure = OSError('injected '+phase+' failure')
                    self.setup_failure(stack, phase, failure)
                    workers = []
                    def spawn(*args, **kwargs):
                        worker = original_spawn(*args, **kwargs)
                        workers.append(worker)
                        return worker
                    stack.enter_context(patch.object(owner.subprocess, 'Popen', side_effect=spawn))
                    read_fd, write_fd = os.pipe()
                    os.write(write_fd, b'S'); os.close(write_fd)
                    try:
                        with self.assertRaises(OSError) as caught:
                            owner._linux_supervisor(command, read_fd, time.monotonic()+5)
                        self.assertIs(caught.exception, failure)
                        self.assertEqual(len(workers), 1)
                        self.assertIn(workers[0].returncode, (-signal.SIGKILL, 1))
                        with self.assertRaises(ChildProcessError): os.waitpid(workers[0].pid, os.WNOHANG)
                        self.assertFalse(marker.exists(), 'worker executed before ownership was established')
                    finally:
                        for worker in workers:
                            if worker.poll() is None: worker.kill()
                            worker.wait(timeout=5)


class LinuxProcessOwnership(unittest.TestCase):
    def setUp(self):
        self.path_type = type(Path())
        self.root = os.getpid()
        for target, attribute, value in ((owner.os, 'name', 'posix'),
                                         (owner.sys, 'platform', 'linux'),
                                         (owner, '_SUPERVISOR_PID', self.root)):
            context = patch.object(target, attribute, value)
            context.start(); self.addCleanup(context.stop)
        context = patch.object(owner, '_require_pidfds')
        context.start(); self.addCleanup(context.stop)
        # Exercise the Linux algorithm on Windows without invoking POSIX APIs.
        for target, attribute, value in ((owner.os, 'getpgrp', lambda: self.root),
                                         (owner.os, 'WNOHANG', 1),
                                         (owner.signal, 'SIGKILL', 9)):
            context = patch.object(target, attribute, value, create=True)
            context.start(); self.addCleanup(context.stop)
        self.now = 0
        self.data = {self.root: (1, 10), 101: (self.root, 20), 102: (101, 30),
                     201: (1, 40), 202: (201, 50)}
        self.opened, self.closed, self.delivered = [], [], []
        self.bindings = {}

    def clock(self): return self.now

    def open_fd(self, pid):
        fd = 10000+len(self.opened)
        self.opened.append(pid)
        self.bindings[fd] = (pid, self.data[pid][1])
        return fd

    def send_signal(self, fd, value):
        self.assertEqual(value, signal.SIGKILL)
        self.delivered.append(self.bindings[fd])

    def tree(self, **kwargs):
        options = dict(proc_entries=lambda: iter([str(pid) for pid in self.data]),
                       proc_reader=lambda pid: self.data.get(pid), pidfd_open=self.open_fd,
                       pidfd_signal=self.send_signal, pidfd_close=self.closed.append)
        options.update(kwargs)
        tree = owner.ProcessTree(Mock(pid=101, _ownership_control=None), same_group=True, **options)
        self.addCleanup(tree.close)
        return tree

    def stop(self, tree): tree.stop(deadline=10, clock=self.clock)

    def test_only_owned_creation_identities_are_signalled_through_retained_handles(self):
        tree = self.tree()
        with patch.object(owner.os, 'kill', side_effect=AssertionError('bare PID signal')):
            self.stop(tree); self.stop(tree)
        self.assertEqual(self.opened, [101, 102])
        self.assertEqual(self.delivered, [(101, 20), (102, 30)]*2)
        self.assertFalse(self.closed)
        tree.close(); tree.close()
        self.assertEqual(sorted(self.closed), sorted(self.bindings))

    def test_stat_bytes_authorize_owned_children_and_ignore_unrelated_names(self):
        names = {101: b'worker\xff', 102: b'worker\xe2\x82', 201: b'unrelated ) with spaces)'}
        def read(path):
            pid = int(path.parent.name)
            return stat_bytes(pid, *self.data[pid], names.get(pid, b'worker'))
        with patch.object(owner, 'Path', self.path_type), \
                patch.object(self.path_type, 'read_bytes', autospec=True, side_effect=read):
            tree = self.tree(proc_reader=owner._proc_identity)
            self.stop(tree)
        self.assertEqual(self.opened, [101, 102])
        self.assertEqual(self.delivered, [(101, 20), (102, 30)])

    def test_bad_stat_entries_do_not_abort_scan_or_owned_child_cleanup(self):
        for failure in (b'', b'201 (bad) S', PermissionError(), FileNotFoundError()):
            with self.subTest(failure=failure):
                self.opened.clear(); self.closed.clear(); self.delivered.clear(); self.bindings.clear()
                reads = []
                def read(path):
                    pid = int(path.parent.name)
                    reads.append(pid)
                    if pid == 201:
                        if isinstance(failure, OSError): raise failure
                        return failure
                    return stat_bytes(pid, *self.data[pid])
                with patch.object(owner, 'Path', self.path_type), \
                        patch.object(self.path_type, 'read_bytes', autospec=True, side_effect=read):
                    # The unrelated process can disappear after enumeration.
                    tree = self.tree(proc_entries=lambda: iter(('201', '101', '102')),
                                     proc_reader=owner._proc_identity)
                    self.stop(tree)
                self.assertIn(201, reads)
                self.assertEqual(self.opened, [101, 102])
                self.assertEqual(self.delivered, [(101, 20), (102, 30)])
                tree.close()

    def test_exit_and_reuse_before_handle_acquisition_or_revalidation_is_discarded(self):
        for phase in ('before_open', 'after_open', 'exit'):
            with self.subTest(phase=phase):
                self.data[102] = (101, 30)
                self.delivered.clear()
                def open_fd(pid):
                    if pid == 102 and phase == 'before_open': self.data[pid] = (201, 99)
                    fd = self.open_fd(pid)
                    if pid == 102 and phase == 'after_open': self.data[pid] = (201, 99)
                    if pid == 102 and phase == 'exit': del self.data[pid]
                    return fd
                tree = self.tree(pidfd_open=open_fd)
                self.stop(tree)
                self.assertEqual(self.delivered, [(101, 20)])
                self.assertEqual(len(tree._pidfds), 1)
                self.assertIn(max(self.bindings), self.closed)
                tree.close()

    def test_reuse_after_validation_cannot_redirect_a_retained_handle(self):
        def send(fd, value):
            pid, start = self.bindings[fd]
            if pid == 101:
                self.data[102] = (201, 99)  # old grandchild exited; PID reused
                self.send_signal(fd, value)
            elif (pid, start) == (102, 30):
                raise ProcessLookupError('original process exited')
            else: self.fail('signal reached an unrelated process')
        tree = self.tree(pidfd_signal=send)
        self.stop(tree)
        self.assertEqual(self.delivered, [(101, 20)])
        self.assertEqual(self.closed, [10001])

    def test_exit_before_pidfd_open_is_harmless(self):
        def open_fd(pid):
            if pid == 102:
                del self.data[pid]
                raise ProcessLookupError('exited before open')
            return self.open_fd(pid)
        tree = self.tree(pidfd_open=open_fd)
        self.stop(tree)
        self.assertEqual(self.delivered, [(101, 20)])
        self.assertEqual(self.opened, [101])

    def test_runtime_pidfd_errors_refuse_without_bare_pid_fallback(self):
        for phase in ('open', 'signal'):
            with self.subTest(phase=phase):
                def open_fd(pid):
                    if phase == 'open' and pid == 102: raise OSError('denied')
                    return self.open_fd(pid)
                def send(fd, value): raise OSError('denied')
                tree = self.tree(pidfd_open=open_fd,
                                 pidfd_signal=send if phase == 'signal' else self.send_signal)
                with patch.object(owner.os, 'kill', side_effect=AssertionError('bare PID fallback')):
                    with self.assertRaisesRegex(OSError, 'pidfd .*unavailable'): self.stop(tree)
                tree.close()
        self.assertFalse(self.delivered)
        self.assertCountEqual(self.closed, self.bindings)

    def test_reused_parent_identity_cannot_authorize_a_grandchild(self):
        def open_fd(pid):
            fd = self.open_fd(pid)
            if pid == 102: self.data[101] = (self.root, 99)
            return fd
        tree = self.tree(pidfd_open=open_fd)
        self.stop(tree)
        self.assertEqual(self.delivered, [(101, 20)])
        self.assertEqual(self.closed, [10001])

    def test_reparented_descendant_is_validated_after_subreaper_adoption(self):
        def open_fd(pid):
            fd = self.open_fd(pid)
            if pid == 102: self.data[pid] = (self.root, 30)
            return fd
        tree = self.tree(pidfd_open=open_fd)
        self.stop(tree)
        self.assertEqual(self.delivered, [(101, 20)])
        self.stop(tree)
        self.assertIn((102, 30), self.delivered)
        self.assertNotIn((202, 50), self.delivered)

    def test_deadline_checked_inside_proc_iteration_and_identity_reads(self):
        for phase in ('iteration', 'read'):
            with self.subTest(phase=phase):
                self.now = 0
                visited, finished = [], []
                def entries():
                    try:
                        for pid in self.data:
                            visited.append(pid)
                            if phase == 'iteration': self.now += 5
                            yield str(pid)
                    finally: finished.append(True)
                def read(pid):
                    if phase == 'read' and visited: self.now += 5
                    return self.data.get(pid)
                tree = self.tree(proc_entries=entries, proc_reader=read)
                with self.assertRaisesRegex(OSError, 'deadline'): self.stop(tree)
                self.assertEqual(len(visited), 2)
                self.assertEqual(finished, [True])
                self.assertFalse(self.opened); self.assertFalse(self.delivered)

    def test_deadline_checked_inside_frontier_expansion_and_closes_pending_handle(self):
        for phase in ('open', 'parent_read', 'child_read'):
            with self.subTest(phase=phase):
                self.now = 0
                self.opened.clear(); self.bindings.clear(); self.closed.clear()
                def open_fd(pid):
                    fd = self.open_fd(pid)
                    if pid == 102 and phase == 'open': self.now = 10
                    return fd
                def read(pid):
                    if len(self.opened) == 2 and pid == (101 if phase == 'parent_read' else 102):
                        self.now = 10
                    return self.data.get(pid)
                tree = self.tree(pidfd_open=open_fd, proc_reader=read)
                with self.assertRaisesRegex(OSError, 'deadline'): self.stop(tree)
                self.assertEqual(self.opened, [101, 102])
                self.assertEqual(self.closed, [10001])
                self.assertFalse(self.delivered)
                tree.close()
                self.assertCountEqual(self.closed, [10000, 10001])

    def test_deadline_checked_between_handle_signals(self):
        def send(fd, value):
            self.send_signal(fd, value); self.now = 10
        tree = self.tree(pidfd_signal=send)
        with self.assertRaisesRegex(OSError, 'deadline'): self.stop(tree)
        self.assertEqual(self.delivered, [(101, 20)])

    def test_reaping_checks_clock_inside_continuing_waitpid_results(self):
        tree = self.tree()
        def waitpid(*args):
            self.now += 5
            return 101, 9
        with patch.object(owner.os, 'waitpid', side_effect=waitpid, create=True) as wait:
            with self.assertRaisesRegex(OSError, 'deadline'):
                tree.reap(deadline=10, clock=self.clock)
        self.assertEqual(wait.call_count, 2)
        self.assertTrue(tree._pidfds)  # remaining handles survive until close
        tree.close()
        self.assertCountEqual(self.closed, self.bindings)

    def test_scan_and_frontier_have_explicit_work_limits(self):
        for limit in ('MAX_PROC_ENTRIES', 'MAX_OWNED_PROCESSES'):
            with self.subTest(limit=limit):
                tree = self.tree()
                with patch.object(owner, limit, 2):
                    with self.assertRaisesRegex(OSError, 'traversal limit'): self.stop(tree)
                tree.close()
        self.assertFalse(self.delivered)


class OwnershipAvailability(unittest.TestCase):
    def test_missing_pidfd_apis_refuse_before_any_spawn(self):
        for target, name in ((owner.os, 'pidfd_open'), (owner.signal, 'pidfd_send_signal')):
            with self.subTest(api=name), patch.object(owner.os, 'name', 'posix'), \
                    patch.object(owner.sys, 'platform', 'linux'), \
                    patch.object(owner.os, 'pidfd_open', create=True), \
                    patch.object(owner.signal, 'pidfd_send_signal', create=True), \
                    patch.object(target, name, None, create=True), \
                    patch.object(owner.subprocess, 'Popen') as spawn:
                with self.assertRaises(Refusal) as caught:
                    owner.run_owned(['unused'], b'', timeout=1)
                self.assertEqual(caught.exception.finding['rule_id'], 'worker.unavailable')
                self.assertIn('pidfd', str(caught.exception))
                spawn.assert_not_called()

    def test_unavailable_kernel_pidfd_enforcement_refuses_and_closes_probe(self):
        for phase in ('open', 'signal'):
            with self.subTest(phase=phase), patch.object(owner.os, 'name', 'posix'), patch.object(owner.sys, 'platform', 'linux'), patch.object(owner.os, 'pidfd_open', create=True) as opened, patch.object(owner.signal, 'pidfd_send_signal', create=True) as send, patch.object(owner.os, 'close') as close, patch.object(owner.subprocess, 'Popen') as spawn:
                opened.return_value = 12345
                (opened if phase == 'open' else send).side_effect = OSError('unsupported')
                with self.assertRaises(Refusal) as caught:
                    owner.run_owned(['unused'], b'', timeout=1)
                self.assertEqual(caught.exception.finding['rule_id'], 'worker.unavailable')
                spawn.assert_not_called()
                self.assertEqual(close.call_count, phase == 'signal')

    def test_other_posix_platforms_keep_the_documented_refusal(self):
        with patch.object(owner.os, 'name', 'posix'), patch.object(owner.sys, 'platform', 'darwin'), patch.object(owner.subprocess, 'Popen') as spawn:
            with self.assertRaises(Refusal) as caught:
                owner.run_owned(['unused'], b'', timeout=1)
            self.assertEqual(caught.exception.finding['rule_id'], 'worker.unavailable')
            spawn.assert_not_called()
