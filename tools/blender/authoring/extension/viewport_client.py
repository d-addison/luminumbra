"""Asynchronous installed viewport client; draw callbacks only submit/poll."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import threading
import time
import uuid

from .viewport import protocol as wire
from .viewport_installation import SHADERS, file_hash, renderer_module, verify_installation
from .viewport_process import OwnedProcess, private_directory


class ViewportClient:
    def __init__(self, python, broker_script, host, project, host_manifest, *,
                 startup_timeout=10.0, frame_timeout=60.0, shutdown_timeout=5.0, max_restarts=1):
        for value in (startup_timeout, frame_timeout, shutdown_timeout):
            wire.require(type(value) in (int, float) and 0 < value <= 60, 'Client deadline')
        wire.require(type(max_restarts) is int and 0 <= max_restarts <= 3, 'Restart bound')
        # Path resolution, file reads and process creation belong to the worker.
        self.python, self.broker_script, self.host = python, broker_script, host
        self.project, self.host_manifest = project, host_manifest
        self.startup_timeout, self.frame_timeout = startup_timeout, frame_timeout
        self.shutdown_timeout, self.max_restarts = shutdown_timeout, max_restarts
        self._lock = threading.Lock()
        self._wake, self._closing = threading.Event(), threading.Event()
        self._restart_requested = False
        self._sequence, self._restarts = 0, 0
        self._session = uuid.uuid4().hex
        self._gate = wire.StateGate(self._session)
        self._desired = self._frame = None
        self._state, self._error, self._closed = 'starting', None, False
        self._root = self._broker_pid = None
        self._last_result = None
        self._session_receipt = None
        self._worker = threading.Thread(target=self._run, name='Luminumbra viewport client', daemon=True)
        self._worker.start()

    def submit(self, state):
        """Snapshot a bounded state; no filesystem, pipe, process or plane work."""
        wire.validate_state(state)
        snapshot = json.loads(wire.canonical(state))
        with self._lock:
            if self._closing.is_set() or self._closed:
                raise RuntimeError('Viewport client closed')
            header = {'kind': 'state', 'session': self._session,
                      'sequence': self._sequence + 1, 'state': snapshot}
            admitted = self._gate.accept(header)
            self._sequence += 1
            self._desired = admitted  # One replaceable latest state, no backlog.
            self._frame = None
        self._wake.set()
        return header['sequence']

    def poll(self):
        """Consume only a completed frame matching the latest submitted state."""
        with self._lock:
            value, self._frame = self._frame, None
            if value is not None and self._desired is not None:
                if all(value[0][key] == self._desired[key] for key in ('session', 'sequence', 'state')):
                    return value
            return None

    @property
    def status(self):
        with self._lock:
            return {'state': self._state, 'error': self._error, 'session': self._session,
                    'sequence': self._sequence, 'restarts': self._restarts, 'closed': self._closed,
                    'session_directory': str(self._root) if self._root else None,
                    'broker_pid': self._broker_pid,
                    'last_result': dict(self._last_result) if self._last_result is not None else None}

    def session_receipt(self):
        """Inspect retained final host evidence outside the per-draw status path.

        Exact UTF-8 receipt text and its raw-byte SHA survive directory cleanup.
        The returned structure is independently owned; no visual approval is made.
        """
        with self._lock:
            return copy.deepcopy(self._session_receipt)

    def restart(self):
        """Explicit fresh-session recovery; never silently repin a changed SDK."""
        with self._lock:
            if self._closing.is_set() or self._closed:
                raise RuntimeError('Viewport client closed')
            self._restart_requested = True
            self._frame = None
            self._session_receipt = None
        self._wake.set()

    def close(self, wait=False, timeout=None):
        self._closing.set()
        self._wake.set()
        if wait and self._worker is not threading.current_thread():
            limit = self.startup_timeout + self.shutdown_timeout + 4 if timeout is None else timeout
            if type(limit) not in (int, float) or not 0 <= limit <= 124:
                raise ValueError('Close wait bound')
            self._worker.join(limit)
        return not self._worker.is_alive()

    def _update(self, state, error=None):
        with self._lock:
            self._state, self._error = state, str(error)[:512] if error is not None else None
            if error is not None:
                self._frame = None

    def _new_session(self):
        with self._lock:
            self._session = uuid.uuid4().hex
            self._gate = wire.StateGate(self._session)
            self._frame = None
            self._session_receipt = None
            if self._desired:
                self._desired = self._gate.accept({**self._desired, 'session': self._session})
            return self._session

    def _stop(self, process, root, key, session):
        if process is None:
            return
        if process.poll() is None:
            with self._lock:
                stop = {'kind': 'stop', 'session': session, 'sequence': self._sequence + 1}
            wire.atomic_write(root / 'stop.bin', wire.encode(stop, b'', key))
            until = time.monotonic() + self.shutdown_timeout
            while process.poll() is None and time.monotonic() < until:
                time.sleep(0.01)
        process.close()  # Always close the group/job, even after broker exit.

    def _retain_receipt(self, root, session):
        value = {'session': session, 'status': 'unavailable', 'receipt': None, 'receipt_raw': None,
                 'receipt_sha256': None, 'failure': None, 'failure_raw': None,
                 'failure_sha256': None, 'error': None, 'visual_approved': False}
        try:
            for stem, name in (('receipt', 'session.json'), ('failure', 'failure.json')):
                path = wire.safe_path(root / 'host-evidence' / name)
                if not path.exists():
                    continue
                wire.require(path.is_file() and path.stat().st_size <= 1024 * 1024, 'Host receipt size/file')
                with path.open('rb') as stream:
                    raw = stream.read(1024 * 1024 + 1)
                wire.require(len(raw) <= 1024 * 1024, 'Host receipt read bound')
                value[stem + '_sha256'] = hashlib.sha256(raw).hexdigest()
                value[stem + '_raw'] = raw.decode('utf-8')
                value[stem] = json.loads(raw)
                wire.require(type(value[stem]) is dict, 'Host receipt object')
            receipt = value['receipt']
            if value['failure'] is not None:
                value['status'] = 'failed'
                raise RuntimeError('Viewport host reported failure')
            elif receipt is not None:
                wire.require(receipt.get('schema') == 'luminumbra.viewport.session.v1'
                             and receipt.get('status') == 'complete'
                             and receipt.get('shutdown_complete') is True
                             and receipt.get('visual_approved') is False, 'Host session did not complete')
                for name in ('source_commit', 'source_dirty', 'source_input_sha256'):
                    wire.require(receipt.get(name) == self._installation[name], 'Host receipt source identity')
                wire.require(receipt.get('executable_sha256') ==
                             self._installation['files'][self._installation['executable']],
                             'Host receipt executable identity')
                files = self._installation['files']
                wire.require(receipt.get('module_sha256') == files[renderer_module(files)],
                             'Host receipt module identity')
                wire.require(receipt.get('resources') == {Path(name).name: files[name] for name in SHADERS},
                             'Host receipt shader identity')
                value['status'] = 'complete'
        except Exception as error:
            value['status'], value['error'] = 'failed', str(error)[:512]
            raise
        finally:
            with self._lock:
                self._session_receipt = value

    def _session_loop(self, python, broker, host, project, root, key, session):
        config = {'root': str(root), 'session': session,
                  'command': [str(host), '--serve', '--output', str(root / 'host-evidence')],
                  'executable_sha256': self._installation['files'][self._installation['executable']],
                  'project_root': str(project), 'deadline': self.frame_timeout}
        wire.atomic_write(root / 'config.json', wire.canonical(config))
        env = os.environ.copy()
        env['LUMINUMBRA_VIEWPORT_KEY'] = key.hex()
        env['PYTHONDONTWRITEBYTECODE'] = '1'
        process = None
        error = None
        try:
            process = OwnedProcess([str(python), '-E', '-s', '-B', str(broker), '--config', str(root / 'config.json')],
                                   env, root)
            with self._lock:
                self._broker_pid = process.pid
            slots = wire.Slots(root, key, session)
            started, sent, received, outstanding = time.monotonic(), 0, 0, None
            while not self._closing.is_set():
                with self._lock:
                    desired, restart = self._desired, self._restart_requested
                if restart:
                    break
                code = process.poll()
                if code is not None:
                    raise RuntimeError('Viewport broker exited: ' + str(code))
                ready = all((root / f'frame-{index}.bin').exists() for index in range(2))
                if not ready:
                    if time.monotonic() - started >= self.startup_timeout:
                        raise TimeoutError('Viewport broker startup deadline')
                elif desired is not None and desired['sequence'] > sent:
                    wire.atomic_write(root / 'desired.bin', wire.encode(desired, b'', key))
                    sent = desired['sequence']
                    if outstanding is None:
                        outstanding = time.monotonic()
                    self._update('rendering')
                elif ready and desired is None:
                    self._update('ready')
                if ready:
                    # Read at most one full candidate at a time. No payload queue;
                    # the broker owns exactly two leased slots and this owns one
                    # completed current frame until poll consumes it.
                    for index in range(2):
                        descriptor = wire.safe_path(root / f'frame-{index}.json')
                        if not descriptor.exists():
                            continue
                        meta = json.loads(wire.read_record_file(descriptor, 128))
                        wire.require(type(meta) is dict and set(meta) == {'sequence', 'length'}
                                     and wire.integer(meta['sequence'], 1, 2**53 - 1)
                                     and wire.integer(meta['length'], 49, wire.CAPACITY), 'Frame descriptor')
                        with self._lock:
                            newest = self._desired['sequence'] if self._desired else 0
                        if meta['sequence'] <= received or meta['sequence'] < newest:
                            continue
                        frame = slots.read(index)
                        if frame is None:
                            continue
                        with self._lock:
                            current = self._desired
                            wire.require(current is not None and frame[0]['sequence'] <= current['sequence'],
                                         'Unsolicited future viewport frame')
                            if frame[0]['sequence'] == current['sequence']:
                                wire.match_frame(frame[0], current)
                                self._frame = frame
                                self._state, self._error = 'ready', None
                                received = frame[0]['sequence']
                                outstanding = None
                        del frame
                if outstanding is not None:
                    limit = self.frame_timeout if received else min(self.startup_timeout, self.frame_timeout)
                    if time.monotonic() - outstanding >= limit:
                        raise TimeoutError('Viewport frame deadline')
                self._wake.wait(0.01)
                self._wake.clear()
        except Exception as failure:
            error = failure
        finally:
            try:
                self._stop(process, root, key, session)
            except Exception as failure:
                error = error or failure
                if process is not None:
                    process.close()
            result = root / 'broker-result.json'
            if result.exists() and result.stat().st_size <= 4096:
                try:
                    value = json.loads(result.read_bytes())
                    wire.require(type(value) is dict, 'Broker shutdown receipt object')
                    with self._lock:
                        self._last_result = value
                    if value.get('status') != 'stopped' or value.get('child_reaped') is not True:
                        error = error or RuntimeError('Viewport host shutdown was incomplete')
                except (OSError, ValueError) as failure:
                    error = error or failure
            else:
                error = error or RuntimeError('Viewport broker shutdown receipt missing')
            try:
                self._retain_receipt(root, session)
            except Exception as failure:
                error = error or failure
        if error is not None:
            raise error

    def _run(self):
        baseline, dependencies = None, None
        try:
            python = Path(self.python).expanduser().resolve(strict=True)
            broker, host, project, manifest = [wire.safe_path(path) for path in
                (self.broker_script, self.host, self.project, self.host_manifest)]
            wire.require(project.is_dir(), 'Viewport project directory')
            pins = (python, broker, broker.parent / 'protocol.py')
            while not self._closing.is_set():
                # Consume this attempt's explicit request even if preflight
                # refuses it. A rejected pin must not create a busy retry loop.
                with self._lock:
                    self._restart_requested = False
                root = None
                error = None
                try:
                    until = time.monotonic() + self.startup_timeout
                    baseline, self._installation = verify_installation(manifest, host, until,
                        expected_manifest=baseline, cancelled=self._closing)
                    actual = {str(path): file_hash(path, until, self._closing) for path in pins}
                    wire.require(dependencies is None or actual == dependencies, 'Viewport broker/interpreter changed')
                    dependencies = actual
                    root = wire.safe_path(private_directory())
                    key = os.urandom(32)
                    with self._lock:
                        self._root, self._broker_pid = root, None
                        self._restart_requested = False
                    session = self._new_session()
                    self._update('starting')
                    self._session_loop(python, broker, host, project, root, key, session)
                except InterruptedError as failure:
                    if not self._closing.is_set():
                        error = failure
                except Exception as failure:
                    error = failure
                finally:
                    if baseline is not None:
                        try:
                            until = time.monotonic() + self.startup_timeout
                            verify_installation(manifest, host, until, expected_manifest=baseline)
                            if dependencies is not None:
                                wire.require({str(path): file_hash(path, until) for path in pins} == dependencies,
                                             'Viewport broker/interpreter changed')
                        except Exception as failure:
                            error = failure
                    if root is not None:
                        try:
                            shutil.rmtree(wire.safe_path(root))
                        except OSError as failure:
                            error = error or failure
                    with self._lock:
                        self._root, self._broker_pid = None, None
                        if error is not None and self._session_receipt is not None:
                            self._session_receipt['status'] = 'failed'
                            self._session_receipt['error'] = str(error)[:512]
                if self._closing.is_set():
                    if error is not None:
                        self._update('failed', error)
                    break
                with self._lock:
                    explicit = self._restart_requested
                    if explicit:
                        self._restarts = 0
                    elif error is not None and self._restarts < self.max_restarts:
                        self._restarts += 1
                        explicit = True
                if explicit:
                    self._update('restarting', error)
                    continue
                self._update('failed', error or 'Viewport session ended')
                while not self._closing.is_set():
                    self._wake.wait(0.05)
                    self._wake.clear()
                    with self._lock:
                        if self._restart_requested:
                            self._restarts = 0
                            break
        except Exception as error:
            self._update('failed', error)
        finally:
            with self._lock:
                self._closed, self._frame = True, None
                if self._closing.is_set() and self._error is None:
                    self._state = 'closed'
