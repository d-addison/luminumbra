"""Simulated workers only: portable pipe readiness, deadlines, kill/reap and pins."""
from pathlib import Path
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from meshgen_e4_fixture import arguments
from luminumbra_author.contracts import Refusal, Toolchain, TOOLCHAIN, canonical, digest
from luminumbra_author.meshgen_worker import run_simulated, command, finisher_envelope, run_native
from luminumbra_author.process import run_owned, ProcessTree
from luminumbra_author import meshgen_worker


# Each fixture has one writer at a time. Readers wait for child exit or READY,
# so no destination is held open during replacement, including on Windows.
PID_PUBLICATION = '''def publish(path, pids):
    temporary = path.with_suffix('.publishing')
    try:
        with temporary.open('w') as stream:
            json.dump(pids, stream)
            stream.flush()
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
'''


class WorkerSupervision(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.resolved = arguments()['resolved']
        self.config = self.root/'effective_config.json'; self.config.write_bytes(self.resolved.stored_bytes)

    def refusal(self, rule, call):
        with self.assertRaises(Refusal) as caught: call()
        self.assertEqual(caught.exception.finding['rule_id'], rule, caught.exception.finding)

    def simulate(self, **kwargs):
        return run_simulated(self.config, self.resolved.stored_sha256, self.resolved.config_digest, **kwargs)

    def test_simulated_worker_verifies_config_and_records_no_native_measurement(self):
        value = self.simulate()
        self.assertTrue(value['simulated'])
        self.assertEqual(value['config_digest'], self.resolved.config_digest)
        self.assertEqual(value['observations']['finish']['blender_version'], 'simulated')
        self.assertEqual([s['id'] for s in value['observations']['finish']['steps']], self.resolved.effective['finish']['steps'])

    def test_configuration_bytes_and_resolved_identity_rechecked_before_spawn(self):
        with patch.object(meshgen_worker, 'run_owned') as spawn:
            self.config.write_bytes(self.resolved.stored_bytes+b' ')
            self.refusal('config.digest_mismatch', self.simulate); spawn.assert_not_called()
            self.config.write_bytes(self.resolved.stored_bytes)
            self.refusal('config.digest_mismatch', lambda: run_simulated(self.config, self.resolved.stored_sha256, 'f'*64))
            spawn.assert_not_called()

    def test_policy_forgery_refused_after_simulated_worker(self):
        self.refusal('worker.policy', lambda: self.simulate(mode='policy'))

    def test_windows_compatible_readiness_timeout_cancellation_and_reaping(self):
        # Clock progresses independently of OS scheduler; no sleeps or long polls.
        class Clock:
            def __init__(self): self.value = 0
            def __call__(self): self.value += .05; return self.value
        self.refusal('worker.readiness', lambda: self.simulate(mode='no_ready', readiness_timeout=1, clock=Clock()))
        cancelled = threading.Event(); cancelled.set()
        self.refusal('cancelled', lambda: self.simulate(mode='hang', cancel=cancelled))
        self.refusal('worker.timeout', lambda: run_owned([sys.executable, '-I', '-S', '-B', '-c',
            'import sys,threading; sys.stdin.buffer.readline(); print("READY",flush=True); threading.Event().wait(60)'],
            b'{}\n', timeout=1, clock=Clock(), require_ready=False))

    def test_crash_missing_readiness_bounded_stdout_stderr_and_stdin(self):
        def script(code): return [sys.executable, '-I', '-S', '-B', '-c', code]
        self.refusal('worker.readiness', lambda: run_owned(script('import sys; sys.stdin.buffer.readline(); sys.exit(7)'), b'{}\n', timeout=5))
        self.refusal('worker.failed', lambda: self.simulate(mode='crash'))
        self.refusal('worker.limit', lambda: run_owned(script('import sys; sys.stdin.buffer.readline(); print("READY",flush=True); sys.stdout.buffer.write(b"x"*1048577)'), b'{}\n', timeout=5))
        result = run_owned(script('import sys; sys.stdin.buffer.readline(); print("READY",flush=True); sys.stderr.buffer.write(b"x"*70000)'), b'{}\n', timeout=5)
        self.assertEqual(len(result['log']), 65536); self.assertTrue(result['log_truncated'])
        self.refusal('worker.limit', lambda: run_owned(script('pass'), b'x'*65537, timeout=5))

    def test_blocked_stdin_writer_is_bounded_and_reaped(self):
        class Clock:
            def __init__(self): self.value = 0
            def __call__(self): self.value += .05; return self.value
        self.refusal('worker.timeout', lambda: run_owned([sys.executable, '-I', '-S', '-B', '-c',
            'import threading; threading.Event().wait(60)'], b'x'*65536, timeout=1, clock=Clock(), require_ready=False))

    def test_readiness_line_endings_split_reads_preserve_binary_response(self):
        import luminumbra_author.process as owner
        popen = subprocess.Popen
        class ByteReader:
            def __init__(self, stream): self.stream = stream
            def __getattr__(self, name): return getattr(self.stream, name)
            def read(self, size): return self.stream.read(min(size, 1))
        def spawn(*args, **kwargs):
            process = popen(*args, **kwargs)
            process.stdout = ByteReader(process.stdout)
            return process
        payload = b'\x00\r\nREADY\n\xff'
        for marker in (b'READY\n', b'READY\r\n', b'READY\r', b'READY\r\r\n', b'XREADY\n'):
            with self.subTest(marker=marker), patch.object(owner.subprocess, 'Popen', side_effect=spawn):
                source = ('import sys; sys.stdin.buffer.readline(); '
                          f'sys.stdout.buffer.write({marker+payload!r}); sys.stdout.buffer.flush()')
                call = lambda: run_owned([sys.executable, '-I', '-S', '-B', '-c', source], b'{}\n', timeout=5)
                if marker in (b'READY\n', b'READY\r\n'):
                    self.assertEqual(call()['stdout'], payload)
                else:
                    self.refusal('worker.readiness', call)

    def test_pinned_game_script_envelope_recheck_and_native_lane_unavailable(self):
        (self.root/'processor').write_bytes(b'not executed')
        script = self.root/'scripts'/'finish.py'; script.parent.mkdir(); script.write_text('import bpy\nfrom helper import run\n')
        helper = script.parent/'helper.py'; helper.write_text('def run(): pass\n')
        document = {'schema': TOOLCHAIN, 'id': 'test.compiler', 'processor': 'processor', 'files': {'processor': digest(b'not executed')},
            'scripts': {'finisher': {'path': 'scripts/finish.py', 'sha256': digest(script.read_bytes()),
                                    'modules': {'scripts/helper.py': digest(helper.read_bytes())}}}}
        manifest = self.root/'toolchain.json'; manifest.write_bytes(canonical(document)); toolchain = Toolchain(manifest)
        envelope = finisher_envelope(toolchain, self.root, self.resolved, self.root/'.luminumbra-author-recipes'/'raw.glb', self.root/'.luminumbra-author-recipes'/'scratch')
        self.assertFalse(envelope['qualified'])
        self.assertEqual(envelope['memory_mib'], 4096)
        for flag in ('--factory-startup', '--disable-autoexec', '--threads'): self.assertIn(flag, envelope['blender_args'])
        self.assertEqual(len(envelope['resource_environment']), 3)
        for raw, output in ((self.root/'escaped.glb', self.root/'.luminumbra-author-recipes'/'scratch'),
                            (self.root/'.luminumbra-author-recipes'/'raw.glb', self.root/'escaped'),
                            (self.root/'.luminumbra-author-recipes'/'..'/'raw.glb', self.root/'.luminumbra-author-recipes'/'scratch')):
            self.refusal('candidate.path', lambda: finisher_envelope(toolchain, self.root, self.resolved, raw, output))
        helper.write_text('def run(): return 42\n')
        self.refusal('toolchain.changed', lambda: finisher_envelope(toolchain, self.root, self.resolved, self.root/'.luminumbra-author-recipes'/'raw.glb', self.root/'.luminumbra-author-recipes'/'scratch'))
        self.refusal('worker.unavailable', run_native)

    def test_eof_stops_simulated_worker_tree_with_bounded_readiness(self):
        request = {'config_path': str(self.config), 'stored_sha256': self.resolved.stored_sha256,
                   'config_digest': self.resolved.config_digest, 'mode': 'descendant', 'timeout': self.resolved.effective['timeouts']['finish_s'], 'memory_mib': 4096}
        process = subprocess.Popen(command('_supervisor'), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=os.name != 'nt', close_fds=True)
        tree, thread = None, None
        try:
            tree = ProcessTree(process, 4096)
            ready = queue.Queue(maxsize=1)
            thread = threading.Thread(target=lambda: ready.put(process.stdout.readline(64)), daemon=True); thread.start()
            process.stdin.write(canonical(request)+b'\n'); process.stdin.flush()
            self.assertEqual(ready.get(timeout=5), b'READY\n')
            path = self.config.with_suffix('.worker.pids')
            import json
            # READY follows replace and closure of every publication handle.
            owned = [process.pid, *json.loads(path.read_text())]
            process.stdin.close()
            process.wait(timeout=5)
            for pid in owned:
                if os.name == 'posix':
                    self.assertFalse((Path('/proc')/str(pid)).exists(), 'Owned process remains, including zombie state')
                    with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
                else:
                    from luminumbra_author.meshgen_jobs import process_start
                    self.assertIsNone(process_start(pid))
        finally:
            if tree: tree.stop()
            elif process.poll() is None: process.kill()
            process.wait(timeout=5)
            if tree: tree.close()
            for stream in (process.stdin, process.stdout, process.stderr): stream.close()
            if thread: thread.join(timeout=1)

    def test_descendant_readiness_follows_complete_closed_pid_publications(self):
        import json
        request = {'config_path': str(self.config), 'stored_sha256': self.resolved.stored_sha256,
                   'config_digest': self.resolved.config_digest, 'mode': 'descendant', 'timeout': 5}
        opened, publications, ready = [], [], []
        create, replace = tempfile.NamedTemporaryFile, os.replace
        def capture(**kwargs):
            stream = create(**kwargs)
            opened.append(stream)
            return stream
        def publish(source, destination):
            self.assertTrue(opened[-1].closed)
            self.assertEqual(source.parent, destination.parent)
            self.assertEqual(destination.read_bytes(), b'previous witness')
            replace(source, destination)
            publications.append(destination)
        def witness():
            self.assertEqual(len(publications), 2)
            self.assertEqual(self.config.with_suffix('.descendant.pid').read_text(), '123')
            self.assertEqual(json.loads(self.config.with_suffix('.worker.pids').read_text()), [os.getpid(), 123])
            ready.append(True)
        for suffix in ('.descendant.pid', '.worker.pids'):
            self.config.with_suffix(suffix).write_bytes(b'previous witness')
        with patch.object(meshgen_worker, '_request', return_value=request), \
                patch.object(subprocess, 'Popen') as spawn, \
                patch.object(meshgen_worker.tempfile, 'NamedTemporaryFile', side_effect=capture), \
                patch.object(meshgen_worker.os, 'replace', side_effect=publish), \
                patch.object(meshgen_worker, '_ready', side_effect=witness), \
                patch.object(meshgen_worker.threading, 'Event'):
            spawn.return_value.pid = 123
            self.assertEqual(meshgen_worker._simulator(), 0)
        self.assertEqual(ready, [True])
        self.assertFalse(list(self.root.glob('.pids-*')))

    def test_worker_modules_ship_without_ml_bpy_and_work_in_installed_package(self):
        import ast
        import importlib.util
        import zipfile
        service = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location('e4_package', service/'package.py')
        package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
        names = ['meshgen_candidate.py', 'meshgen_provenance.py', 'meshgen_worker.py', 'meshgen_metrics.py']
        for name in names:
            self.assertIn(name, package.SERVICE_MODULES)
            tree = ast.parse((service/'luminumbra_author'/name).read_text())
            for node in ast.walk(tree):
                imports = [a.name for a in node.names] if isinstance(node, ast.Import) else ([node.module] if isinstance(node, ast.ImportFrom) and not node.level else [])
                for imported in imports: self.assertIn(imported.split('.')[0], sys.stdlib_module_names)
        app = package.build(self.root/'service.pyz')
        with zipfile.ZipFile(app) as archive:
            for name in names: self.assertIn('luminumbra_author/'+name, archive.namelist())
            self.assertFalse(any('finish_candidate.py' in n or 'provenance.json' in n or 'mock_backend' in n for n in archive.namelist()))
        source = ('import sys; sys.path.insert(0,sys.argv[1]); '
                  'from luminumbra_author.meshgen_worker import run_simulated; '
                  'print(run_simulated(sys.argv[2],sys.argv[3],sys.argv[4])["simulated"])')
        result = subprocess.run([sys.executable, '-I', '-S', '-B', '-c', source, str(app), str(self.config),
                                 self.resolved.stored_sha256, self.resolved.config_digest], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(result.stdout.strip(), 'True')
        workflow = (service.parents[3]/'.github/workflows/blender-authoring.yml').read_text()
        self.assertIn('unittest discover -s tools/blender/authoring/service/tests', workflow)
        self.assertIn('windows-2025', workflow); self.assertIn('ubuntu-24.04', workflow)

    def test_unavailable_tree_enforcement_refuses_and_reaps_without_handshake(self):
        import luminumbra_author.process as owner
        spawned = []
        popen = subprocess.Popen
        def capture(*args, **kwargs):
            process = popen(*args, **kwargs); spawned.append(process); return process
        with patch.object(owner.subprocess, 'Popen', side_effect=capture), patch.object(owner, 'ProcessTree', side_effect=OSError('unavailable')):
            self.refusal('worker.unavailable', lambda: run_owned([sys.executable, '-I', '-S', '-B', '-c',
                'import sys; sys.stdin.buffer.readline()'], b'{}\n', timeout=5))
        self.assertEqual(len(spawned), 1)
        self.assertIsNotNone(spawned[0].returncode)
        for stream in (spawned[0].stdin, spawned[0].stdout, spawned[0].stderr): self.assertTrue(stream.closed)

    def test_every_owned_pid_disappears_after_timeout_cancellation_and_exception(self):
        import json
        child_code = ("import subprocess,sys,time,json,os; from pathlib import Path; "
                      f"exec({PID_PUBLICATION!r}); "
                      "p=subprocess.Popen([sys.executable,'-I','-S','-B','-c','import time; time.sleep(60)'], start_new_session=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); publish(Path(sys.argv[1]),[os.getpid(),p.pid]); sys.exit(0)")
        for mode in ('timeout', 'cancel', 'exception', 'success'):
            with self.subTest(mode=mode):
                path = self.root/(mode+'.pids')
                source = ("import subprocess,sys,time,os,json; from pathlib import Path; sys.stdin.buffer.readline(); "
                          "p=subprocess.Popen([sys.executable,'-I','-S','-B','-c',"+repr(child_code)+",sys.argv[1]],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
                          "p.wait(); "
                          "print('READY',flush=True); " + ("sys.exit(0)" if mode == 'success' else "time.sleep(60)"))
                cancel = threading.Event()
                witnessed = []
                def ready():
                    witnessed.extend(json.loads(path.read_text()))
                    if mode == 'cancel': cancel.set()
                    if mode == 'exception': raise RuntimeError('callback failure')
                clock_start = time.monotonic()
                def clock(): return clock_start+4.6 if witnessed and mode == 'timeout' else time.monotonic()
                spawned = []
                popen = subprocess.Popen
                def capture(*args, **kwargs):
                    process = popen(*args, **kwargs); spawned.append(process); return process
                with patch('luminumbra_author.process.subprocess.Popen', side_effect=capture):
                    call = lambda: run_owned([sys.executable, '-I', '-S', '-B', '-c', source, str(path)], b'{}\n', timeout=5, cancel=cancel, clock=clock, ready_callback=ready)
                    if mode == 'exception':
                        with self.assertRaisesRegex(RuntimeError, 'callback failure'): call()
                    elif mode == 'success': call()
                    else: self.refusal('cancelled' if mode == 'cancel' else 'worker.timeout', call)
                self.assertEqual(len(witnessed), 2)
                for pid in [spawned[0].pid, *witnessed]:
                    if os.name == 'posix':
                        self.assertFalse((Path('/proc')/str(pid)).exists(), f'Owned PID {pid} remains (zombies are forbidden)')
                        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
                    else:
                        from luminumbra_author.meshgen_jobs import process_start
                        self.assertIsNone(process_start(pid))
                for stream in (spawned[0].stdin, spawned[0].stdout, spawned[0].stderr): self.assertTrue(stream.closed)

    def test_result_waits_for_delayed_stderr_and_writer_and_validates_late_failures(self):
        import luminumbra_author.process as owner
        for stream_kind in ('stderr', 'stdin'):
            for outcome in ('success', 'failure', 'deadline'):
                with self.subTest(stream=stream_kind, outcome=outcome):
                    gate, entered = threading.Event(), threading.Event()
                    spawned, exited_checks = [], [0]
                    popen = subprocess.Popen
                    class Delayed:
                        def __init__(self, stream): self.stream = stream
                        def __getattr__(self, name): return getattr(self.stream, name)
                        def delay(self):
                            entered.set()
                            if not gate.wait(5): raise OSError('test gate did not open')
                            if outcome == 'failure': raise OSError('late I/O failure')
                        def read(self, size):
                            # Drain the kernel pipe so the child can exit, then
                            # withhold buffered bytes from the supervision loop.
                            chunks = []
                            while True:
                                block = self.stream.read(size)
                                if not block: break
                                chunks.append(block)
                            self.delay()
                            return b''.join(chunks)
                        def close(self):
                            if stream_kind == 'stdin' and threading.current_thread() is not threading.main_thread(): self.delay()
                            else: gate.set()
                            return self.stream.close()
                    def spawn(*args, **kwargs):
                        process = popen(*args, **kwargs)
                        setattr(process, stream_kind, Delayed(getattr(process, stream_kind)))
                        spawned.append(process)
                        return process
                    clock_start = time.monotonic()
                    def clock():
                        if spawned and spawned[0].poll() is not None and entered.is_set():
                            exited_checks[0] += 1
                            if exited_checks[0] >= 5:
                                if outcome == 'deadline': return clock_start+9.6
                                gate.set()
                        return time.monotonic()
                    source = "import sys; sys.stdin.buffer.readline(); print('READY',flush=True); sys.stderr.buffer.write(b'x'*70000)"
                    with patch.object(owner.subprocess, 'Popen', side_effect=spawn):
                        call = lambda: run_owned([sys.executable, '-I', '-S', '-B', '-c', source], b'{}\n', timeout=10, clock=clock)
                        if outcome == 'success':
                            result = call()
                            self.assertEqual(result['log'], b'x'*65536)
                            self.assertEqual(result['log_bytes'], 70000)
                            self.assertTrue(result['log_truncated'])
                        else: self.refusal('worker.failed' if outcome == 'failure' else 'worker.timeout', call)
                    self.assertGreaterEqual(exited_checks[0], 5)
                    self.assertIsNotNone(spawned[0].returncode)
                    self.assertTrue(all(stream.closed for stream in (spawned[0].stdin, spawned[0].stdout, spawned[0].stderr)))

    @unittest.skipUnless(sys.platform.startswith('linux'),
                         'prctl subreaper state is Linux-only; Windows Job path covered by '
                         'test_windows_job_ownership_terminates_only_assigned_child_and_drains_before_close')
    def test_supervision_preserves_caller_subreaper_and_unrelated_child_lifecycle(self):
        import ctypes
        libc = ctypes.CDLL(None, use_errno=True)
        def state():
            value = ctypes.c_int()
            self.assertEqual(libc.prctl(37, ctypes.byref(value), 0, 0, 0), 0)  # GET only
            return value.value
        before = state()
        unrelated = subprocess.Popen([sys.executable, '-I', '-S', '-B', '-c',
            "import sys; print('READY',flush=True); sys.stdin.buffer.read(); print('DONE',flush=True)"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(unrelated.stdout.readline(), b'READY\n')
            self.simulate()
            self.assertEqual(state(), before); self.assertIsNone(unrelated.poll())
            result = run_owned([sys.executable, '-I', '-S', '-B', '-c',
                "import sys; sys.stdin.buffer.readline(); print('READY',flush=True); print('real')"], b'{}\n', timeout=5)
            self.assertEqual(result['stdout'], b'real\n')
            self.assertEqual(state(), before); self.assertIsNone(unrelated.poll())
            unrelated.stdin.close()
            self.assertEqual(unrelated.wait(timeout=5), 0)
            self.assertEqual(unrelated.stdout.read(), b'DONE\n')
        finally:
            if unrelated.poll() is None: unrelated.kill()
            unrelated.wait(timeout=5)
            for stream in (unrelated.stdin, unrelated.stdout, unrelated.stderr): stream.close()

    def test_windows_job_accounting_uses_injected_absolute_deadline_and_disallows_breakaway(self):
        import ctypes
        from unittest.mock import Mock
        import luminumbra_author.process as owner
        class Function:
            def __init__(self, action): self.action = action
            def __call__(self, *args): return self.action(*args)
        flags, queries, closed, terminated = [], [], [], []
        kernel = Mock()
        kernel.CreateJobObjectW = Function(lambda *args: 123)
        kernel.SetInformationJobObject = Function(lambda job, kind, value, size: flags.append(value._obj.basic.flags) or 1)
        kernel.AssignProcessToJobObject = Function(lambda *args: 1)
        kernel.TerminateJobObject = Function(lambda *args: terminated.append(args) or 1)
        kernel.CloseHandle = Function(lambda handle: closed.append(handle) or 1)
        def query(job, kind, value, size, unused):
            queries.append(job); value._obj.active = 1; return 1
        kernel.QueryInformationJobObject = Function(query)
        with patch.object(owner.os, 'name', 'nt'), patch.object(owner.sys, 'platform', 'win32'), patch.object(ctypes, 'WinDLL', return_value=kernel, create=True):
            tree = ProcessTree(Mock(_handle=42), 4096)
            self.assertEqual(flags, [0x2000 | 0x200])
            self.assertEqual(flags[0] & (0x800 | 0x1000), 0)  # both breakaway flags absent
            readings = iter((9.99, 10.0))
            waits = []
            with patch.object(owner.threading, 'Event') as event, patch.object(owner.time if hasattr(owner, 'time') else time, 'monotonic', side_effect=AssertionError('real clock used')):
                event.return_value.wait.side_effect = lambda delay: waits.append(delay)
                tree.stop()
                with self.assertRaisesRegex(OSError, 'deadline'):
                    tree.reap(deadline=10, clock=lambda: next(readings))
            tree.close()
        self.assertEqual(len(queries), 2); self.assertEqual(waits, [.005])
        self.assertEqual(closed, [123]); self.assertEqual(terminated, [(123, 1)])

    def test_windows_job_setup_failure_kills_reaps_and_closes_without_handshake(self):
        import ctypes
        import io
        from unittest.mock import Mock
        import luminumbra_author.process as owner
        class Handle(int): pass
        for phase, kill_fails in (('limits', False), ('assignment', False), ('assignment', True)):
            with self.subTest(phase=phase, kill_fails=kill_fails):
                handle = Handle(42); handle.Close = Mock()
                process = Mock(_handle=handle)
                process.stdin, process.stdout, process.stderr = io.BytesIO(), io.BytesIO(), io.BytesIO()
                kernel = Mock()
                kernel.CreateJobObjectW.return_value = 123
                kernel.SetInformationJobObject.return_value = phase != 'limits'
                kernel.AssignProcessToJobObject.return_value = 0
                events = []
                def kill():
                    events.append('kill')
                    if kill_fails: raise OSError('injected kill failure')
                process.kill.side_effect = kill
                process.wait.side_effect = lambda **kwargs: events.append('wait')
                handle.Close.side_effect = lambda: events.append('handle close')
                with patch.object(owner.os, 'name', 'nt'), patch.object(owner.sys, 'platform', 'win32'), patch.object(ctypes, 'WinDLL', return_value=kernel, create=True), patch.object(ctypes, 'get_last_error', return_value=5, create=True), patch.object(ctypes, 'WinError', side_effect=lambda code: OSError('injected Job setup failure'), create=True), patch.object(owner.subprocess, 'Popen', return_value=process), patch.object(owner.threading, 'Thread') as thread, patch.object(owner, '_enable_subreaper') as subreaper, patch.object(owner, '_require_pidfds') as pidfds:
                    with self.assertRaises(Refusal) as caught:
                        run_owned(['fake'], b'handshake', timeout=2, clock=lambda: 10)
                self.assertEqual(caught.exception.finding['rule_id'], 'worker.unavailable')
                self.assertEqual(str(caught.exception.__cause__), 'injected Job setup failure')
                self.assertEqual(events, ['kill', 'wait', 'handle close'])
                process.kill.assert_called_once_with()
                process.wait.assert_called_once_with(timeout=2)
                kernel.CloseHandle.assert_called_once_with(123)
                self.assertEqual(kernel.AssignProcessToJobObject.call_count, phase == 'assignment')
                if phase == 'assignment':
                    self.assertEqual(kernel.AssignProcessToJobObject.call_args.args[0], 123)
                    self.assertEqual(kernel.AssignProcessToJobObject.call_args.args[1].value, 42)
                kernel.TerminateJobObject.assert_not_called()
                thread.assert_not_called(); subreaper.assert_not_called(); pidfds.assert_not_called()
                self.assertTrue(all(stream.closed for stream in (process.stdin, process.stdout, process.stderr)))

    def test_windows_job_ownership_terminates_only_assigned_child_and_drains_before_close(self):
        import ctypes
        from unittest.mock import Mock
        import luminumbra_author.process as owner
        kernel = Mock()
        kernel.CreateJobObjectW.return_value = 123
        kernel.SetInformationJobObject.return_value = 1
        kernel.AssignProcessToJobObject.return_value = 1
        kernel.TerminateJobObject.return_value = 1
        events, active = [], iter((2, 1, 0))
        kernel.TerminateJobObject.side_effect = lambda *args: events.append(('terminate', args)) or 1
        def query(job, kind, value, size, unused):
            value._obj.active = next(active)
            events.append(('active', value._obj.active))
            return 1
        kernel.QueryInformationJobObject.side_effect = query
        kernel.CloseHandle.side_effect = lambda job: events.append(('close', job)) or 1
        with patch.object(owner.os, 'name', 'nt'), patch.object(owner.sys, 'platform', 'win32'), patch.object(ctypes, 'WinDLL', return_value=kernel, create=True), patch.object(owner, '_require_pidfds') as pidfds, patch.object(owner, '_enable_subreaper') as subreaper, patch.object(owner.os, 'kill') as kill, patch.object(owner.os, 'waitpid', create=True) as waitpid, patch.object(owner.threading, 'Event') as event:
            tree = ProcessTree(Mock(_handle=42), 4096)
            tree.stop()
            tree.reap(deadline=10, clock=lambda: 9)
            tree.close(); tree.close()
        kernel.AssignProcessToJobObject.assert_called_once()
        self.assertEqual(kernel.AssignProcessToJobObject.call_args.args[0], 123)
        self.assertEqual(kernel.AssignProcessToJobObject.call_args.args[1].value, 42)
        self.assertEqual(events, [('terminate', (123, 1)), ('active', 2), ('active', 1), ('active', 0), ('close', 123)])
        self.assertEqual([call.args for call in event.return_value.wait.call_args_list], [(.005,), (.005,)])
        kill.assert_not_called(); waitpid.assert_not_called()
        pidfds.assert_not_called(); subreaper.assert_not_called()

    def test_cleanup_closes_job_streams_handle_and_joins_after_wait_and_kill_failures(self):
        import io
        from unittest.mock import Mock
        import luminumbra_author.process as owner
        for mode in ('wait_retry', 'kill', 'stop'):
            with self.subTest(mode=mode):
                process = Mock()
                process.stdin, process.stdout, process.stderr = io.BytesIO(), io.BytesIO(b'READY\nresult'), io.BytesIO()
                process.poll.return_value = 0; process.returncode = 0
                process.wait.side_effect = [subprocess.TimeoutExpired('root', 1), OSError('retry failed')]
                process.kill.side_effect = OSError('kill failed') if mode == 'kill' else None
                tree = Mock()
                if mode == 'stop': tree.stop.side_effect = OSError('stop failed')
                # Run I/O threads synchronously so no scheduling obscures the
                # unconditional finalizers after injected cleanup failures.
                threads = []
                class Thread:
                    def __init__(self, target, args=(), **kwargs): self.target, self.args, self.joined = target, args, []; threads.append(self)
                    def start(self): self.target(*self.args)
                    def join(self, timeout): self.joined.append(timeout)
                readings = iter((0, .1, .2, .3, .4, .5, .6))
                with patch.object(owner.os, 'name', 'nt'), patch.object(owner.sys, 'platform', 'win32'), patch.object(owner.subprocess, 'Popen', return_value=process), patch.object(owner, 'ProcessTree', return_value=tree), patch.object(owner.threading, 'Thread', Thread):
                    with self.assertRaises(OSError if mode == 'stop' else subprocess.TimeoutExpired):
                        run_owned(['fake'], b'{}\n', timeout=2, clock=lambda: next(readings))
                self.assertEqual([call.kwargs['timeout'] for call in process.wait.call_args_list], [1.8, 1.7])
                process.kill.assert_called_once()
                tree.reap.assert_called_once()
                self.assertEqual(tree.reap.call_args.kwargs['deadline'], 2)
                tree.close.assert_called_once(); process._handle.Close.assert_called_once()
                self.assertTrue(all(stream.closed for stream in (process.stdin, process.stdout, process.stderr)))
                self.assertEqual([thread.joined for thread in threads], [[1.6], [1.5], [1.4]])

    @unittest.skipUnless(sys.platform.startswith('linux'),
                         'pidfd adoption and waitpid(-1) are Linux-only here; Windows Job path covered by '
                         'test_windows_job_ownership_terminates_only_assigned_child_and_drains_before_close')
    def test_linux_reaper_rescans_fork_races_and_waits_for_all_children(self):
        from unittest.mock import Mock
        import luminumbra_author.process as owner
        tree = ProcessTree.__new__(ProcessTree)
        tree.job, tree.control, tree.supervised, tree.same_group = None, None, False, True
        tree._pidfds = {}
        snapshots = iter(({101}, {101, 102}, {102}))
        def discover(**kwargs):
            self.assertEqual(kwargs['deadline'], 1)
            for pid in next(snapshots): tree._pidfds.setdefault((pid, 1), pid+1000)
        tree._descendants = Mock(side_effect=discover)
        tree._pidfd_signal, tree._pidfd_close = Mock(), Mock()
        replies = iter(((0, 0), (101, 9), (0, 0), (102, 9), ChildProcessError()))
        def reaped(selector, options):
            self.assertEqual((selector, options), (-1, 1))
            value = next(replies)
            if isinstance(value, BaseException): raise value
            return value
        with patch.object(owner.os, 'name', 'posix'), \
                patch.object(owner.sys, 'platform', 'linux'), \
                patch.object(owner.signal, 'SIGKILL', 9, create=True), \
                patch.object(owner.os, 'WNOHANG', 1, create=True), \
                patch.object(owner.os, 'kill') as kill, \
                patch.object(owner.os, 'waitpid', side_effect=reaped, create=True), \
                patch.object(owner.threading, 'Event') as event:
            tree.reap(deadline=1, clock=lambda: 0)
        self.assertEqual(tree._descendants.call_count, 3)
        kill.assert_not_called()
        self.assertEqual({call.args[0] for call in tree._pidfd_signal.call_args_list}, {1101, 1102})
        self.assertTrue(all(call.args[1] == 9 for call in tree._pidfd_signal.call_args_list))
        self.assertEqual(tree._pidfds, {})
        self.assertEqual(event.return_value.wait.call_count, 2)

    @unittest.skipUnless(sys.platform.startswith('linux'),
                         'Linux held-launcher/control pipe and subreaper adoption are Linux-only; '
                         'Windows Job path covered by '
                         'test_windows_job_ownership_terminates_only_assigned_child_and_drains_before_close '
                         'and test_cleanup_closes_job_streams_handle_and_joins_after_wait_and_kill_failures')
    def test_real_supervisor_owner_eof_reaps_detached_orphan_descendants(self):
        import json
        from luminumbra_author.process import _owned_command
        path = self.root/'owner-eof.pids'
        grandchild = "import time; time.sleep(60)"
        intermediate = ("import subprocess,sys,os,json; from pathlib import Path; "
            f"exec({PID_PUBLICATION!r}); "
            "p=subprocess.Popen([sys.executable,'-I','-S','-B','-c',"+repr(grandchild)+"],start_new_session=True,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
            "publish(Path(sys.argv[1]),[os.getpid(),p.pid])")
        root = ("import subprocess,sys,time,os,json; from pathlib import Path; "
            f"exec({PID_PUBLICATION!r}); "
            "p=subprocess.Popen([sys.executable,'-I','-S','-B','-c',"+repr(intermediate)+",sys.argv[1]],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
            "p.wait(); path=Path(sys.argv[1]); publish(path,[os.getpid(),*json.loads(path.read_text())]); print('READY',flush=True); time.sleep(60)")
        wrapped, read_fd, write_fd = _owned_command([sys.executable, '-I', '-S', '-B', '-c', root, str(path)], 5, None)
        process, tree = None, None
        try:
            process = subprocess.Popen(wrapped, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       pass_fds=(read_fd,), start_new_session=True)
            process._ownership_control = write_fd
            tree = ProcessTree(process)
            os.write(write_fd, b'S')
            ready = queue.Queue(maxsize=1)
            reader = threading.Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True); reader.start()
            self.assertEqual(ready.get(timeout=3), b'READY\n')
            owned = [process.pid, *json.loads(path.read_text())]
            self.assertEqual(len(owned), 4)
            tree.stop()  # Exactly EOF on the owner's dedicated control pipe.
            process.wait(timeout=3)
            for pid in owned:
                self.assertFalse((Path('/proc')/str(pid)).exists(), f'PID {pid}, including zombies, survived owner EOF')
                with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
            reader.join(timeout=1)
        finally:
            os.close(read_fd)
            if tree is not None: tree.stop(); tree.close()
            else: os.close(write_fd)
            if process is not None:
                process.wait(timeout=3)
                process.stdout.close(); process.stderr.close()
