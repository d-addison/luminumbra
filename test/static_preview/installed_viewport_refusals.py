"""Real installed-host refusal probes. Each case owns a fresh process and restart.

Failure documents never substitute for successful session/frame receipts. Keys
are ephemeral startup inputs and are not written to evidence.
"""
import copy
import hashlib
import hmac
import io
import json
import os
import queue
import struct
import subprocess
import threading
import time

import protocol as wire


CASES = (
    ('bad_magic', 'Viewport record magic'),
    ('oversized_header', 'Viewport record size'),
    ('truncated_prefix', 'Viewport truncated pipe record'),
    ('bad_authentication', 'Viewport authentication'),
    ('duplicate_json', 'Viewport duplicate JSON member'),
    ('replayed_sequence', 'Viewport replay'),
    ('changed_session', 'Viewport changed session'),
    ('scene_rollback', 'Viewport revision rollback'),
    ('camera_rollback', 'Viewport revision rollback'),
    ('camera_without_revision', 'Viewport changed camera without revision'),
    ('scene_without_revision', 'Viewport changed scene without revision'),
    ('generation_without_barrier', 'Viewport generation barrier'),
    ('changed_manifest', 'Viewport changed immutable manifest'),
    ('missing_generation', 'Missing regular prefab member'),
    ('initial_wrong_manifest', 'Prefab manifest hash mismatch'),
)


def digest(value):
    return hashlib.sha256(value).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def packet(name, initial, key, missing):
    candidate = copy.deepcopy(initial)
    candidate['sequence'] = 2
    state = candidate['state']
    if name == 'bad_magic':
        return b'BAD!' + wire.encode(candidate, b'', key)[4:16]
    if name == 'oversized_header':
        return wire.PREFIX.pack(b'LVP1', wire.HEADER_LIMIT + 1, 0)
    if name == 'truncated_prefix':
        return b'LVP1'
    if name == 'bad_authentication':
        record = bytearray(wire.encode(candidate, b'', key))
        record[16] ^= 1
        return bytes(record)
    if name == 'duplicate_json':
        raw = b'{"kind":"state",' + wire.canonical(candidate)[1:]
        prefix = wire.PREFIX.pack(b'LVP1', len(raw), 0)
        return prefix + hmac.digest(key, prefix + raw, 'sha256') + raw
    if name == 'replayed_sequence':
        candidate['sequence'] = 1
    elif name == 'changed_session':
        candidate['session'] = '0' * 32 if initial['session'] != '0' * 32 else 'f' * 32
    elif name == 'scene_rollback':
        state['scene_revision'] = 1
    elif name == 'camera_rollback':
        state['camera_revision'] = 1
    elif name == 'camera_without_revision':
        state['view'][12] += .1
    elif name == 'scene_without_revision':
        state['locals'][0]['matrix'][12] += .1
    elif name in ('generation_without_barrier', 'missing_generation'):
        state['generation_id'] = missing
        if name == 'missing_generation':
            state['scene_revision'] = 3
    elif name in ('changed_manifest', 'initial_wrong_manifest'):
        state['manifest_sha256'] = '0' * 64 if state['manifest_sha256'] != '0' * 64 else 'f' * 64
        if name == 'initial_wrong_manifest':
            candidate['sequence'] = 1
    else:
        raise ValueError('Unknown refusal case')
    return wire.encode(candidate, b'', key)


class Session:
    def __init__(self, host, project, directory, environment, software):
        self.host, self.project, self.directory = host, project, directory
        self.environment, self.software = environment, software
        self.key, self.session = os.urandom(32), os.urandom(16).hex()
        self.process = self.reader = self.writer = self.log = self.timer = None
        self.responses = queue.Queue(maxsize=2)
        self.read_errors, self.write_errors = [], []
        self.eof = False
        self.expired = threading.Event()

    def __enter__(self):
        self.directory.mkdir(mode=0o700)
        self.log = (self.directory / 'host.log').open('wb')
        env = dict(self.environment)
        env.update(LUMINUMBRA_VIEWPORT_KEY=self.key.hex(), LUMINUMBRA_VIEWPORT_PROJECT=str(self.project))
        argv = [str(self.host), '--serve', '--output', str(self.directory / 'host-evidence')]
        if self.software:
            argv.append('--software-only')
        try:
            self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=self.log, env=env, cwd=self.directory, bufsize=0)
            def expire():
                self.expired.set()
                if self.process.poll() is None:
                    self.process.kill()
            self.timer = threading.Timer(75, expire)
            self.timer.start()
            def read():
                try:
                    with io.BufferedReader(self.process.stdout) as stream:
                        while stream.peek(1):
                            self.responses.put_nowait(wire.read_record(stream, self.key))
                        self.eof = True
                except Exception as error:
                    self.read_errors.append(type(error).__name__ + ': ' + str(error))
            self.reader = threading.Thread(target=read, daemon=True)
            self.reader.start()
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def send(self, record):
        def write_record():
            try:
                wire.write_record(self.process.stdin, record)
            except Exception as error:
                self.write_errors.append(str(error))
        self.writer = threading.Thread(target=write_record, daemon=True)
        self.writer.start()
        self.writer.join(timeout=65)
        wire.require(not self.writer.is_alive() and not self.write_errors, 'Bounded native probe write')

    def frame(self, state):
        self.send(wire.encode(state, b'', self.key))
        until = time.monotonic() + 65
        while True:
            try:
                header, payload = self.responses.get(timeout=.1)
                break
            except queue.Empty:
                wire.require(time.monotonic() < until and self.process.poll() is None and
                             not self.read_errors and not self.eof, 'Native probe frame deadline/disconnect')
        wire.match_frame(header, state)
        directory = self.directory / 'valid-frame'
        directory.mkdir()
        write(directory / 'request.json', state)
        write(directory / 'header.json', header)
        (directory / 'planes.bin').write_bytes(payload)
        wire.require(sum(payload[8 * (len(payload) // 9):]) > 0, 'Visible native probe frame')
        return header, payload

    def finish(self, code):
        self.process.stdin.close()
        wire.require(self.process.wait(timeout=65) == code and not self.expired.is_set(),
                     'Expected native host exit code without timeout')
        self.reader.join(timeout=5)
        wire.require(not self.reader.is_alive() and self.eof and not self.read_errors,
                     'Native host response stream ends cleanly')
        wire.require(self.responses.empty(), 'No frame accepted after the invalid request or stop')

    def __exit__(self, *_):
        if self.timer:
            self.timer.cancel()
        if self.process:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=5)
            for worker in (self.reader, self.writer):
                if worker and worker.ident is not None:
                    worker.join(timeout=2)
            self.process.stdin.close()
            self.process.stdout.close()
        if self.log:
            self.log.close()
        write(self.directory / 'process.json', {
            'session': self.session, 'pid': self.process.pid if self.process else None,
            'exit_code': self.process.returncode if self.process else None,
            'reaped': self.process is not None and self.process.poll() is not None,
            'timed_out': self.expired.is_set(), 'reader_errors': self.read_errors,
            'writer_errors': self.write_errors})


def initial_state(first, session):
    state = copy.deepcopy(first)
    state.update(session=session.session, sequence=1)
    state['state'].update(scene_revision=2, camera_revision=2)
    return state


def run_refusals(host, project, output, environment, first, software, identity):
    output.mkdir(mode=0o700)
    summary = {'passed': False, 'visual_approved': False, 'blender_qualified': False,
               'software_only': software, 'host_sha256': digest(host.read_bytes()),
               'source_commit': identity['source_commit'], 'source_dirty': identity['source_dirty'],
               'cases': []}
    missing = '0' * 32
    wire.require(not (project / '.luminumbra-author/generations' / missing).exists(),
                 'Fixed missing-generation negative control must be absent')
    try:
        for name, expected in CASES:
            directory = output / name
            directory.mkdir(mode=0o700)
            row = {'name': name, 'expected_refusal': expected, 'passed': False}
            summary['cases'].append(row)
            started = time.monotonic()
            warmup = None
            with Session(host, project, directory / 'refused', environment, software) as failed:
                baseline = initial_state(first, failed)
                if name != 'initial_wrong_manifest':
                    warmup = failed.frame(baseline)
                malformed = packet(name, baseline, failed.key, missing)
                (directory / 'rejected-request.bin').write_bytes(malformed)
                row.update(rejected_request_sha256=digest(malformed), refused_session=failed.session,
                           last_valid_frame_sha256=digest(warmup[1]) if warmup else None)
                failed.send(malformed)
                failed.finish(1)
                path = failed.directory / 'host-evidence/failure.json'
                failure = json.loads(path.read_bytes())
                wire.require(failure == {'status': 'refused', 'error': expected, 'visual_approved': False},
                             'Exact native refusal: ' + name)
                wire.require(not (failed.directory / 'host-evidence/session.json').exists(),
                             'Failed host must not have a successful session receipt')
                row['failure_receipt_sha256'] = digest(path.read_bytes())
            # The failed process is reaped before the fresh host is launched.
            with Session(host, project, directory / 'restart', environment, software) as restart:
                state = initial_state(first, restart)
                header, payload = restart.frame(state)
                restart.send(wire.encode({'kind': 'stop', 'session': restart.session, 'sequence': 2}, b'', restart.key))
                restart.finish(0)
                path = restart.directory / 'host-evidence/session.json'
                report = json.loads(path.read_bytes())
                wire.require(report['status'] == 'complete' and report['shutdown_complete'] and
                             report['frame_count'] == 1 and len(report['recent_frames']) == 1,
                             'Fresh restart completes exactly one frame')
                wire.require(not (restart.directory / 'host-evidence/failure.json').exists(),
                             'Successful restart must not carry failure evidence')
                for field in ('source_commit', 'source_dirty', 'source_input_sha256',
                              'executable_sha256', 'module_sha256', 'resources'):
                    wire.require(report[field] == identity[field], 'Restart installed identity: ' + field)
                actual = report['recent_frames'][0]
                wire.require(actual['wire_header'] == header and actual['planes_sha256'] == digest(payload),
                             'Restart frame/plane receipt join')
                for field in ('view', 'projection'):
                    expected_matrix = list(struct.unpack('<16f', struct.pack('<16f', *state['state'][field])))
                    wire.require(actual['actual_' + field] == expected_matrix, 'Restart actual matrix: ' + field)
                if software:
                    wire.require('llvmpipe' in actual['gpu']['renderer'], 'Restart actual software GPU')
                if warmup:
                    wire.require(payload == warmup[1], 'Restart reproduces the last valid frame bytes')
                row.update(restart_session=restart.session, restart_frame_sha256=digest(payload),
                           restart_receipt_sha256=digest(path.read_bytes()))
            row.update(passed=True, seconds=time.monotonic()-started)
            write(directory / 'acceptance.json', row)
        summary['passed'] = True
        return summary
    except Exception as error:
        summary['error'] = str(error)
        raise
    finally:
        write(output / 'acceptance.json', summary)
