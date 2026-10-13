import copy
from contextlib import contextmanager
import hashlib
import hmac
import io
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'tools/blender/authoring/viewport'
sys.path.insert(0, str(MODULE))
import protocol as p
from broker import Broker

KEY = bytes(range(32))
SESSION = 'a' * 32
MATRIX = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def state(sequence=1):
    return {'kind': 'state', 'session': SESSION, 'sequence': sequence, 'state': {
        'generation_id': 'b'*32, 'manifest_sha256': 'c'*64, 'scene_revision': 1,
        'camera_revision': 1, 'width': 1, 'height': 1, 'near_plane': 0.1,
        'far_plane': 3200., 'view': MATRIX.copy(), 'projection': MATRIX.copy(),
        'locals': [{'instance_id': 'asset.1', 'node_id': 'node.1', 'matrix': MATRIX.copy()}]}}


def frame(s=None):
    payload = bytes([1, 2, 3, 255]) + struct.pack('<f', .5) + b'\1'
    return {**(s or state()), 'kind': 'frame', 'planes_sha256': hashlib.sha256(payload).hexdigest()}, payload


def signed_raw(raw, payload=b''):
    prefix = p.PREFIX.pack(b'LVP1', len(raw), len(payload))
    return prefix + hmac.digest(KEY, prefix+raw+payload, 'sha256') + raw + payload


class Transport(unittest.TestCase):
    def test_record_reader_retains_old_complete_record_across_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'desired.bin'
            first, second = state(1), state(2)
            p.atomic_write(path, p.encode(first, b'', KEY))
            with p.open_record_reader(path) as old:
                p.atomic_write(path, p.encode(second, b'', KEY))
                self.assertEqual(p.read_record(old, KEY), (first, b''))
                self.assertFalse(old.read(1))
                self.assertEqual(p.read_record(io.BytesIO(p.read_record_file(path, p.CAPACITY)), KEY),
                                 (second, b''))
            self.assertFalse(list(Path(directory).glob('.pending-*')))

    def test_partial_read_write(self):
        class Reader(io.BytesIO):
            def read(self, count=-1):
                return super().read(min(count, 3))
        class Writer(io.BytesIO):
            def write(self, data):
                return super().write(data[:2])
        header, payload = frame()
        encoded = p.encode(header, payload, KEY)
        output = Writer()
        p.write_record(output, encoded)
        self.assertEqual(p.read_record(Reader(output.getvalue()), KEY), (header, payload))

    def test_mac_covers_header_and_raw(self):
        encoded = p.encode(*frame(), KEY)
        for index in (17, 50, len(encoded)-1):
            changed = bytearray(encoded); changed[index] ^= 1
            with self.assertRaises(p.Refusal):
                p.read_record(io.BytesIO(changed), KEY)

    def test_other_producer_number_format(self):
        raw = json.dumps(state()).replace('3200.0', '3.2e3').encode()
        self.assertEqual(p.read_record(io.BytesIO(signed_raw(raw)), KEY)[0], state())

    def test_duplicate_and_nonfinite(self):
        for raw in (b'{"kind":"state","kind":"stop"}', b'{"kind":NaN}'):
            with self.assertRaises(p.Refusal):
                p.read_record(io.BytesIO(signed_raw(raw)), KEY)

    def test_bounds_before_body_read(self):
        for h, body in ((p.HEADER_LIMIT+1, 0), (1, p.PAYLOAD_LIMIT+1)):
            with self.assertRaisesRegex(p.Refusal, 'bounds'):
                p.read_record(io.BytesIO(p.PREFIX.pack(b'LVP1', h, body)), KEY)

    def test_all_truncations(self):
        encoded = p.encode(state(), b'', KEY)
        for count in (0, 4, 15, 16, 47, len(encoded)-1):
            with self.assertRaises(p.Refusal):
                p.read_record(io.BytesIO(encoded[:count]), KEY)

    def test_plane_refusals(self):
        header, payload = frame()
        for bad in (payload[:-1], payload[:4]+struct.pack('<f', float('nan'))+payload[8:],
                    payload[:8]+b'\0', payload[:3]+b'\0'+payload[4:]):
            header['planes_sha256'] = hashlib.sha256(bad).hexdigest()
            with self.assertRaises(p.Refusal):
                p.encode(header, bad, KEY)

    def test_gate_replay_session_and_join(self):
        gate = p.StateGate(SESSION); first = state(); gate.accept(first)
        first['state']['locals'].clear()
        self.assertEqual(len(gate.last['state']['locals']), 1)
        for candidate in (state(), {**state(2), 'session': 'f'*32}):
            with self.assertRaises(p.Refusal): gate.accept(candidate)
        with self.assertRaises(p.Refusal): p.match_frame(frame(state(3))[0], state(2))

    def test_revision_and_generation(self):
        for mutate in (
            lambda s: s['state'].update(camera_revision=0),
            lambda s: s['state'].update(manifest_sha256='d'*64),
            lambda s: s['state'].update(generation_id='d'*32),
            lambda s: s['state']['view'].__setitem__(12, 2),
            lambda s: s['state']['locals'][0]['matrix'].__setitem__(12, 2),
        ):
            gate = p.StateGate(SESSION); gate.accept(state())
            second = state(2); mutate(second)
            with self.assertRaises(p.Refusal): gate.accept(second)
        gate = p.StateGate(SESSION); gate.accept(state())
        newer = state(4); newer['state'].update(generation_id='d'*32, scene_revision=2)
        gate.accept(newer)

    def test_stop_terminal(self):
        gate = p.StateGate(SESSION); gate.accept(state())
        gate.accept({'kind': 'stop', 'session': SESSION, 'sequence': 2})
        with self.assertRaises(p.Refusal): gate.accept(state(3))

    def test_generation_switch_cannot_reset_revisions_or_hide_camera_changes(self):
        first = state()
        first['state']['camera_revision'] = 10
        for changes in ({'camera_revision': 0}, {'scene_revision': 0},
                        {'view': [*MATRIX[:12], 3, *MATRIX[13:]]}):
            gate = p.StateGate(SESSION)
            gate.accept(first)
            newer = copy.deepcopy(first)
            newer['sequence'] = 2
            newer['state'].update(generation_id='d'*32, scene_revision=2)
            newer['state'].update(changes)
            with self.assertRaises(p.Refusal): gate.accept(newer)
            self.assertEqual(gate.last, first)
            self.assertEqual(len(gate.generations), 1)

    def test_coalesced_generation_roundtrip_preserves_admission(self):
        first = state()
        first['state']['camera_revision'] = 10
        middle = copy.deepcopy(first)
        middle['sequence'] = 2
        middle['state'].update(generation_id='d'*32, scene_revision=2, camera_revision=11)
        middle['state']['view'][12] = 1
        last = copy.deepcopy(first)
        last['sequence'] = 3
        last['state'].update(scene_revision=3, camera_revision=12)
        last['state']['view'][12] = 2
        last['state']['locals'][0]['matrix'][12] = 4
        complete = p.StateGate(SESSION)
        for item in (first, middle, last): complete.accept(item)
        coalesced = p.StateGate(SESSION)
        coalesced.accept(first)
        coalesced.accept(last)
        self.assertEqual(complete.last, coalesced.last)

    def test_revisited_generation_keeps_original_manifest(self):
        gate = p.StateGate(SESSION)
        gate.accept(state())
        middle = state(2)
        middle['state'].update(generation_id='d'*32, manifest_sha256='e'*64, scene_revision=2)
        gate.accept(middle)
        last = state(3)
        last['state'].update(manifest_sha256='f'*64, scene_revision=3)
        with self.assertRaisesRegex(p.Refusal, 'manifest changed'): gate.accept(last)
        self.assertEqual(gate.last, middle)
        last['state']['manifest_sha256'] = 'c'*64
        gate.accept(last)

    def test_generation_bound_allows_revisits_without_forgetting_pins(self):
        gate = p.StateGate(SESSION)
        for index in range(1, 65):
            item = state(index)
            item['state'].update(generation_id=f'{index:032x}', manifest_sha256=f'{index:064x}',
                                 scene_revision=index)
            gate.accept(item)
        overflow = state(65)
        overflow['state'].update(generation_id='f'*32, scene_revision=65)
        with self.assertRaisesRegex(p.Refusal, 'session bound'): gate.accept(overflow)
        self.assertEqual(gate.last['sequence'], 64)
        revisit = state(65)
        revisit['state'].update(generation_id=f'{1:032x}', manifest_sha256=f'{1:064x}', scene_revision=65)
        gate.accept(revisit)
        self.assertEqual(len(gate.generations), 64)

    def test_slot_leases_bounds_and_identity(self):
        with tempfile.TemporaryDirectory() as root:
            slots = p.Slots(root, KEY, SESSION, create=True)
            lease0 = slots._lease(0); lease1 = slots._lease(1)
            self.assertIsNone(slots.publish(*frame()))
            lease1.unlink()
            self.assertEqual(slots.publish(*frame()), 1)
            self.assertEqual(slots.read(1), frame())
            self.assertIsNone(slots.read(0))
            lease0.unlink()
            meta = Path(root)/'frame-1.json'
            meta.write_text('{"length":999999999,"sequence":1}')
            with self.assertRaises(p.Refusal): slots.read(1)

    def test_slot_mac_and_symlink(self):
        with tempfile.TemporaryDirectory() as root:
            slots = p.Slots(root, KEY, SESSION, create=True)
            slots.publish(*frame())
            file = Path(root)/'frame-0.bin'
            with file.open('r+b') as stream: stream.write(b'EVIL')
            with self.assertRaises(p.Refusal): slots.read(0)
            link = Path(root)/'link'; link.symlink_to(file)
            with self.assertRaises(p.Refusal): p.safe_path(link)

    def run_broker(self, mode, update=False, terminal_record=None):
        with tempfile.TemporaryDirectory() as root:
            executable = str(Path(sys.executable).resolve())
            command = [executable, '-B', str(Path(__file__).with_name('fixture_child.py')), str(MODULE), mode]
            if terminal_record is not None:
                # A child that sends a correct initial frame, then emits extra
                # bytes only after receiving the authenticated stop.
                script = '\n'.join([
                    'import os, sys',
                    'sys.path.insert(0, sys.argv[1])',
                    'from protocol import read_record, require',
                    "key = bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY'))",
                    'read_record(sys.stdin.buffer, key)',
                    f'sys.stdout.buffer.write(bytes.fromhex({p.encode(*frame(), KEY).hex()!r}))',
                    'sys.stdout.buffer.flush()',
                    "require(read_record(sys.stdin.buffer, key)[0]['kind'] == 'stop', 'Stop')",
                    f'sys.stdout.buffer.write(bytes.fromhex({terminal_record.hex()!r}))',
                    'sys.stdout.buffer.flush()',
                ])
                command = [executable, '-B', '-c', script, str(MODULE)]
            broker = Broker(root, KEY, SESSION,
                command,
                hashlib.sha256(Path(executable).read_bytes()).hexdigest(), root, deadline=.5)
            p.atomic_write(Path(root)/'desired.bin', p.encode(state(), b'', KEY))
            errors = []
            def run():
                try: broker.run()
                except Exception as error: errors.append(error)
            thread = threading.Thread(target=run); thread.start()
            if update:
                time.sleep(.05)
                newer = state(2); newer['state']['locals'][0]['matrix'][12] = 3
                newer['state']['scene_revision'] = 2
                p.atomic_write(Path(root)/'desired.bin', p.encode(newer, b'', KEY))
            end = time.monotonic()+3
            observed = None
            while time.monotonic() < end and thread.is_alive():
                for index in (0, 1):
                    value = broker.slots.read(index)
                    if value is not None: observed = value
                if observed:
                    p.atomic_write(Path(root)/'stop.bin', p.encode(
                        {'kind':'stop','session':SESSION,'sequence':99}, b'', KEY))
                    break
                time.sleep(.01)
            thread.join(timeout=4)
            self.assertFalse(thread.is_alive())
            self.assertIsNotNone(broker.child.poll())
            receipt = json.loads((Path(root)/'broker-result.json').read_bytes())
            self.assertTrue(receipt['child_reaped'])
            if receipt['status'] == 'stopped':
                self.assertTrue(broker.reader_eof)
                self.assertFalse(broker.errors)
                self.assertTrue(broker.responses.empty())
            return errors, observed, receipt

    def test_real_fixture_child_lifecycle(self):
        errors, value, receipt = self.run_broker('echo')
        self.assertFalse(errors); self.assertEqual(value, frame())
        self.assertEqual(receipt['published'], 1)
        self.assertEqual(receipt['status'], 'stopped')
        self.assertEqual(receipt['child_returncode'], 0)

    def test_stop_deadline_reaps_uncooperative_child(self):
        errors, value, receipt = self.run_broker('stop-hang')
        self.assertTrue(errors); self.assertIsNotNone(value)
        self.assertEqual(receipt['status'], 'failed')
        self.assertNotEqual(receipt['child_returncode'], 0)

    def test_thread_start_failure_reaps_started_child_and_records_failure(self):
        original_start = threading.Thread.start
        for failed_start in (1, 2):
            with self.subTest(failed_start=failed_start), tempfile.TemporaryDirectory() as root:
                executable = str(Path(sys.executable).resolve())
                broker = Broker(root, KEY, SESSION,
                    [executable, '-B', str(Path(__file__).with_name('fixture_child.py')), str(MODULE), 'echo'],
                    hashlib.sha256(Path(executable).read_bytes()).hexdigest(), root, deadline=.5)
                starts = 0
                def start(thread):
                    nonlocal starts
                    starts += 1
                    if starts == failed_start:
                        raise RuntimeError('injected reader startup failure')
                    return original_start(thread)
                with mock.patch.object(threading.Thread, 'start', start):
                    with self.assertRaisesRegex(RuntimeError, 'injected reader startup failure'):
                        broker.run()
                self.assertIsNotNone(broker.child.poll())
                for stream in (broker.child.stdin, broker.child.stdout, broker.child.stderr):
                    self.assertTrue(stream.closed)
                for thread in (broker.reader, broker.stderr_reader):
                    if thread is not None:
                        self.assertFalse(thread.is_alive())
                receipt = json.loads((Path(root)/'broker-result.json').read_bytes())
                self.assertEqual(receipt['status'], 'failed')
                self.assertTrue(receipt['child_reaped'])
                self.assertTrue((Path(root)/'host-stderr.log').is_file())

    def test_stop_joins_reader_before_accepting_terminal_responses(self):
        malformed_mac = bytearray(p.encode(*frame(), KEY))
        malformed_mac[-1] ^= 1
        for terminal in (p.encode(*frame(), KEY), b'LVP1', bytes(malformed_mac)):
            with self.subTest(terminal=terminal[:16]):
                records = 0
                def delayed_read(stream, key):
                    nonlocal records
                    # Shared mailbox readers also own integer descriptors.
                    # Count the actual response pipe, not the stream's label.
                    if stat.S_ISFIFO(os.fstat(stream.fileno()).st_mode):
                        records += 1
                        if records == 2:
                            # Let the child exit while terminal bytes are still
                            # being processed; polling before join races.
                            time.sleep(.15)
                    return p.read_record(stream, key)
                with mock.patch('broker.read_record', side_effect=delayed_read):
                    errors, value, receipt = self.run_broker('terminal', terminal_record=terminal)
                self.assertEqual(records, 2)
                self.assertTrue(errors)
                self.assertIsNotNone(value)
                self.assertEqual(receipt['status'], 'failed')
                self.assertEqual(receipt['child_returncode'], 0)

    def test_latest_complete_state_coalesces(self):
        errors, value, receipt = self.run_broker('delay', update=True)
        self.assertFalse(errors); self.assertEqual(value[0]['sequence'], 2)
        self.assertEqual(value[0]['state']['locals'][0]['matrix'][12], 3)
        self.assertGreaterEqual(receipt['dropped'], 1)

    def wait_for(self, condition, timeout=2):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if condition():
                return
            time.sleep(.005)
        self.assertTrue(condition(), 'Timed out waiting for fixture condition')

    @contextmanager
    def contended_broker(self, deadline=.8, mode='echo'):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = str(Path(sys.executable).resolve())
            broker = Broker(root, KEY, SESSION, [executable, '-B',
                str(Path(__file__).with_name('fixture_child.py')), str(MODULE), mode],
                hashlib.sha256(Path(executable).read_bytes()).hexdigest(), root, deadline=deadline)
            leases = [broker.slots._lease(index) for index in (0, 1)]
            self.assertTrue(all(leases))
            fixture = SimpleNamespace(root=root, broker=broker, leases=leases,
                                      attempts=[], errors=[], observed=None)
            publish = broker.slots.publish
            def observe_publish(header, payload):
                index = publish(header, payload)
                # Observe actual leased-file publication, without mocking lease
                # failure or synthesizing the child's response.
                fixture.attempts.append((header['sequence'], index))
                return index
            def run():
                try:
                    broker.run()
                except Exception as error:
                    fixture.errors.append(error)
            p.atomic_write(root / 'desired.bin', p.encode(state(), b'', KEY))
            with mock.patch.object(broker.slots, 'publish', side_effect=observe_publish):
                fixture.thread = threading.Thread(target=run)
                fixture.thread.start()
                try:
                    self.wait_for(lambda: fixture.attempts)
                    self.assertEqual(fixture.attempts[0], (1, None))
                    yield fixture
                finally:
                    if fixture.thread.is_alive():
                        p.atomic_write(root / 'stop.bin', p.encode(
                            {'kind': 'stop', 'session': SESSION, 'sequence': 9999}, b'', KEY))
                    fixture.thread.join(timeout=4)
                    self.assertFalse(fixture.thread.is_alive())
                    fixture.receipt = json.loads((root / 'broker-result.json').read_bytes())
                    self.assertTrue(fixture.receipt['child_reaped'])
                    for lease in leases:
                        lease.unlink(missing_ok=True)

    def release_and_observe(self, fixture):
        for lease in fixture.leases:
            lease.unlink()
        def read():
            for index in (0, 1):
                value = fixture.broker.slots.read(index)
                if value is not None:
                    fixture.observed = value
                    return True
            return False
        self.wait_for(read)

    def test_completed_frame_retries_after_slot_release_without_new_state(self):
        with self.contended_broker() as fixture:
            self.release_and_observe(fixture)
            self.assertEqual(fixture.observed, frame())
            time.sleep(.04)
            self.assertEqual([seq for seq, index in fixture.attempts if index is not None], [1])
        self.assertFalse(fixture.errors)
        self.assertEqual(fixture.receipt['status'], 'stopped')
        self.assertEqual(fixture.receipt['published'], 1)
        self.assertEqual(fixture.receipt['dropped'], 0)

    def test_permanent_slot_leases_fail_with_original_deadline_and_reap_child(self):
        with self.contended_broker(deadline=.3) as fixture:
            fixture.thread.join(timeout=1.5)
            self.assertFalse(fixture.thread.is_alive(), 'Permanent leases must not leave broker idle forever')
        self.assertEqual(len(fixture.errors), 1)
        self.assertRegex(str(fixture.errors[0]), 'publication deadline')
        self.assertEqual(fixture.receipt['status'], 'failed')
        self.assertEqual(fixture.receipt['published'], 0)

    def test_newer_state_supersedes_completed_frame_while_slots_are_leased(self):
        with self.contended_broker() as fixture:
            newer = state(2)
            newer['state']['scene_revision'] = 2
            newer['state']['locals'][0]['matrix'][12] = 3
            p.atomic_write(fixture.root / 'desired.bin', p.encode(newer, b'', KEY))
            self.wait_for(lambda: (2, None) in fixture.attempts)
            self.release_and_observe(fixture)
            self.assertEqual(fixture.observed, frame(newer))
            self.assertEqual([seq for seq, index in fixture.attempts if index is not None], [2])
        self.assertFalse(fixture.errors)
        self.assertEqual(fixture.receipt['published'], 1)
        self.assertEqual(fixture.receipt['dropped'], 1)

    def test_stop_discards_completed_frame_without_waiting_for_leases(self):
        with self.contended_broker() as fixture:
            p.atomic_write(fixture.root / 'stop.bin', p.encode(
                {'kind': 'stop', 'session': SESSION, 'sequence': 2}, b'', KEY))
            fixture.thread.join(timeout=2)
            self.assertFalse(fixture.thread.is_alive())
            self.assertTrue(all(lease.exists() for lease in fixture.leases))
            self.assertFalse(list(fixture.root.glob('frame-*.json')))
        self.assertFalse(fixture.errors)
        self.assertEqual(fixture.receipt['status'], 'stopped')
        self.assertEqual(fixture.receipt['published'], 0)
        self.assertEqual(fixture.receipt['dropped'], 1)
        self.assertTrue(fixture.broker.reader_eof)

    def test_desired_flood_does_not_extend_blocked_publication_deadline(self):
        with self.contended_broker(deadline=.4) as fixture:
            started = time.monotonic()
            sequence = 1
            while fixture.thread.is_alive() and time.monotonic() - started < 1:
                sequence += 1
                newer = state(sequence)
                newer['state']['scene_revision'] = sequence
                newer['state']['locals'][0]['matrix'][12] = sequence
                p.atomic_write(fixture.root / 'desired.bin', p.encode(newer, b'', KEY))
                time.sleep(.025)
            self.assertFalse(fixture.thread.is_alive(), 'Desired updates must not renew delivery deadline')
            self.assertLess(time.monotonic() - started, .8)
            self.assertGreater(max(seq for seq, _ in fixture.attempts), 1)
        self.assertEqual(len(fixture.errors), 1)
        self.assertRegex(str(fixture.errors[0]), 'publication deadline')
        self.assertEqual(fixture.receipt['status'], 'failed')
        self.assertEqual(fixture.receipt['published'], 0)

    def test_stop_after_supersession_waits_for_inflight_response(self):
        with self.contended_broker(mode='delay') as fixture:
            p.atomic_write(fixture.root / 'desired.bin', p.encode(state(2), b'', KEY))
            self.wait_for(lambda: fixture.broker.gate.last['sequence'] == 2)
            p.atomic_write(fixture.root / 'stop.bin', p.encode(
                {'kind': 'stop', 'session': SESSION, 'sequence': 3}, b'', KEY))
            fixture.thread.join(timeout=2)
            self.assertFalse(fixture.thread.is_alive())
            self.assertTrue(all(lease.exists() for lease in fixture.leases))
        self.assertFalse(fixture.errors)
        self.assertEqual(fixture.receipt['status'], 'stopped')
        self.assertEqual(fixture.receipt['child_returncode'], 0)
        self.assertEqual(fixture.receipt['published'], 0)
        self.assertEqual(fixture.receipt['dropped'], 2)
        self.assertTrue(fixture.broker.reader_eof)
        self.assertTrue(fixture.broker.responses.empty())

    def test_timeout_reaps_child(self):
        errors, value, _ = self.run_broker('hang')
        self.assertTrue(errors); self.assertIsNone(value)

    def test_wrong_child_frame_refused(self):
        errors, value, _ = self.run_broker('wrong')
        self.assertTrue(errors); self.assertIsNone(value)


if __name__ == '__main__':
    unittest.main()
