"""Exercise an installed persistent renderer over its authenticated binary pipe.

Requires an actual service-compiled static prefab. Does not import Blender.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import selectors
import signal
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/blender/authoring/viewport'))
import protocol as wire
from broker import Broker


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {p.relative_to(root).as_posix(): sha(p)
            for p in sorted(root.rglob('*')) if p.is_file()}


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def camera_states(first, profile, root_ids):
    """At most eight complete states, matching the host's bounded receipt ring."""
    def ortho(state):
        near, far = state['near_plane'], state['far_plane']
        state['projection'] = [.375, 0, 0, 0, 0, .5, 0, 0,
                               0, 0, 1 / (far-near), 0, 0, 0, far / (far-near), 1]

    current = copy.deepcopy(first)
    if profile == 'orthographic':
        ortho(current['state'])
    states = [copy.deepcopy(current)]
    if profile == 'mixed':
        ortho(current['state'])
        states.append(copy.deepcopy(current))
    else:
        current['state']['locals'][0]['matrix'][12] += .35
        states.extend([copy.deepcopy(current), copy.deepcopy(current)])
    current['state']['view'][12] = -.25
    states.append(copy.deepcopy(current))
    if profile != 'perspective':
        for index in (0, 5):
            current['state']['projection'][index] *= 1.5
        states.append(copy.deepcopy(current))
    current['state'].update(width=240, height=180)
    states.append(copy.deepcopy(current))
    changed = copy.deepcopy(current)
    if profile == 'perspective':
        changed['state']['locals'] = []
    else:
        for local in changed['state']['locals']:
            if local['node_id'] in root_ids:
                local['matrix'][14] -= 2
    states.extend([changed, copy.deepcopy(current)])
    if profile == 'mixed':
        states.append(copy.deepcopy(first))
    scene_revision = camera_revision = 0
    camera = ('view', 'projection', 'width', 'height', 'near_plane', 'far_plane')
    for index, desired in enumerate(states):
        if index:
            previous = states[index-1]['state']
            scene_revision += desired['state']['locals'] != previous['locals']
            camera_revision += any(desired['state'][name] != previous[name] for name in camera)
        desired['sequence'] = index + 1
        desired['state'].update(scene_revision=scene_revision, camera_revision=camera_revision)
    return states


def depth_translation_check(check, output, frames, first, second):
    a = (output / f'frame-{first+1}/planes.bin').read_bytes()
    b = (output / f'frame-{second+1}/planes.bin').read_bytes()
    pixels = len(a) // 9
    check('orthographic depth movement preserves extent and coverage',
          len(a) == len(b) and a[8*pixels:] == b[8*pixels:] and frames[first]['covered_pixels'] > 0)
    error = max(abs((db[0] - da[0]) + 2 / 99.9)
                for i, (da, db) in enumerate(zip(struct.iter_unpack('<f', a[4*pixels:8*pixels]),
                                                 struct.iter_unpack('<f', b[4*pixels:8*pixels])))
                if a[8*pixels+i])
    check('orthographic depth changes by the supplied linear translation', error < 2e-6)
    check('orthographic translation retains color under parallel lighting',
          frames[first]['rgba_sha256'] == frames[second]['rgba_sha256'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('host', 'project', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--generation', required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--software-only', action='store_true')
    parser.add_argument('--xvfb', type=Path)
    parser.add_argument('--broker', action='store_true', help='Exercise the production mailbox broker and leased slots')
    parser.add_argument('--camera-profile', choices=('perspective', 'orthographic', 'mixed'), default='perspective')
    parser.add_argument('--refusals', action='store_true', help='Run isolated native IPC/state refusals and clean restarts')
    parser.add_argument('--expected-source-commit', help='Require this exact clean installed source commit')
    args = parser.parse_args()
    host, project, output = (getattr(args, name).resolve() for name in ('host', 'project', 'output'))
    generation = project / '.luminumbra-author/generations' / args.generation
    wire.require(len(args.generation) == 32 and all(c in '0123456789abcdef' for c in args.generation), 'Generation')
    wire.require(sha(generation / 'manifest.json') == args.manifest_sha256, 'Manifest pin')
    descriptor = json.loads((generation / 'prefab.json').read_bytes())
    output.mkdir(parents=False, exist_ok=False)
    sdk = host.parent.parent
    before = {'sdk': inventory(sdk), 'generation': inventory(generation),
              'probe_sha256': sha(Path(__file__)), 'wire_sha256': sha(Path(wire.__file__)),
              'broker_sha256': sha(Path(sys.modules[Broker.__module__].__file__)),
              'refusal_probe_sha256': sha(Path(__file__).with_name('installed_viewport_refusals.py'))}
    write(output / 'inputs-before.json', before)
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    scale = 1 / math.tan(math.pi / 8)
    near, far = .1, 100.
    projection = [scale / (4 / 3), 0, 0, 0, 0, scale, 0, 0,
                  0, 0, near / (far - near), -1, 0, 0, far * near / (far - near), 0]
    locals_ = []
    for node in descriptor['nodes']:
        local = list(node['local_matrix'])
        if node['parent'] is None:
            # Place the synthetic service fixture in front of the camera.
            local = identity.copy()
            local[12:15] = [-.5, -.5, -3]
        locals_.append({'instance_id': 'probe', 'node_id': node['id'], 'matrix': local})
    key = os.urandom(32)
    session = os.urandom(16).hex()
    first = {'kind': 'state', 'session': session, 'sequence': 1, 'state': {
        'generation_id': args.generation, 'manifest_sha256': args.manifest_sha256,
        'scene_revision': 0, 'camera_revision': 0, 'width': 160, 'height': 120,
        'near_plane': near, 'far_plane': far, 'view': identity, 'projection': projection,
        'locals': locals_}}
    states = camera_states(first, args.camera_profile,
                           {node['id'] for node in descriptor['nodes'] if node['parent'] is None})
    write(output / 'states.json', states)
    env = os.environ.copy()
    env.update(LUMINUMBRA_VIEWPORT_KEY=key.hex(), LUMINUMBRA_VIEWPORT_PROJECT=str(project))
    if args.software_only:
        env.update(LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='llvmpipe',
                   MESA_LOADER_DRIVER_OVERRIDE='swrast', __GLX_VENDOR_LIBRARY_NAME='mesa',
                   LP_NUM_THREADS='2', MESA_GLTHREAD='false')
    display = process = timer = broker = broker_thread = None
    reader = None
    broker_errors = []
    saved_environment = {}
    expired = threading.Event()
    responses = queue.Queue(maxsize=2)
    frames = []
    started = time.monotonic()
    receipt = {'passed': False, 'visual_approved': False, 'blender_qualified': False,
               'software_only': args.software_only, 'transport': 'broker' if args.broker else 'direct',
               'camera_profile': args.camera_profile,
               'checks': [], 'frames': frames}

    def check(name, condition):
        receipt['checks'].append({'name': name, 'passed': bool(condition)})
        wire.require(condition, name)

    try:
        with (output / 'display.log').open('wb') as display_log, (output / 'host.log').open('wb') as log:
            if args.xvfb:
                wire.require(os.name == 'posix' and args.software_only, 'Xvfb requires POSIX software profile')
                read_fd, write_fd = os.pipe()
                try:
                    display = subprocess.Popen([str(args.xvfb), '-displayfd', str(write_fd),
                        '-screen', '0', '1280x720x24', '-nolisten', 'tcp', '-nolisten', 'unix', '-listen', 'local'],
                        pass_fds=(write_fd,), stdout=display_log, stderr=display_log, env=env, start_new_session=True)
                    os.close(write_fd)
                    write_fd = -1
                    with selectors.DefaultSelector() as selector:
                        selector.register(read_fd, selectors.EVENT_READ)
                        wire.require(selector.select(10), 'Owned display readiness')
                        number = os.read(read_fd, 32).decode().strip()
                    wire.require(number.isdigit(), 'Owned display identity')
                    env['DISPLAY'] = ':' + number
                finally:
                    os.close(read_fd)
                    if write_fd >= 0:
                        os.close(write_fd)
            argv = [str(host), '--serve', '--output', str(output / 'host-evidence')]
            if args.software_only:
                argv.append('--software-only')
            if args.broker:
                mailbox = output / 'mailbox'
                mailbox.mkdir(mode=0o700)
                # This is a standalone probe. Only its graphics environment is
                # inherited by the actual broker, which supplies its own key.
                for name in ('DISPLAY', 'LIBGL_ALWAYS_SOFTWARE', 'GALLIUM_DRIVER',
                             'MESA_LOADER_DRIVER_OVERRIDE', '__GLX_VENDOR_LIBRARY_NAME',
                             'LP_NUM_THREADS', 'MESA_GLTHREAD'):
                    if name in env:
                        saved_environment[name] = os.environ.get(name)
                        os.environ[name] = env[name]
                broker = Broker(mailbox, key, session, argv, sha(host), project)

                def run_broker():
                    try:
                        broker.run()
                    except Exception as error:
                        broker_errors.append(error)

                broker_thread = threading.Thread(target=run_broker, daemon=True)
                broker_thread.start()
                until = time.monotonic() + 10
                while broker.child is None and not broker_errors and time.monotonic() < until:
                    time.sleep(.01)
                wire.require(broker.child is not None, 'Broker starts owned host')
                process = broker.child
            else:
                process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                           stderr=log, env=env, cwd=output, bufsize=0)
            receipt['argv'] = argv
            receipt['pid'] = process.pid

            def kill_owned():
                expired.set()
                if process.poll() is None:
                    process.kill()

            timer = threading.Timer(75, kill_owned)
            timer.start()

            def read():
                try:
                    while True:
                        responses.put(wire.read_record(process.stdout, key), timeout=1)
                except Exception as error:
                    try:
                        responses.put(error, timeout=1)
                    except queue.Full:
                        pass

            if not args.broker:
                reader = threading.Thread(target=read, daemon=True)
                reader.start()
            for desired in states:
                tick = time.monotonic()
                record = wire.encode(desired, b'', key)
                if args.broker:
                    wire.atomic_write(mailbox / 'desired.bin', record)
                    result = None
                    while result is None and time.monotonic() - tick < 65:
                        wire.require(not broker_errors and broker_thread.is_alive(), 'Broker remains active')
                        for slot in (0, 1):
                            candidate = broker.slots.read(slot)
                            if candidate and candidate[0]['sequence'] >= desired['sequence']:
                                result = candidate
                        if result is None:
                            time.sleep(.005)
                    wire.require(result is not None, 'Broker frame deadline')
                else:
                    wire.write_record(process.stdin, record)
                    result = responses.get(timeout=65)
                if isinstance(result, Exception):
                    raise result
                frame, payload = result
                wire.match_frame(frame, desired)
                elapsed_ms = (time.monotonic() - tick) * 1000
                directory = output / ('frame-' + str(desired['sequence']))
                directory.mkdir()
                write(directory / 'header.json', frame)
                (directory / 'planes.bin').write_bytes(payload)
                pixels = frame['state']['width'] * frame['state']['height']
                frames.append({'sequence': frame['sequence'], 'planes_sha256': hashlib.sha256(payload).hexdigest(),
                               'rgba_sha256': hashlib.sha256(payload[:4*pixels]).hexdigest(),
                               'depth_sha256': hashlib.sha256(payload[4*pixels:8*pixels]).hexdigest(),
                               'covered_pixels': sum(payload[8*pixels:]),
                               'request_to_frame_ms': elapsed_ms})
            stop = {'kind': 'stop', 'session': session, 'sequence': len(states) + 1}
            if args.broker:
                wire.atomic_write(mailbox / 'stop.bin', wire.encode(stop, b'', key))
                broker_thread.join(timeout=10)
                check('broker cooperative stop', not broker_thread.is_alive() and not broker_errors and
                      json.loads((mailbox / 'broker-result.json').read_bytes())['status'] == 'stopped')
            else:
                wire.write_record(process.stdin, wire.encode(stop, b'', key))
                process.stdin.close()
            check('authenticated stop completes and reaps host', process.wait(timeout=10) == 0 and not expired.is_set())
            timer.cancel()
            timer = None
            report = json.loads((output / 'host-evidence/session.json').read_bytes())
            check('all complete frames and one generation load', report['shutdown_complete'] and
                  report['frame_count'] == len(states) and report['generation_loads'] == 1 and
                  len(report['recent_frames']) == len(states))
            check('installed source identity is clean', report['source_dirty'] is False)
            if args.expected_source_commit:
                check('installed source commit pin', report['source_commit'] == args.expected_source_commit)
            check('rendering produces visible coverage', frames[0]['covered_pixels'] > 0)
            if args.camera_profile != 'mixed':
                check('matrix edit changes pixels and unchanged state repeats exactly',
                      frames[0]['rgba_sha256'] != frames[1]['rgba_sha256'] == frames[2]['rgba_sha256'] and
                      frames[1]['planes_sha256'] == frames[2]['planes_sha256'])
            pan = 2 if args.camera_profile == 'mixed' else 3
            check('camera pan changes pixels', frames[pan-1]['rgba_sha256'] != frames[pan]['rgba_sha256'])
            if args.camera_profile == 'perspective':
                check('complete removal clears output', frames[5]['covered_pixels'] == 0 and report['recent_frames'][5]['draws'] == 0)
                check('restoration reproduces resized frame', frames[4]['planes_sha256'] == frames[6]['planes_sha256'])
            else:
                zoom, resize = pan + 1, pan + 2
                check('orthographic zoom increases actual coverage', frames[zoom]['covered_pixels'] > frames[pan]['covered_pixels'])
                check('orthographic resize increases actual coverage', frames[resize]['covered_pixels'] > frames[zoom]['covered_pixels'])
                depth_translation_check(check, output, frames, resize, resize + 1)
                check('orthographic restoration reproduces all planes', frames[resize]['planes_sha256'] == frames[resize+2]['planes_sha256'])
                if args.camera_profile == 'mixed':
                    check('perspective to orthographic changes actual color and depth',
                          frames[0]['rgba_sha256'] != frames[1]['rgba_sha256'] and
                          frames[0]['depth_sha256'] != frames[1]['depth_sha256'])
                    check('mixed camera restoration reproduces perspective planes', frames[0]['planes_sha256'] == frames[-1]['planes_sha256'])
            for index, row in enumerate(report['recent_frames']):
                check('frame state and plane receipt ' + str(index + 1), row['wire_header'] ==
                      json.loads((output / f'frame-{index+1}/header.json').read_bytes()) and
                      row['planes_sha256'] == frames[index]['planes_sha256'] and
                      row['renderer_sequence'] == index + 1)
                for matrix_name in ('view', 'projection'):
                    expected = list(struct.unpack('<16f', struct.pack('<16f',
                                    *states[index]['state'][matrix_name])))
                    check('actual ' + matrix_name + ' ' + str(index + 1),
                          row['actual_' + matrix_name] == expected)
                if index > 0 and (args.camera_profile != 'perspective' or index <= 4):
                    check('camera/transform/resize retains uploads ' + str(index + 1),
                          row['mesh_uploads'] == row['texture_uploads'] == 0)
                if args.software_only:
                    check('actual software renderer ' + str(index + 1), 'llvmpipe' in row['gpu']['renderer'])
            if args.refusals:
                from installed_viewport_refusals import run_refusals
                receipt['refusals'] = run_refusals(host, project, output / 'refusals', env, first,
                                                  args.software_only, report)
                check('native refusals and fresh restarts pass', receipt['refusals']['passed'])
            after = {'sdk': inventory(sdk), 'generation': inventory(generation),
                     'probe_sha256': sha(Path(__file__)), 'wire_sha256': sha(Path(wire.__file__)),
                     'broker_sha256': sha(Path(sys.modules[Broker.__module__].__file__)),
                     'refusal_probe_sha256': sha(Path(__file__).with_name('installed_viewport_refusals.py'))}
            check('installed engine and generation bytes unchanged', before == after)
            receipt.update(passed=True, source_commit=report['source_commit'], source_dirty=report['source_dirty'],
                           source_input_sha256=report['source_input_sha256'],
                           session_sha256=sha(output / 'host-evidence/session.json'), inputs_before=before, inputs_after=after)
    except Exception as error:
        receipt['broker_errors'] = [str(value)[:512] for value in broker_errors[:2]]
        if broker is not None:
            receipt['broker_reader_errors'] = list(broker.errors[:2])
        receipt['error'] = str(error)
        raise
    finally:
        if timer:
            timer.cancel()
        if process:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            if broker_thread:
                broker_thread.join(timeout=5)
            if not process.stdin.closed:
                process.stdin.close()
            process.stdout.close()
            if reader:
                reader.join(timeout=2)
            receipt.update(exit_code=process.returncode, timed_out=expired.is_set())
        if display and display.poll() is None:
            os.killpg(display.pid, signal.SIGTERM)
            try:
                display.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(display.pid, signal.SIGKILL)
                display.wait(timeout=5)
        for name, value in saved_environment.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        receipt['seconds'] = time.monotonic() - started
        write(output / 'acceptance.json', receipt)
    print(json.dumps({'passed': receipt['passed'], 'checks': len(receipt['checks']), 'receipt': str(output / 'acceptance.json')}))


if __name__ == '__main__':
    main()
