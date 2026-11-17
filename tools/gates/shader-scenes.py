#!/usr/bin/env python3
"""Compare bounded linear shader scenes, or stage reviewed reference candidates.

Regeneration writes a fresh candidate directory; applying it and refreshing asset
inventories is an explicit review step, never a response to a failed comparison.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import stat
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
# One 128-square RGB float image is 196608 bytes; all reads have this ceiling.
MAX_BYTES = 256 * 1024
SIZE = 128
CHANNELS = SIZE * SIZE * 3


def read_file(path, bound=MAX_BYTES):
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NONBLOCK', 0)
    flags |= getattr(os, 'O_NOFOLLOW', 0)
    descriptor = os.open(path, flags)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_size > bound:
            raise ValueError(f'{path}: expected bounded regular file')
        with os.fdopen(descriptor, 'rb', closefd=False) as stream:
            data = stream.read(bound + 1)
    finally:
        os.close(descriptor)
    if len(data) > bound:
        raise ValueError(f'{path}: file exceeds bound')
    return data


def digest(data):
    return hashlib.sha256(data).hexdigest()


def rgb(data, floating):
    header = b'PF\n128 128\n-1.0\n' if floating else b'P6\n128 128\n255\n'
    stride = 4 if floating else 1
    if not data.startswith(header) or len(data) != len(header) + CHANNELS * stride:
        raise ValueError('invalid image header, dimensions or exact payload size')
    values = (struct.unpack('<' + 'f' * CHANNELS, data[len(header):]) if floating
              else tuple(channel / 255 for channel in data[len(header):]))
    if not all(math.isfinite(value) and 0 <= value <= 1 for value in values):
        raise ValueError('nonfinite or out-of-range linear RGB')
    # Both formats in this fixture contract store rows bottom first.
    return values


def metrics(actual, reference, mask):
    if len(actual) != CHANNELS or len(reference) != CHANNELS:
        raise ValueError('wrong channel count')
    if (len(mask) != 4 or any(type(value) is not int for value in mask)
            or not 0 <= mask[0] < mask[2] <= SIZE or not 0 <= mask[1] < mask[3] <= SIZE):
        raise ValueError('invalid or empty rectangular mask')
    if not all(math.isfinite(value) for value in (*actual, *reference)):
        raise ValueError('nonfinite sample, including outside the mask')
    errors = sorted(abs(actual[(y * SIZE + x) * 3 + channel]
                        - reference[(y * SIZE + x) * 3 + channel])
                    for y in range(mask[1], mask[3]) for x in range(mask[0], mask[2])
                    for channel in range(3))
    return math.fsum(errors) / len(errors), errors[math.ceil(len(errors) * .99) - 1]


def local_file(root, name):
    if (not isinstance(name, str) or '\\' in name or ':' in name
            or not name or PurePosixPath(name).is_absolute()
            or any(part in ('', '.', '..') for part in name.split('/'))):
        raise ValueError('expected a relative generic file path')
    resolved = (root / name).resolve(strict=True)
    if not resolved.is_relative_to(root.resolve()):
        raise ValueError('file escapes its root')
    return resolved


def load_manifest(path, root=ROOT, verify_inputs=True):
    manifest = json.loads(read_file(path))
    if (not isinstance(manifest, dict) or type(manifest.get('schema')) is not int
            or manifest['schema'] != 1 or manifest.get('encoding') != 'linear_rgb_bottom_first'
            or manifest.get('size') != [SIZE, SIZE] or manifest.get('defines') != []
            or manifest.get('fixtures') != [{'name': 'material', 'reference': 'references/material.ppm',
                                            'mask': [0, 0, SIZE, SIZE]}]):
        raise ValueError('unknown scene inventory or encoding')
    if any(type(value) is not int for value in manifest['size'] + manifest['fixtures'][0]['mask']):
        raise ValueError('scene dimensions and mask must be integers')
    expected = {'res/shaders/static_asset.vert', 'res/shaders/static_asset.frag',
                'test/rendering/shader_scene_render_test.cpp'}
    if not isinstance(manifest.get('inputs'), dict) or set(manifest['inputs']) != expected:
        raise ValueError('incomplete shader/fixture input inventory')
    if verify_inputs:
        for name, expected_hash in manifest['inputs'].items():
            if digest(read_file(local_file(root, name)).replace(b'\r\n', b'\n')) != expected_hash:
                raise ValueError(f'{name}: input hash changed; review regeneration')
    reference = read_file(local_file(path.parent, 'references/material.ppm'))
    if digest(reference) != manifest.get('reference_sha256'):
        raise ValueError('reference hash mismatch')
    rgb(reference, False)
    metadata = manifest.get('reference_capture', {})
    if (not isinstance(metadata, dict) or set(metadata) != {'renderer', 'version', 'origin'}
            or not all(isinstance(value, str) and value for value in metadata.values())
            or metadata['origin'] not in ('analytical', 'llvmpipe')
            or (metadata['origin'] == 'llvmpipe' and 'llvmpipe' not in metadata['renderer'])):
        raise ValueError('missing reference renderer/version/origin')
    return manifest, reference


def run(args):
    manifest, reference = load_manifest(args.manifest, verify_inputs=args.command != 'regenerate')
    captured_inputs = {
        name: digest(read_file(args.capture / PurePosixPath(name).name).replace(b'\r\n', b'\n'))
        for name in manifest['inputs'] if name.startswith('res/shaders/')}
    fixture_hash = read_file(args.capture / 'fixture-sha256.txt', 128).decode('ascii').strip()
    if len(fixture_hash) != 64 or any(value not in '0123456789abcdef' for value in fixture_hash):
        raise ValueError('invalid compiled fixture identity')
    captured_inputs['test/rendering/shader_scene_render_test.cpp'] = fixture_hash
    current_inputs = (manifest['inputs'] if args.command == 'compare' else
                      {name: digest(read_file(local_file(ROOT, name)).replace(b'\r\n', b'\n'))
                       for name in manifest['inputs']})
    if captured_inputs != current_inputs:
        raise ValueError('capture inputs differ from the current shader/fixture sources; rebuild and recapture')
    actual_data = read_file(args.capture / 'material.pfm')
    actual = rgb(actual_data, True)
    renderer = read_file(args.capture / 'renderer.txt', 1024).decode('utf-8').strip()
    version = read_file(args.capture / 'version.txt', 1024).decode('utf-8').strip()
    if 'llvmpipe' not in renderer or not version or '\x00' in renderer + version:
        raise ValueError('missing actual llvmpipe renderer/version evidence')
    if args.command == 'compare':
        mean, percentile = metrics(actual, rgb(reference, False), manifest['fixtures'][0]['mask'])
        print(f'material: linear RGB MAE={mean:.8f} (<=0.005), p99={percentile:.8f} (<=0.02)')
        return int(mean > .005 or percentile > .02)
    if args.output is None:
        raise ValueError('regenerate requires a fresh --output candidate directory')
    payload = b'P6\n128 128\n255\n' + bytes(round(value * 255) for value in actual)
    manifest['inputs'] = captured_inputs
    manifest['reference_sha256'] = digest(payload)
    manifest['reference_capture'] = {'renderer': renderer, 'version': version, 'origin': 'llvmpipe'}
    encoded = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode('utf-8')
    # All refusals above precede publication. The new directory owns every write;
    # a failed I/O leaves an unpublished candidate, never an updated baseline.
    args.output.mkdir(exist_ok=False)
    (args.output / 'references').mkdir()
    (args.output / 'references/material.ppm').write_bytes(payload)
    (args.output / 'scenes.json').write_bytes(encoded)
    print(f'Review reference candidates in {args.output}; refresh inventories when applying.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('compare', 'regenerate'))
    parser.add_argument('--manifest', type=Path, default=ROOT / 'test/rendering/shader_scenes/scenes.json')
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, ValueError, TypeError, KeyError, UnicodeError) as error:
        print(f'shader scenes failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
