"""TEST ONLY: echoes synthetic one-pixel planes, never invokes a renderer."""
import hashlib
import os
import struct
import sys
import time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from protocol import atomic_write, canonical, encode, read_record, write_record
key = bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY'))
mode = sys.argv[2]
atomic_write(os.environ.pop('LUMINUMBRA_VIEWPORT_READY'), canonical({
    'schema': 'luminumbra.viewport.ready.v1',
    'session': os.environ.pop('LUMINUMBRA_VIEWPORT_SESSION'), 'pid': os.getpid()}))
while True:
    state, _ = read_record(sys.stdin.buffer, key)
    if state['kind'] == 'stop':
        atomic_write(Path(os.environ['LUMINUMBRA_VIEWPORT_PROJECT']) / 'stop-received', b'')
        if mode == 'stop-hang':
            __import__('threading').Event().wait()
        sys.stderr.write('authenticated stop received\n')
        sys.exit(0)
    atomic_write(Path(os.environ['LUMINUMBRA_VIEWPORT_PROJECT']) / 'request-received', b'')
    if mode == 'hang':
        __import__('threading').Event().wait()
    if mode == 'delay':
        while not (Path(os.environ['LUMINUMBRA_VIEWPORT_PROJECT']) / ('release-' + str(state['sequence']))).exists():
            time.sleep(.01)
    pixels = state['state']['width'] * state['state']['height']
    payload = bytes((1, 2, 3, 255)) * pixels + struct.pack('<f', 0.5) * pixels + bytes([1]) * pixels
    frame = {**state, 'kind': 'frame', 'planes_sha256': hashlib.sha256(payload).hexdigest()}
    if mode == 'wrong':
        frame['sequence'] += 1
    write_record(sys.stdout.buffer, encode(frame, payload, key))
