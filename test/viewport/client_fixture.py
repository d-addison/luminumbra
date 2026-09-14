"""Actual child-process fixture only; synthetic planes, never an engine/GPU."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.path.insert(0, sys.argv[1])
from protocol import encode, read_record, write_record

mode = sys.argv[2]
key = bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY'))
project = Path(os.environ['LUMINUMBRA_VIEWPORT_PROJECT'])
descendant = None
if mode == 'descendant':
    descendant = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
(project / 'child.json').write_text(json.dumps({'pid': os.getpid(),
    'descendant': descendant.pid if descendant else None,
    'key_removed_before_descendants': 'LUMINUMBRA_VIEWPORT_KEY' not in os.environ}))
while True:
    state, _ = read_record(sys.stdin.buffer, key)
    if state['kind'] == 'stop':
        if mode == 'stop-hang':
            time.sleep(60)
        if mode in ('receipt', 'failure-receipt', 'large-receipt'):
            evidence = Path(os.environ['CLIENT_FIXTURE_EVIDENCE'])
            evidence.mkdir()
            if mode == 'receipt':
                # Synthetic receipt tests preservation and joins, never rendering.
                identity = json.loads(Path(os.environ['CLIENT_FIXTURE_IDENTITY']).read_text())
                (evidence / 'session.json').write_text(json.dumps(identity, indent=2) + '\n')
            elif mode == 'failure-receipt':
                (evidence / 'failure.json').write_text(json.dumps({'status': 'refused',
                    'error': 'deliberate synthetic failure', 'visual_approved': False}))
            else:
                (evidence / 'session.json').write_bytes(b' ' * (1024 * 1024 + 1))
        sys.exit(0)
    if mode == 'hang':
        time.sleep(60)
    if mode == 'delay':
        time.sleep(.1)
    pixels = state['state']['width'] * state['state']['height']
    color = state['state']['camera_revision'] % 256
    payload = bytes((color, 2, 3, 255)) * pixels + struct.pack('<f', .5) * pixels + bytes([1]) * pixels
    frame = {**state, 'kind': 'frame', 'planes_sha256': hashlib.sha256(payload).hexdigest()}
    if mode == 'wrong':
        frame['sequence'] += 1
    write_record(sys.stdout.buffer, encode(frame, payload, key))
