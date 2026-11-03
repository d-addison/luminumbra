"""Actual child-process fixture only; synthetic planes, never an engine/GPU."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, sys.argv[1])
from protocol import atomic_write, canonical, encode, read_record, write_record

mode = sys.argv[2]
key = bytes.fromhex(os.environ.pop('LUMINUMBRA_VIEWPORT_KEY'))
project = Path(os.environ['LUMINUMBRA_VIEWPORT_PROJECT'])
session = os.environ.pop('LUMINUMBRA_VIEWPORT_SESSION')
ready_path = Path(os.environ.pop('LUMINUMBRA_VIEWPORT_READY'))
# Intentional hangs below stay inside owned child processes. Parent watchdogs
# terminate their job/process group without needing a lock held by this fixture.
descendant = None
if mode in ('descendant', 'startup-delay-descendant'):
    descendant = subprocess.Popen([sys.executable, '-c', 'import threading; threading.Event().wait()'],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
child = {'session': session, 'pid': os.getpid(), 'broker_pid': os.getppid(),
    'descendant': descendant.pid if descendant else None,
    'key_removed_before_descendants': 'LUMINUMBRA_VIEWPORT_KEY' not in os.environ}
atomic_write(project / ('child-' + session + '.json'), canonical(child))
atomic_write(project / 'child.json', canonical(child))
marker = {'schema': 'luminumbra.viewport.ready.v1', 'session': session, 'pid': os.getpid()}
if mode in ('startup-delay', 'startup-delay-descendant', 'startup-frame-delay'):
    # Test-controlled release avoids asserting against fixture interpreter startup.
    while not (project / ('release-' + session)).exists():
        time.sleep(.01)
elif mode == 'startup-hang':
    threading.Event().wait()
elif mode == 'startup-exit':
    sys.exit(7)
elif mode == 'startup-wrong-session':
    marker['session'] = 'f' * 32
elif mode == 'startup-wrong-pid':
    marker['pid'] += 1
elif mode == 'startup-oversized':
    atomic_write(ready_path, b' ' * 513)
if mode != 'startup-oversized':
    atomic_write(ready_path, canonical(marker))
while True:
    state, _ = read_record(sys.stdin.buffer, key)
    if state['kind'] == 'stop':
        (project / ('stop-received-' + session)).touch()
        if mode == 'stop-hang':
            threading.Event().wait()
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
    atomic_write(project / ('frame-received-' + session), canonical(state['sequence']))
    if mode == 'hang':
        threading.Event().wait()
    if mode == 'startup-frame-delay':
        while not (project / ('frame-release-' + session)).exists():
            time.sleep(.01)
    pixels = state['state']['width'] * state['state']['height']
    color = state['state']['camera_revision'] % 256
    payload = bytes((color, 2, 3, 255)) * pixels + struct.pack('<f', .5) * pixels + bytes([1]) * pixels
    frame = {**state, 'kind': 'frame', 'planes_sha256': hashlib.sha256(payload).hexdigest()}
    if mode == 'wrong':
        frame['sequence'] += 1
    write_record(sys.stdout.buffer, encode(frame, payload, key))
    atomic_write(project / ('frame-sent-' + session), canonical(state['sequence']))
