"""External-only viewport broker. Experimental; fixture child is not a renderer."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

from protocol import (CAPACITY, Refusal, Slots, StateGate, atomic_write, canonical,
                      encode, match_frame, open_record_reader, read_record, require, safe_path,
                      validation_backend, write_record)


def mailbox(path, key):
    path = safe_path(path)
    if not path.exists():
        return None
    with open_record_reader(path) as stream:
        require(os.fstat(stream.fileno()).st_size <= CAPACITY, 'Mailbox size')
        header, payload = read_record(stream, key)
        require(not stream.read(1), 'Trailing mailbox bytes')
    require(not payload, 'Mailbox payload')
    return header


class Broker:
    def __init__(self, root, key, session, command, executable_sha256, project_root,
                 deadline=60.0):
        self.root, self.key, self.session = safe_path(root), key, session
        self.project_root = safe_path(project_root)
        require(self.project_root.is_dir(), 'Project root')
        require(command and Path(command[0]).is_absolute(), 'Absolute executable')
        executable = safe_path(command[0])
        require(hashlib.sha256(executable.read_bytes()).hexdigest() == executable_sha256, 'Executable pin')
        require(0 < deadline <= 60, 'Deadline')
        self.command, self.deadline = command, deadline
        self.gate = StateGate(session)
        self.slots = Slots(root, key, session, create=True)
        self.child = None
        self.responses = queue.Queue(maxsize=2)
        self.reader = None
        self.stderr_reader = None
        self.writer = None
        self.reader_eof = False
        self.errors = []
        self.stderr_tail = bytearray()

    def _stderr(self):
        # Drain continuously so diagnostics cannot deadlock the renderer. Retain
        # only the final MiB, including failures before its first frame.
        while True:
            chunk = self.child.stderr.read(8192)
            if not chunk:
                return
            self.stderr_tail.extend(chunk)
            del self.stderr_tail[:-1024 * 1024]

    def _send(self, header):
        record = encode(header, b'', self.key)
        self.writer_error = []

        def send():
            try:
                write_record(self.child.stdin, record)
            except (OSError, ValueError) as error:
                self.writer_error.append((type(error).__name__ + ': ' + str(error))[:512])

        self.writer = threading.Thread(target=send, daemon=True)
        self.writer.start()

    def _finish_write(self, timeout):
        self.writer.join(timeout=timeout)
        require(not self.writer.is_alive() and not self.writer_error, 'Incomplete state write')

    def _read(self):
        try:
            # Inspect EOF only at record boundaries. An empty stream is normal
            # after stop; a partial prefix/body remains a protocol failure.
            with io.BufferedReader(self.child.stdout) as stream:
                while stream.peek(1):
                    value = read_record(stream, self.key)
                    self.responses.put_nowait(value)
                self.reader_eof = True
        except (OSError, ValueError, queue.Full) as error:
            # An unbounded unsolicited producer cannot block this thread forever.
            self.errors.append((type(error).__name__ + ': ' + str(error))[:512])

    def _new(self, filename):
        header = mailbox(self.root / filename, self.key)
        if header is None:
            return None
        if self.gate.last and header['sequence'] == self.gate.last['sequence']:
            require(header == self.gate.last, 'Sequence reused with changed state')
            return None
        return self.gate.accept(header)

    def run(self):
        env = os.environ.copy()
        env['LUMINUMBRA_VIEWPORT_KEY'] = self.key.hex()
        env['LUMINUMBRA_VIEWPORT_PROJECT'] = str(self.project_root)
        pending = active = completed = None
        shutdown = None
        started = outstanding = None
        published = dropped = 0
        stopped = False
        try:
            # Own the process before starting either reader: Thread.start may
            # fail after Popen succeeds and must still reap the child.
            self.child = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, env=env, bufsize=0, shell=False)
            self.reader = threading.Thread(target=self._read, daemon=True)
            self.reader.start()
            self.stderr_reader = threading.Thread(target=self._stderr, daemon=True)
            self.stderr_reader.start()
            while True:
                stop = self.root / 'stop.bin'
                if shutdown is None and stop.exists():
                    shutdown = self._new('stop.bin')
                    require(shutdown is not None and shutdown['kind'] == 'stop', 'Stop kind')
                    pending = None
                    if completed is not None:
                        completed = None
                        dropped += 1
                if shutdown is not None and active is None:
                    # Complete any in-flight request before sending stop on the
                    # same pipe. A clean stop lets the host persist its receipt.
                    self._send(shutdown)
                    self._finish_write(min(self.deadline, 5))
                    self.child.stdin.close()
                    require(self.child.wait(timeout=min(self.deadline, 5)) == 0,
                            'Host shutdown failed')
                    self.reader.join(timeout=min(self.deadline, 5))
                    require(not self.reader.is_alive() and self.reader_eof and not self.errors,
                            'Host shutdown response invalid or incomplete')
                    require(self.responses.empty(), 'Unsolicited shutdown frame')
                    stopped = True
                    break
                desired = self._new('desired.bin') if shutdown is None else None
                if desired:
                    require(desired['kind'] == 'state', 'Desired kind')
                    pending = desired
                    if completed is not None:
                        completed = None
                        dropped += 1
                if shutdown is None and outstanding is not None:
                    # One delivery deadline survives coalescing and lease retries.
                    # A desired-state flood cannot keep an unpublished session alive.
                    require(time.monotonic() - outstanding < self.deadline,
                            'Frame publication deadline')
                if active is None and pending is not None:
                    active, pending = pending, None
                    # Small writes happen in a separate thread with the same render deadline.
                    started = time.monotonic()
                    if outstanding is None:
                        outstanding = started
                    self._send(active)
                try:
                    frame, payload = self.responses.get_nowait()
                except queue.Empty:
                    frame = None
                if frame is not None:
                    require(active is not None, 'Unsolicited frame')
                    match_frame(frame, active)
                    if pending is None and shutdown is None:
                        completed = (frame, payload)
                    else:
                        dropped += 1
                    self._finish_write(0.1)
                    active = None
                    frame = payload = None
                if completed is not None:
                    # Both reader leases may be held briefly. Keep exactly this
                    # latest completed frame until publication or supersession.
                    require(time.monotonic() - outstanding < self.deadline,
                            'Frame publication deadline')
                    if self.slots.publish(*completed) is not None:
                        published += 1
                        completed = outstanding = None
                require(not self.errors and self.child.poll() is None, 'Host disconnected or invalid response')
                if active:
                    require(not self.writer_error and time.monotonic() - started < self.deadline, 'Host deadline/write')
                time.sleep(0.01)
        finally:
            if self.child is not None:
                if self.child.poll() is None:
                    self.child.terminate()
                    try:
                        self.child.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        self.child.kill()
                        self.child.wait(timeout=2)
                for thread in (self.reader, self.stderr_reader, self.writer):
                    if thread is not None and thread.ident is not None:
                        thread.join(timeout=2)
                self.child.stdin.close()
                self.child.stdout.close()
                self.child.stderr.close()
            atomic_write(self.root / 'broker-result.json', canonical({
                'session': self.session, 'status': 'stopped' if stopped else 'failed',
                'last_sequence': self.gate.last['sequence'] if self.gate.last else None,
                'published': published, 'dropped': dropped,
                'child_returncode': self.child.returncode if self.child else None,
                'child_reaped': self.child is not None and self.child.poll() is not None,
                'reader_errors': self.errors[:2],
                'writer_errors': getattr(self, 'writer_error', [])[:2],
                'plane_validation': validation_backend(),
                'qualification': 'transport-only-no-renderer-or-Blender-claim'}))
            atomic_write(self.root / 'host-stderr.log', bytes(self.stderr_tail))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    args = parser.parse_args()
    config = json.loads(safe_path(args.config).read_bytes())
    key = bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY', ''))
    require(len(key) == 32, 'Key')
    Broker(key=key, **config).run()


if __name__ == '__main__':
    main()
