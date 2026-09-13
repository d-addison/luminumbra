"""Portable real-process client tests; no renderer, Blender or GPU acceptance."""
import copy
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


def wait_for(function, timeout=5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = function()
        if result:
            return result
        time.sleep(.01)
    raise AssertionError('Timed out waiting for actual child-process result')


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
        # This trusted test wrapper uses the real Broker and real fixture child;
        # the dummy SDK is solely an installation-identity contract fixture.
        self.broker.write_text('''import hashlib,json,os,sys,threading,time
from pathlib import Path
sys.path.insert(0, WIRE_PATH)
from broker import Broker
config=json.loads(Path(sys.argv[2]).read_text())
project=Path(config['project_root'])
mode=(project/'mode.txt').read_text()
os.environ['CLIENT_FIXTURE_EVIDENCE']=str(Path(config['root'])/'host-evidence')
os.environ['CLIENT_FIXTURE_IDENTITY']=str(project/'fixture-identity.json')
starts=project/'starts.json'
rows=json.loads(starts.read_text()) if starts.exists() else []
rows.append({'session':config['session'],'root':config['root'],'pid':os.getpid()})
starts.write_text(json.dumps(rows))
if mode=='broker-hang': time.sleep(60)
if mode=='crash-once' and len(rows)==1: sys.exit(9)
if mode=='crash-tree':
    mode='descendant'
    threading.Timer(.7, lambda: os._exit(9)).start()
config['command']=[sys.executable, FIXTURE_PATH, WIRE_PATH, mode]
config['executable_sha256']=hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest()
Broker(key=bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY')), **config).run()
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

    def test_installed_client_returns_owned_current_frame_and_closes_tree(self):
        client = self.client()
        desired = state()
        sequence = client.submit(desired)
        desired['view'][12] = 999
        header, payload = wait_for(client.poll)
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
        with mock.patch.object(wire.Slots, 'read', blocked):
            client = self.client('delay')
            client.submit(state())
            self.assertTrue(entered.wait(3))
            for revision in range(2, 52):
                client.submit(state(revision))
                self.assertIsNone(client.poll())
            self.assertFalse(release.is_set())  # All callbacks returned while read was blocked.
            release.set()
            header, payload = wait_for(client.poll)
            self.assertEqual(header['state']['camera_revision'], 51)
            self.assertEqual(payload[0], 51)
            self.assertNotIn(threading.get_ident(), threads)

    def test_startup_timeout_and_retry_budget_use_fresh_private_sessions(self):
        client = self.client('broker-hang', startup_timeout=.3, shutdown_timeout=.1, max_restarts=1)
        client.submit(state())
        wait_for(lambda: client.status['state'] == 'failed', timeout=4)
        rows = json.loads((self.project / 'starts.json').read_text())
        self.assertEqual(len(rows), 2)
        self.assertEqual(len({row['session'] for row in rows}), 2)
        self.assertEqual(len({row['root'] for row in rows}), 2)
        self.assertEqual(client.status['restarts'], 1)
        self.assertIn('startup deadline', client.status['error'])
        self.assertTrue(all(not running(row['pid']) and not Path(row['root']).exists() for row in rows))

    def test_crash_recovery_replays_latest_state_in_a_fresh_session(self):
        client = self.client('crash-once', max_restarts=1)
        client.submit(state(3))
        header, _ = wait_for(client.poll)
        rows = json.loads((self.project / 'starts.json').read_text())
        self.assertEqual(len(rows), 2)
        self.assertNotEqual(rows[0]['session'], rows[1]['session'])
        self.assertEqual(header['session'], rows[1]['session'])
        self.assertEqual(header['state']['camera_revision'], 3)

    def test_explicit_restart_after_failure_is_new_session_without_repins(self):
        client = self.client('wrong')
        client.submit(state())
        wait_for(lambda: client.status['state'] == 'failed')
        previous = client.status['session']
        (self.project / 'mode.txt').write_text('echo')
        client.restart()
        header, _ = wait_for(client.poll)
        self.assertNotEqual(header['session'], previous)

    def test_frame_timeout_does_not_get_extended_by_flooding_desired_states(self):
        client = self.client('hang', frame_timeout=.35, startup_timeout=1, shutdown_timeout=.1)
        for revision in range(1, 50):
            client.submit(state(revision))
            if client.status['state'] == 'failed':
                break
            time.sleep(.02)
        wait_for(lambda: client.status['state'] == 'failed')
        self.assertIn('frame deadline', client.status['error'])
        self.assertIsNone(client.poll())

    def test_hung_shutdown_is_bounded_and_does_not_claim_cooperative_stop(self):
        client = self.client('stop-hang', shutdown_timeout=.15)
        client.submit(state())
        wait_for(client.poll)
        self.assertTrue(client.close(wait=True, timeout=4))
        self.assertEqual(client.status['state'], 'failed')
        self.assertIn('shutdown', client.status['error'])

    def test_broker_crash_reaps_descendants_not_only_direct_child(self):
        client = self.client('crash-tree')
        client.submit(state())
        wait_for(lambda: (self.project / 'child.json').exists())
        child = json.loads((self.project / 'child.json').read_text())
        self.assertTrue(child['key_removed_before_descendants'])
        self.assertTrue(running(child['descendant']))
        wait_for(lambda: client.status['state'] == 'failed')
        wait_for(lambda: not running(child['pid']) and not running(child['descendant']))

    def test_sdk_change_on_shutdown_invalidates_the_session(self):
        client = self.client()
        client.submit(state())
        wait_for(client.poll)
        self.module.write_bytes(b'changed installed module')
        self.assertTrue(client.close(wait=True, timeout=5))
        self.assertEqual(client.status['state'], 'failed')
        self.assertIn('SDK file changed', client.status['error'])

    def test_restart_does_not_repin_a_replaced_manifest(self):
        client = self.client()
        client.submit(state())
        wait_for(client.poll)
        value = json.loads(self.manifest.read_text())
        value['identity_receipt_sha256'] = 'f'*64
        self.manifest.write_text(json.dumps(value))
        client.restart()
        wait_for(lambda: client.status['state'] == 'failed')
        self.assertIn('Installation manifest changed', client.status['error'])
        self.assertEqual(len(json.loads((self.project / 'starts.json').read_text())), 1)

    def test_two_clients_have_independent_sessions_and_lifecycles(self):
        first = self.client()
        first.submit(state())
        first_frame, _ = wait_for(first.poll)
        second = self.client()
        second.submit(state(2))
        second_frame, _ = wait_for(second.poll)
        self.assertNotEqual(first_frame['session'], second_frame['session'])
        self.assertNotEqual(first.status['session_directory'], second.status['session_directory'])
        self.assertTrue(first.close(wait=True, timeout=5))
        second.submit(state(3))
        header, _ = wait_for(second.poll)
        self.assertEqual(header['state']['camera_revision'], 3)

    def test_invalid_startup_paths_close_worker_and_refuse_new_submissions(self):
        client = ViewportClient(self.root / 'missing-python', self.broker, self.host, self.project, self.manifest)
        self.clients.append(client)
        wait_for(lambda: client.status['closed'])
        self.assertEqual(client.status['state'], 'failed')
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            client.submit(state())
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            client.restart()

    def test_extra_sdk_file_refuses_launch_before_any_child(self):
        (self.sdk / 'unexpected.dll').write_bytes(b'not sealed')
        client = self.client()
        wait_for(lambda: client.status['state'] == 'failed')
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
        wait_for(client.poll)
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
        wait_for(client.poll)
        self.assertTrue(client.close(wait=True, timeout=5))
        value = client.session_receipt()
        self.assertEqual(value['status'], 'failed')
        self.assertIsNone(value['receipt'])
        self.assertFalse(value['visual_approved'])
        self.assertEqual(hashlib.sha256(value['failure_raw'].encode()).hexdigest(), value['failure_sha256'])

    def test_oversized_host_receipt_is_bounded_and_refused(self):
        client = self.client('large-receipt')
        client.submit(state())
        wait_for(client.poll)
        self.assertTrue(client.close(wait=True, timeout=5))
        self.assertEqual(client.session_receipt()['status'], 'failed')
        self.assertIn('Host receipt size/file', client.status['error'])


if __name__ == '__main__':
    unittest.main()
