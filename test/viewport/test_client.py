"""Portable real-process client tests; no renderer, Blender or GPU acceptance."""
import copy
from contextlib import contextmanager
import hashlib
import importlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
AUTHORING = ROOT / 'tools/blender/authoring'
PACKAGE = types.ModuleType('viewport_client_contract')
PACKAGE.__path__ = [str(AUTHORING / 'extension'), str(AUTHORING)]
sys.modules[PACKAGE.__name__] = PACKAGE
client_module = importlib.import_module(PACKAGE.__name__ + '.viewport_client')
installation = importlib.import_module(PACKAGE.__name__ + '.viewport_installation')
wire = client_module.wire
ViewportClient = client_module.ViewportClient
MATRIX = [1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1.]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def state(revision=1):
    view = MATRIX.copy()
    view[12] = revision
    return {'generation_id': 'b'*32, 'manifest_sha256': 'c'*64, 'scene_revision': 1,
            'camera_revision': revision, 'width': 1, 'height': 1, 'near_plane': .1,
            'far_plane': 100., 'view': view, 'projection': MATRIX.copy(), 'locals': []}


def wait_for(function, timeout=5, diagnostics=None):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = function()
        if result:
            return result
        time.sleep(.01)
    detail = diagnostics() if diagnostics is not None else None
    raise AssertionError('Timed out waiting for actual child-process result: ' + repr(detail))


def running(pid):
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes as w
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
        kernel.WaitForSingleObject.argtypes, kernel.WaitForSingleObject.restype = [w.HANDLE, w.DWORD], w.DWORD
        kernel.CloseHandle.argtypes = [w.HANDLE]
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            return False
        try:
            return kernel.WaitForSingleObject(handle, 0) == 258
        finally:
            kernel.CloseHandle(handle)
    proc = Path('/proc') / str(pid) / 'stat'
    if proc.exists() and proc.read_text().split(') ')[1].startswith('Z '):
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


class DeadlineClock:
    """Only session startup/frame time is virtual; polling and cleanup stay real."""
    sleep = staticmethod(time.sleep)

    def __init__(self, project):
        self.path = project / 'deadline-clock.json'
        self.observed = project / 'broker-clock-observed.json'
        self._lock = threading.Lock()
        self.value, self.samples = 0., 0
        wire.atomic_write(self.path, wire.canonical(self.value))

    def monotonic(self):
        with self._lock:
            self.samples += 1
            return self.value

    def advance(self, elapsed, *, broker=True):
        with self._lock:
            self.value += elapsed
            self.samples = 0
            if broker:
                wire.atomic_write(self.path, wire.canonical(self.value))

    def sampled_by_client(self):
        with self._lock:
            return self.samples >= 2

    def sampled_by_both(self):
        with self._lock:
            value, samples = self.value, self.samples
        # Two samples ensure a complete watchdog iteration after advancing.
        return (samples >= 2 and self.observed.exists()
                and json.loads(wire.read_record_file(self.observed, 64)) == value)


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='viewport-client-test-')
        self.root = Path(self.temporary.name)
        self.sdk, self.project = self.root / 'sdk', self.root / 'project'
        self.sdk.mkdir(); self.project.mkdir()
        self.clients = []
        self.host = self.sdk / 'bin/luminumbra_preview_capture'
        self.module = self.sdk / 'lib/libluminumbra_render_static.so'
        for path in (self.host, self.module, self.sdk / installation.SOURCE_INPUTS,
                     *(self.sdk / name for name in installation.SHADERS)):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'SYNTHETIC IDENTITY FIXTURE; NOT AN EXECUTABLE OR RENDERER\n' + path.name.encode())
        self.files = {p.relative_to(self.sdk).as_posix(): sha(p) for p in self.sdk.rglob('*') if p.is_file()}
        self.manifest = self.sdk / 'installation.json'
        self.native_receipt = self.root / 'session.json'
        self.qualification = self.root / 'qualification.json'
        native = {'schema': 'luminumbra.viewport.session.v1', 'status': 'complete', 'shutdown_complete': True,
            'source_commit': 'a'*40, 'source_dirty': False, 'source_input_sha256': self.files[installation.SOURCE_INPUTS],
            'executable_sha256': sha(self.host), 'module_sha256': sha(self.module),
            'resources': {Path(name).name: self.files[name] for name in installation.SHADERS}}
        self.native_receipt.write_text(json.dumps(native))
        qualification = {key: native[key] for key in ('source_commit', 'source_dirty', 'source_input_sha256')}
        qualification.update(passed=True, session_sha256=sha(self.native_receipt),
            inputs_before={'sdk': self.files}, inputs_after={'sdk': self.files})
        self.qualification.write_text(json.dumps(qualification))
        installation.seal_installation(self.host, self.qualification, self.native_receipt, self.manifest)
        self.broker_root = self.root / 'broker'
        self.broker_root.mkdir()
        self.broker = self.broker_root / 'client_broker.py'
        shutil.copyfile(AUTHORING / 'viewport/protocol.py', self.broker_root / 'protocol.py')
        shutil.copyfile(AUTHORING / 'service/luminumbra_author/file_io.py', self.broker_root / 'file_io.py')
        # This trusted test wrapper uses the real Broker and real fixture child;
        # the dummy SDK is solely an installation-identity contract fixture.
        self.broker.write_text('''import hashlib,json,os,sys,threading,time
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, WIRE_PATH)
import broker as broker_module
from broker import Broker
from protocol import atomic_write,canonical,read_record_file
config=json.loads(Path(sys.argv[2]).read_text())
project=Path(config['project_root'])
clock=project/'deadline-clock.json'
if clock.exists():
    clock_value=None
    clock_samples=0
    def monotonic():
        global clock_value,clock_samples
        # A bounded scalar, read through the portable shared-reader contract.
        value=json.loads(read_record_file(clock,64))
        clock_samples=clock_samples+1 if value==clock_value else 1
        clock_value=value
        if clock_samples==2:
            atomic_write(project/'broker-clock-observed.json',canonical(value))
        return value
    broker_module.time=SimpleNamespace(monotonic=monotonic,sleep=time.sleep)
mode=(project/'mode.txt').read_text()
os.environ['CLIENT_FIXTURE_EVIDENCE']=str(Path(config['root'])/'host-evidence')
os.environ['CLIENT_FIXTURE_IDENTITY']=str(project/'fixture-identity.json')
starts=project/'starts.json'
rows=json.loads(starts.read_text()) if starts.exists() else []
rows.append({'session':config['session'],'root':config['root'],'pid':os.getpid()})
atomic_write(starts,canonical(rows))
if mode=='broker-hang': time.sleep(60)
if mode=='crash-once' and len(rows)==1: sys.exit(9)
if mode=='crash-tree':
    mode='descendant'
    def crash():
        while not (project/('crash-'+config['session'])).exists():
            time.sleep(.01)
        os._exit(9)
    threading.Thread(target=crash,daemon=True).start()
config['command']=[sys.executable, FIXTURE_PATH, WIRE_PATH, mode]
config['executable_sha256']=hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest()
instance=Broker(key=bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY')), **config)
try:
    instance.run()
except BaseException as error:
    value={'session':config['session'],'broker_pid':os.getpid(),
        'child_pid':instance.child.pid if instance.child else None,
        'type':type(error).__name__,'error':str(error)}
    (project/('broker-exception-'+config['session']+'.json')).write_text(json.dumps(value))
    (project/'broker-exception.json').write_text(json.dumps(value))
    raise
'''.replace('WIRE_PATH', repr(str(AUTHORING / 'viewport')))
           .replace('FIXTURE_PATH', repr(str(ROOT / 'test/viewport/client_fixture.py'))))
        (self.project / 'mode.txt').write_text('echo')
        native['visual_approved'] = False
        (self.project / 'fixture-identity.json').write_text(json.dumps(native))

    def tearDown(self):
        for client in self.clients:
            self.assertTrue(client.close(wait=True, timeout=8), client.status)
        self.temporary.cleanup()

    def client(self, mode='echo', **options):
        (self.project / 'mode.txt').write_text(mode)
        arguments = dict(startup_timeout=2, frame_timeout=2, shutdown_timeout=.5, max_restarts=0)
        arguments.update(options)
        client = ViewportClient(sys.executable, self.broker, self.host, self.project, self.manifest, **arguments)
        self.clients.append(client)
        return client

    @contextmanager
    def controlled_deadlines(self):
        """Advance one client's session clock, optionally with its real broker.

        Patch only the session loop: installation checks and process teardown
        still use real time, including when an assertion fails with a host held.
        """
        clock = DeadlineClock(self.project)
        original_loop, original_stop = ViewportClient._session_loop, ViewportClient._stop

        def session_loop(client, *arguments):
            with mock.patch.object(client_module, 'time', clock):
                return original_loop(client, *arguments)

        def stop(client, *arguments):
            with mock.patch.object(client_module, 'time', time):
                return original_stop(client, *arguments)

        with mock.patch.object(ViewportClient, '_session_loop', session_loop), \
                mock.patch.object(ViewportClient, '_stop', stop):
            try:
                yield clock
            finally:
                for client in self.clients:
                    self.assertTrue(client.close(wait=True, timeout=8), self.diagnostics())

    def diagnostics(self):
        """Capture only bounded, identity-matched fixture evidence on failure."""
        values = []
        for client in self.clients:
            status = client.status
            evidence = {'client': status}
            retained = status['last_result']
            broker_pid = status['broker_pid']
            if broker_pid is None and retained and retained.get('session') == status['session']:
                broker_pid = retained.get('broker_pid')
            for stem in ('child-', 'broker-exception-'):
                path = self.project / (stem + status['session'] + '.json')
                try:
                    if path.exists():
                        row = json.loads(wire.read_record_file(path, 4096))
                        if (row.get('session') == status['session']
                                and row.get('broker_pid') == broker_pid):
                            evidence[stem[:-1]] = row
                except (OSError, ValueError) as error:
                    evidence[stem[:-1]] = {'read_error': str(error)}
            values.append(evidence)
        return values

    def wait(self, function, timeout=5):
        return wait_for(function, timeout, diagnostics=self.diagnostics)

    def child(self, client):
        path = self.project / ('child-' + client.status['session'] + '.json')
        if not path.exists():
            return None
        row = json.loads(wire.read_record_file(path, 4096))
        self.assertEqual(row['session'], client.status['session'])
        self.assertEqual(row['broker_pid'], client.status['broker_pid'])
        return row

    def test_installed_client_returns_owned_current_frame_and_closes_tree(self):
        client = self.client()
        desired = state()
        sequence = client.submit(desired)
        desired['view'][12] = 999
        header, payload = self.wait(client.poll)
        self.assertEqual(header['sequence'], sequence)
        self.assertEqual(header['state']['view'][12], 1)
        self.assertEqual(payload[:4], bytes([1, 2, 3, 255]))
        self.assertIsNone(client.poll())
        root, pid = Path(client.status['session_directory']), client.status['broker_pid']
        if os.name != 'nt':
            self.assertEqual(stat.S_IMODE(root.stat().st_mode), 0o700)
        self.assertTrue(client.close(wait=True, timeout=5))
        self.assertFalse(root.exists())
        self.assertFalse(running(pid))
        self.assertEqual(client.status['state'], 'closed')
        self.assertTrue(client.status['last_result']['child_reaped'])

    def test_draw_poll_and_latest_submit_do_not_wait_for_frame_validation(self):
        entered, release = threading.Event(), threading.Event()
        original = wire.Slots.read
        threads = []
        def blocked(slots, index):
            threads.append(threading.get_ident())
            entered.set()
            self.assertTrue(release.wait(3))
            return original(slots, index)
        with self.controlled_deadlines(), mock.patch.object(wire.Slots, 'read', blocked):
            client = self.client()
            child = self.wait(lambda: self.child(client))
            root = Path(client.status['session_directory'])
            self.wait(lambda: wire.read_readiness(root / 'broker-ready.json', child['session'],
                      pid=child['pid'], broker_pid=client.status['broker_pid']))
            client.submit(state())
            try:
                self.assertTrue(entered.wait(3))
                for revision in range(2, 52):
                    client.submit(state(revision))
                    self.assertIsNone(client.poll())
                self.assertFalse(release.is_set())  # All callbacks returned while read was blocked.
            finally:
                release.set()
            header, payload = self.wait(client.poll)
            self.assertEqual(header['state']['camera_revision'], 51)
            self.assertEqual(payload[0], 51)
            self.assertNotIn(threading.get_ident(), threads)

    def test_startup_timeout_and_retry_budget_use_fresh_private_sessions(self):
        with self.controlled_deadlines() as clock:
            client = self.client('broker-hang', startup_timeout=.3, shutdown_timeout=.1, max_restarts=1)
            client.submit(state())
            starts = self.project / 'starts.json'
            for attempt in (1, 2):
                # Each broker must actually start before expiring that attempt.
                self.wait(lambda: starts.exists() and clock.sampled_by_client()
                          and len(json.loads(wire.read_record_file(starts, 4096))) == attempt)
                clock.advance(.31)
            self.wait(lambda: client.status['state'] == 'failed', timeout=4)
            rows = json.loads(wire.read_record_file(starts, 4096))
            self.assertEqual(len(rows), 2)
            self.assertEqual(len({row['session'] for row in rows}), 2)
            self.assertEqual(len({row['root'] for row in rows}), 2)
            self.assertEqual(client.status['restarts'], 1)
            self.assertIn('startup deadline', client.status['error'])
            self.assertTrue(all(not running(row['pid']) and not Path(row['root']).exists() for row in rows))

    def test_allocated_slots_do_not_start_frame_deadline_before_host_ready(self):
        with self.controlled_deadlines() as clock:
            client = self.client('startup-delay', startup_timeout=3, frame_timeout=.35)
            client.submit(state())
            child = self.wait(lambda: self.child(client))
            root = Path(client.status['session_directory'])
            self.assertTrue(all((root / f'frame-{index}.bin').exists() for index in (0, 1)))
            for revision in range(2, 27):
                client.submit(state(revision))
                self.assertEqual(client.status['state'], 'starting', self.diagnostics())
                self.assertFalse((root / 'desired.bin').exists())
            # Exceed the unchanged .35s frame deadline while readiness is held.
            # Both watchdogs must finish observing that time before release.
            clock.advance(.5)
            self.wait(clock.sampled_by_both)
            self.assertEqual(client.status['state'], 'starting', self.diagnostics())
            self.assertFalse((root / 'desired.bin').exists())
            (self.project / ('release-' + child['session'])).touch()
            header, _ = self.wait(client.poll)
            self.assertEqual(header['state']['camera_revision'], revision)
            marker = wire.read_readiness(root / 'broker-ready.json', child['session'],
                                         pid=child['pid'], broker_pid=client.status['broker_pid'])
            self.assertIsNotNone(marker)

    def test_late_ready_marker_cannot_extend_startup_deadline_under_flood(self):
        with self.controlled_deadlines() as clock:
            stopped, receipt_seen = threading.Event(), threading.Event()
            original_stop = ViewportClient._stop
            def observed_stop(client, process, root, *arguments):
                stopped.set()
                # Retain the broker's completed evidence before bounded cleanup.
                self.wait(lambda: (root / 'broker-result.json').exists())
                receipt_seen.set()
                return original_stop(client, process, root, *arguments)
            with mock.patch.object(ViewportClient, '_stop', observed_stop):
                client = self.client('startup-delay', startup_timeout=1, frame_timeout=2, shutdown_timeout=.1)
                client.submit(state())
                child = self.wait(lambda: self.child(client))
                for revision in range(2, 27):
                    client.submit(state(revision))
                    clock.advance(.04, broker=False)
                    self.wait(lambda: stopped.is_set() or clock.sampled_by_client())
                clock.advance(.01, broker=False)
                self.assertTrue(stopped.wait(1.5), self.diagnostics())
                self.assertTrue(stopped.is_set(), self.diagnostics())
                # First prove the client expires independently of the broker.
                # Then let the broker finish and retain its own startup refusal.
                clock.advance(0)
                self.wait(receipt_seen.is_set)
                # A late release must not revive the stopped attempt.
                (self.project / ('release-' + child['session'])).touch()
                self.wait(lambda: client.status['state'] == 'failed')
                self.assertIsNone(client.poll())
                self.assertFalse(running(child['pid']))
                result = client.status['last_result']
                self.assertIsNone(result['host_ready'])
                self.assertEqual(result['published'], 0)
                self.assertTrue(result['child_reaped'])

    def test_invalid_host_ready_identity_and_size_fail_closed(self):
        for mode, message in (('startup-wrong-session', 'Readiness session/schema'),
                              ('startup-wrong-pid', 'Readiness child identity'),
                              ('startup-oversized', 'Record file size')):
            with self.subTest(mode=mode):
                client = self.client(mode)
                client.submit(state())
                starts = self.project / 'starts.json'
                # The worker replaces its provisional session during startup;
                # use the actual started broker's retained fixture identity.
                def started_broker():
                    try:
                        rows = json.loads(wire.read_record_file(starts, 4096))
                    except (OSError, ValueError):
                        return None
                    return next((row for row in rows if row['session'] == client.status['session']), None)
                started = self.wait(started_broker)
                self.wait(lambda: client.status['state'] == 'failed')
                result = client.status['last_result']
                self.assertIsNone(result['host_ready'])
                self.assertEqual(result['failure']['error'], message)
                self.assertEqual(result['session'], client.status['session'])
                self.assertEqual(result['broker_pid'], started['pid'])
                self.assertTrue(result['child_reaped'])

    def test_first_frame_gets_frame_timeout_after_readiness(self):
        client = self.client('startup-frame-delay', startup_timeout=2, frame_timeout=4)
        client.submit(state())
        child = self.wait(lambda: self.child(client))
        (self.project / ('release-' + child['session'])).touch()
        header, _ = self.wait(client.poll)
        self.assertEqual(header['session'], child['session'])
        self.assertEqual(client.status['state'], 'ready')

    def test_readiness_of_second_session_cannot_release_first_session(self):
        first = self.client('startup-delay', startup_timeout=3)
        first.submit(state())
        first_child = self.wait(lambda: self.child(first))
        second = self.client('echo')
        second.submit(state(2))
        second_header, _ = self.wait(second.poll)
        self.assertNotEqual(first_child['session'], second_header['session'])
        self.assertEqual(first.status['state'], 'starting', self.diagnostics())
        self.assertFalse((Path(first.status['session_directory']) / 'desired.bin').exists())
        (self.project / ('release-' + first_child['session'])).touch()
        first_header, _ = self.wait(first.poll)
        self.assertEqual(first_header['session'], first_child['session'])

    def test_child_exit_before_readiness_never_publishes_ready(self):
        client = self.client('startup-exit')
        client.submit(state())
        self.wait(lambda: client.status['state'] == 'failed')
        result = client.status['last_result']
        self.assertIsNone(result['host_ready'])
        self.assertEqual(result['child_returncode'], 7)
        self.assertEqual(result['published'], 0)

    def test_close_before_readiness_reaps_owned_process_tree(self):
        client = self.client('startup-delay-descendant', shutdown_timeout=.1)
        client.submit(state())
        child = self.wait(lambda: self.child(client))
        root = Path(client.status['session_directory'])
        broker_pid = client.status['broker_pid']
        self.assertEqual(client.status['state'], 'starting')
        self.assertTrue(client.close(wait=True, timeout=5), self.diagnostics())
        self.assertFalse(running(child['pid']))
        self.assertFalse(running(child['descendant']))
        self.assertFalse(running(broker_pid))
        self.assertFalse(root.exists())

    def test_restart_before_readiness_uses_fresh_session_and_latest_state(self):
        client = self.client('startup-delay', shutdown_timeout=.1)
        client.submit(state())
        self.wait(lambda: client.status['session_directory'])
        self.wait(lambda: client.status['broker_pid'])
        child = self.wait(lambda: self.child(client))
        previous_root = Path(client.status['session_directory'])
        # The latest pending state must survive restart without resubmission.
        sequence = client.submit(state(3))
        self.assertEqual(client.status['state'], 'starting', self.diagnostics())
        self.assertFalse((previous_root / 'desired.bin').exists())
        client.restart()

        # Each durable milestone gets its own polling budget. Shutdown and
        # cleanup precede replacement preflight; the directory is exposed before
        # the new session id, and the broker pid precedes fixture-child evidence.
        self.wait(lambda: not running(child['pid']))
        self.wait(lambda: not previous_root.exists())

        def replacement_directory():
            directory = client.status['session_directory']
            if directory and Path(directory) != previous_root:
                return Path(directory)
            return None

        # Preflight and private-directory creation have no earlier durable
        # public marker, so their finest available boundary is this directory.
        root = self.wait(replacement_directory)
        self.wait(lambda: client.status['session'] != child['session'])
        self.wait(lambda: client.status['broker_pid'])
        replacement = self.wait(lambda: self.child(client))
        self.assertNotEqual(root, previous_root)
        self.assertFalse(previous_root.exists())
        self.assertFalse(running(child['pid']))
        self.assertEqual(client.status['state'], 'starting', self.diagnostics())
        self.assertFalse((root / 'desired.bin').exists())
        # Keep both hosts behind session-specific readiness gates; frame delivery
        # gets a separate polling budget after releasing only the replacement.
        (self.project / ('release-' + replacement['session'])).touch()
        header, _ = self.wait(client.poll)
        self.assertEqual(header['session'], replacement['session'])
        self.assertEqual(header['sequence'], sequence)
        self.assertEqual(header['state'], state(3))
        self.assertNotEqual(header['session'], child['session'])
        self.assertFalse(running(child['pid']))

    def test_crash_recovery_replays_latest_state_in_a_fresh_session(self):
        client = self.client('crash-once', max_restarts=1)
        client.submit(state(3))
        header, _ = self.wait(client.poll)
        rows = json.loads((self.project / 'starts.json').read_text())
        self.assertEqual(len(rows), 2)
        self.assertNotEqual(rows[0]['session'], rows[1]['session'])
        self.assertEqual(header['session'], rows[1]['session'])
        self.assertEqual(header['state']['camera_revision'], 3)

    def test_explicit_restart_after_failure_is_new_session_without_repins(self):
        client = self.client('wrong')
        client.submit(state())
        self.wait(lambda: client.status['state'] == 'failed')
        previous = client.status['session']
        (self.project / 'mode.txt').write_text('echo')
        client.restart()
        header, _ = self.wait(client.poll)
        self.assertNotEqual(header['session'], previous)

    def test_frame_timeout_does_not_get_extended_by_flooding_desired_states(self):
        with self.controlled_deadlines() as clock:
            stopped, stopping = threading.Event(), {}
            original_stop = ViewportClient._stop
            def observed_stop(client, *arguments):
                stopping['at'] = clock.monotonic()
                stopped.set()
                return original_stop(client, *arguments)
            with mock.patch.object(ViewportClient, '_stop', observed_stop):
                self.check_frame_timeout_under_flood(clock, stopped, stopping)

    def check_frame_timeout_under_flood(self, clock, stopped, stopping):
        client = self.client('hang', frame_timeout=.35, startup_timeout=1, shutdown_timeout=.1)
        # Warm only fixture process startup. The frame deadline remains .35s.
        child = self.wait(lambda: self.child(client))
        broker_pid = client.status['broker_pid']
        directory = Path(client.status['session_directory'])
        self.wait(lambda: wire.read_readiness(directory / 'broker-ready.json', child['session'],
                  pid=child['pid'], broker_pid=broker_pid))
        # Start the frame clock only after the client has sent a request.
        client.submit(state())
        self.wait(lambda: (directory / 'desired.bin').exists() and clock.sampled_by_client())
        started, revision = clock.monotonic(), 1
        # Observe the watchdog decision before SDK verification and process
        # cleanup, whose duration is governed by separate existing bounds.
        # Advance only the client clock so the broker cannot mask a client
        # that incorrectly renews its deadline on every submitted state.
        while not stopped.is_set() and clock.monotonic() - started < .85:
            revision += 1
            client.submit(state(revision))
            clock.advance(.02, broker=False)
            self.wait(lambda: stopped.is_set() or clock.sampled_by_client())
        self.assertTrue(stopped.is_set(), client.status)
        self.assertLessEqual(stopping['at'] - started, .85)
        self.assertGreater(revision, 1)
        self.wait(lambda: client.status['state'] == 'failed')
        error = client.status['error']
        if 'frame deadline' not in error:
            # Any broker-side failure must carry deadline evidence; an
            # unrelated disconnect/parse failure must not pass as timeout.
            self.assertEqual(error, 'Viewport broker exited: 1')
            exception = json.loads((self.project / 'broker-exception.json').read_text())
            self.assertEqual(exception['session'], client.status['session'])
            self.assertEqual(exception['broker_pid'], broker_pid)
            self.assertEqual(exception['child_pid'], child['pid'])
            self.assertEqual(exception['type'], 'Refusal')
            self.assertIn(exception['error'], ('Host deadline/write', 'Frame publication deadline'))
            self.assertTrue(client.status['last_result']['child_reaped'])
            self.assertEqual(client.status['last_result']['status'], 'failed')
        self.assertIsNone(client.poll())
        self.assertFalse(running(broker_pid))
        self.assertFalse(running(child['pid']))
        self.assertFalse(directory.exists())

    def test_hung_shutdown_is_bounded_and_does_not_claim_cooperative_stop(self):
        client = self.client('stop-hang', shutdown_timeout=.15)
        client.submit(state())
        self.wait(client.poll)
        self.assertTrue(client.close(wait=True, timeout=4))
        self.assertEqual(client.status['state'], 'failed')
        self.assertIn('shutdown', client.status['error'])

    def test_broker_crash_reaps_descendants_not_only_direct_child(self):
        with self.controlled_deadlines():
            client = self.client('crash-tree')
            client.submit(state())
            child = self.wait(lambda: self.child(client))
            self.assertTrue(child['key_removed_before_descendants'])
            self.assertTrue(running(child['descendant']))
            # A delivered frame proves the whole tree is live before the crash.
            self.wait(client.poll)
            (self.project / ('crash-' + child['session'])).touch()
            self.wait(lambda: client.status['state'] == 'failed')
            self.wait(lambda: not running(child['pid']) and not running(child['descendant']))

    def test_sdk_change_on_shutdown_invalidates_the_session(self):
        client = self.client()
        client.submit(state())
        self.wait(client.poll)
        self.module.write_bytes(b'changed installed module')
        self.assertTrue(client.close(wait=True, timeout=5))
        self.assertEqual(client.status['state'], 'failed')
        self.assertIn('SDK file changed', client.status['error'])

    def test_restart_does_not_repin_a_replaced_manifest(self):
        client = self.client()
        client.submit(state())
        self.wait(client.poll)
        value = json.loads(self.manifest.read_text())
        value['identity_receipt_sha256'] = 'f'*64
        self.manifest.write_text(json.dumps(value))
        client.restart()
        self.wait(lambda: client.status['state'] == 'failed')
        self.assertIn('Installation manifest changed', client.status['error'])
        self.assertEqual(len(json.loads((self.project / 'starts.json').read_text())), 1)

    def test_two_clients_have_independent_sessions_and_lifecycles(self):
        first = self.client()
        first.submit(state())
        first_frame, _ = self.wait(first.poll)
        second = self.client()
        second.submit(state(2))
        second_frame, _ = self.wait(second.poll)
        self.assertNotEqual(first_frame['session'], second_frame['session'])
        self.assertNotEqual(first.status['session_directory'], second.status['session_directory'])
        self.assertTrue(first.close(wait=True, timeout=5))
        second.submit(state(3))
        header, _ = self.wait(second.poll)
        self.assertEqual(header['state']['camera_revision'], 3)

    def test_invalid_startup_paths_close_worker_and_refuse_new_submissions(self):
        client = ViewportClient(self.root / 'missing-python', self.broker, self.host, self.project, self.manifest)
        self.clients.append(client)
        self.wait(lambda: client.status['closed'])
        self.assertEqual(client.status['state'], 'failed')
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            client.submit(state())
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            client.restart()

    def test_extra_sdk_file_refuses_launch_before_any_child(self):
        (self.sdk / 'unexpected.dll').write_bytes(b'not sealed')
        client = self.client()
        self.wait(lambda: client.status['state'] == 'failed')
        self.assertIn('Unlisted SDK file', client.status['error'])
        self.assertFalse((self.project / 'starts.json').exists())

    def test_manifest_and_source_relabelling_are_refused(self):
        self.manifest.unlink()
        qualification = json.loads(self.qualification.read_text())
        qualification['source_commit'] = 'f'*40
        self.qualification.write_text(json.dumps(qualification))
        with self.assertRaisesRegex(ValueError, 'source identity mismatch'):
            installation.seal_installation(self.host, self.qualification, self.native_receipt, self.manifest)
        self.assertFalse(self.manifest.exists())

    def test_manifest_snapshot_cannot_mix_pinned_bytes_with_an_alternate_roster(self):
        original_bytes = self.manifest.read_bytes()
        original_host = self.host.read_bytes()
        pin = hashlib.sha256(original_bytes).hexdigest()
        alternate = json.loads(original_bytes)
        alternate_host = b'changed SDK executable bytes'
        alternate['files'][alternate['executable']] = hashlib.sha256(alternate_host).hexdigest()
        real_hash, calls = installation.file_hash, []
        def swap(path, *args, **kwargs):
            if Path(path) == self.manifest:
                if not calls:
                    digest = real_hash(path, *args, **kwargs)
                    calls.append(True)
                    self.host.write_bytes(alternate_host)
                    self.manifest.write_text(json.dumps(alternate))
                    return digest
                self.manifest.write_bytes(original_bytes)
            return real_hash(path, *args, **kwargs)
        try:
            with mock.patch.object(installation, 'file_hash', side_effect=swap):
                digest, parsed = installation.verify_installation(
                    self.manifest, self.host, time.monotonic()+2, expected_manifest=pin)
            self.assertEqual(digest, pin)
            self.assertEqual(parsed, json.loads(original_bytes))
        finally:
            self.manifest.write_bytes(original_bytes)
            self.host.write_bytes(original_host)

    def test_seal_cannot_join_one_host_receipt_content_to_another_receipt_hash(self):
        self.manifest.unlink()
        native_bytes = self.native_receipt.read_bytes()
        alternate = json.loads(native_bytes)
        alternate['source_commit'] = 'f'*40
        alternate_bytes = json.dumps(alternate).encode()
        qualification = json.loads(self.qualification.read_text())
        qualification['session_sha256'] = hashlib.sha256(alternate_bytes).hexdigest()
        self.qualification.write_text(json.dumps(qualification))
        real_read = Path.read_bytes
        def swap(path):
            raw = real_read(path)
            if path == self.native_receipt:
                path.write_bytes(alternate_bytes)
            return raw
        with mock.patch.object(Path, 'read_bytes', swap):
            with self.assertRaisesRegex(ValueError, 'Host receipt identity mismatch'):
                installation.seal_installation(self.host, self.qualification, self.native_receipt, self.manifest)
        self.assertFalse(self.manifest.exists())

    def test_seal_rejects_missing_shader_without_leaving_partial_manifest(self):
        self.manifest.unlink()
        (self.sdk / next(iter(installation.SHADERS))).unlink()
        with self.assertRaisesRegex(ValueError, 'roster incomplete'):
            installation.seal_installation(self.host, self.qualification, self.native_receipt, self.manifest)
        self.assertFalse(self.manifest.exists())

    def test_manifest_escape_and_linked_sdk_inputs_are_refused(self):
        value = json.loads(self.manifest.read_text())
        value['files']['../outside'] = 'a'*64
        self.manifest.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, 'relative path'):
            installation.verify_installation(self.manifest, self.host, time.monotonic()+2)
        self.manifest.unlink()
        target = self.root / 'external-module'
        self.module.rename(target)
        try:
            self.module.symlink_to(target)
        except OSError:
            self.skipTest('Symlink creation unavailable')
        with self.assertRaisesRegex(ValueError, 'Reparse'):
            installation.seal_installation(self.host, self.qualification, self.native_receipt, self.manifest)

    def test_no_state_submission_after_close_and_no_revision_rollback(self):
        client = self.client()
        client.submit(state(2))
        with self.assertRaisesRegex(ValueError, 'Revision rollback'):
            client.submit(state(1))
        client.close()
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            client.submit(state(3))

    def test_final_receipt_survives_cleanup_with_exact_bytes_and_independent_ownership(self):
        client = self.client('receipt')
        client.submit(state())
        self.wait(client.poll)
        root = Path(client.status['session_directory'])
        self.assertTrue(client.close(wait=True, timeout=5))
        self.assertFalse(root.exists())
        value = client.session_receipt()
        self.assertEqual(value['status'], 'complete')
        self.assertFalse(value['visual_approved'])
        self.assertEqual(hashlib.sha256(value['receipt_raw'].encode()).hexdigest(), value['receipt_sha256'])
        self.assertEqual(json.loads(value['receipt_raw']), value['receipt'])
        value['receipt']['source_commit'] = 'f'*40
        self.assertEqual(client.session_receipt()['receipt']['source_commit'], 'a'*40)

    def test_failure_receipt_never_becomes_a_success_receipt(self):
        client = self.client('failure-receipt')
        client.submit(state())
        self.wait(client.poll)
        self.assertTrue(client.close(wait=True, timeout=5))
        value = client.session_receipt()
        self.assertEqual(value['status'], 'failed')
        self.assertIsNone(value['receipt'])
        self.assertFalse(value['visual_approved'])
        self.assertEqual(hashlib.sha256(value['failure_raw'].encode()).hexdigest(), value['failure_sha256'])

    def test_oversized_host_receipt_is_bounded_and_refused(self):
        client = self.client('large-receipt')
        client.submit(state())
        self.wait(client.poll)
        self.assertTrue(client.close(wait=True, timeout=5))
        self.assertEqual(client.session_receipt()['status'], 'failed')
        self.assertIn('Host receipt size/file', client.status['error'])


if __name__ == '__main__':
    unittest.main()
