"""TEST ONLY: echoes synthetic one-pixel planes, never invokes a renderer."""
import hashlib
import os
import struct
import sys
import time
sys.path.insert(0, sys.argv[1])
from protocol import encode, read_record, write_record
key = bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY'))
mode = sys.argv[2]
while True:
    state, _ = read_record(sys.stdin.buffer, key)
    if state['kind'] == 'stop':
        if mode == 'stop-hang':
            time.sleep(30)
        sys.stderr.write('authenticated stop received\n')
        sys.exit(0)
    if mode == 'hang':
        time.sleep(30)
    if mode == 'delay':
        time.sleep(0.1)
    pixels = state['state']['width'] * state['state']['height']
    payload = bytes((1, 2, 3, 255)) * pixels + struct.pack('<f', 0.5) * pixels + bytes([1]) * pixels
    frame = {**state, 'kind': 'frame', 'planes_sha256': hashlib.sha256(payload).hexdigest()}
    if mode == 'wrong':
        frame['sequence'] += 1
    write_record(sys.stdout.buffer, encode(frame, payload, key))
