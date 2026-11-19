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
# Exact bounded catalogue; adding a fixture requires matching capture and tests.
FIXTURES = [
    {'name': name, 'reference': f'references/{name}.ppm', 'mask': [0, 0, SIZE, SIZE]}
    for name in ('material', 'tree-cutout', 'night-sky', 'terrain-blend')]
# Dusk has a measured discontinuity: capture/report only, never an accepted baseline.
DIAGNOSTICS = [{'name': 'dusk-sky', 'metric': 'max_adjacent_column_mean_luminance_step',
                'pending_tolerance': .005, 'tracking_issue': 918}]
INPUTS = {'res/shaders/static_asset.vert', 'res/shaders/static_asset.frag',
          'res/shaders/tree_impostor.vert', 'res/shaders/tree_impostor.frag',
          'res/shaders/enhanced_skybox.frag', 'res/shaders/g_buffer.vert', 'res/shaders/g_buffer.frag',
          'src/luminumbra_client/rendering/SkyAtmosphereLut.ipp',
          'test/rendering/shader_scene_render_test.cpp'}


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


def tree_cutout_error(actual):
    """Exact fixed-atlas categories, independent of quantized reference pixels."""
    if len(actual) != CHANNELS or not all(math.isfinite(value) for value in actual):
        raise ValueError('invalid cutout samples')
    # One bounded 128-square pass; zero/0.499 discard, 0.5/1 survive.
    return max(abs(actual[(y * SIZE + x) * 3 + channel] -
                   ((.0625, .125, .1875) if (x // 32 + y // 32) % 4 < 2
                    else (.25, .5, .75))[channel])
               for y in range(SIZE) for x in range(SIZE) for channel in range(3))


def scene_samples(actual):
    if (len(actual) != CHANNELS or
            not all(math.isfinite(value) and 0 <= value <= 1 for value in actual)):
        raise ValueError('invalid scene samples')


def luminance(actual):
    scene_samples(actual)
    return [math.fsum(actual[index + channel] * weight
                     for channel, weight in enumerate((.2126, .7152, .0722)))
            for index in range(0, CHANNELS, 3)]


def night_sky_metrics(actual):
    """Horizon band and four resolved stellar spots, independent of the PPM."""
    light = luminance(actual)
    below = max(actual[:64 * SIZE * 3])  # Rows 0..63 include the exact horizon.
    peaks, contrasts, gaps = [], [], []
    # Four 32x64 tiles; one peak and its eight-neighbour ring per star.
    for tile in range(4):
        peak, x, y = max((light[y * SIZE + x], x, y)
                         for y in range(64, SIZE) for x in range(tile * 32, (tile + 1) * 32))
        if x in (tile * 32, (tile + 1) * 32 - 1) or y in (64, SIZE - 1):
            raise ValueError('stellar peak touches the patch boundary')
        ring = [light[(y + dy) * SIZE + x + dx]
                for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dx or dy]
        peaks.append(peak)
        contrasts.append(peak - math.fsum(ring) / 8)
        gaps.append(peak - max(ring))
    return below, min(peaks), min(contrasts), min(gaps)


def terrain_blend_metrics(actual):
    """Two triangles must carry the same wide, monotonic Stone/Sand ramp."""
    scene_samples(actual)
    widths, reversals, steps = [], [], []
    for y in range(SIZE):
        weights = [(actual[(y * SIZE + x) * 3] - .5) / .4 for x in range(SIZE)]
        widths.append(sum(.1 < weight < .9 for weight in weights))
        reversals.append(max(0., max(weights[x] - weights[x + 1] for x in range(SIZE - 1))))
        steps.append(max(abs(weights[x + 1] - weights[x]) for x in range(SIZE - 1)))
    error = max(abs(actual[(y * SIZE + x) * 3 + channel] - (.5 + delta * (x + .5) / SIZE))
                for y in range(SIZE) for x in range(SIZE)
                for channel, delta in enumerate((.4, .3, 0)))
    return min(widths), max(widths), max(reversals), max(steps), error


def horizontal_step(actual):
    """Largest adjacent column-mean luma step, including the azimuth wrap."""
    light = luminance(actual)
    columns = [math.fsum(light[y * SIZE + x] for y in range(SIZE)) / SIZE
               for x in range(SIZE)]
    return max(abs(columns[(x + 1) % SIZE] - columns[x]) for x in range(SIZE))


def check_scene_invariants(actuals):
    below, peak, contrast, gap = night_sky_metrics(actuals['night-sky'])
    print(f'night-sky: below-horizon energy={below:.8f} (<=0.000001), '
          f'minimum stellar peak={peak:.8f} (>=0.01), '
          f'peak-minus-ring mean={contrast:.8f} (>=0.001), max={gap:.8f} (>=0.00001)')
    if below > .000001 or peak < .01 or contrast < .001 or gap < .00001:
        raise ValueError('night sky horizon/radial invariant failed')
    low, high, reversal, step, error = terrain_blend_metrics(actuals['terrain-blend'])
    print(f'terrain-blend: 10..90% width={low}..{high} (102 pixels), '
          f'backstep={reversal:.8f} (<=0.000001), step={step:.8f} (<=0.008), '
          f'maximum RGB ramp error={error:.8f} (<=0.000001)')
    if low != 102 or high != 102 or reversal > .000001 or step > .008 or error > .000001:
        raise ValueError('terrain blend width/monotonic invariant failed')


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
            or manifest['schema'] != 3 or manifest.get('encoding') != 'linear_rgb_bottom_first'
            or manifest.get('size') != [SIZE, SIZE] or manifest.get('defines') != []
            or manifest.get('fixtures') != FIXTURES
            or manifest.get('diagnostics') != DIAGNOSTICS):
        raise ValueError('unknown scene inventory or encoding')
    dimensions = manifest['size'] + [value for fixture in manifest['fixtures']
                                     for value in fixture['mask']]
    if any(type(value) is not int for value in dimensions):
        raise ValueError('scene dimensions and mask must be integers')
    if not isinstance(manifest.get('inputs'), dict) or set(manifest['inputs']) != INPUTS:
        raise ValueError('incomplete shader/fixture input inventory')
    if verify_inputs:
        for name, expected_hash in manifest['inputs'].items():
            if digest(read_file(local_file(root, name)).replace(b'\r\n', b'\n')) != expected_hash:
                raise ValueError(f'{name}: input hash changed; review regeneration')
    hashes = manifest.get('reference_sha256')
    if not isinstance(hashes, dict) or set(hashes) != {f['name'] for f in FIXTURES}:
        raise ValueError('incomplete reference hash inventory')
    references = {}
    for fixture in FIXTURES:
        reference = read_file(local_file(path.parent, fixture['reference']))
        if digest(reference) != hashes[fixture['name']]:
            raise ValueError('reference hash mismatch')
        rgb(reference, False)
        references[fixture['name']] = reference
    captures = manifest.get('reference_capture')
    if not isinstance(captures, dict) or set(captures) != set(references):
        raise ValueError('incomplete reference renderer inventory')
    for metadata in captures.values():
        if (not isinstance(metadata, dict) or set(metadata) != {'renderer', 'version', 'origin'}
                or not all(isinstance(value, str) and value for value in metadata.values())
                or metadata['origin'] not in ('analytical', 'llvmpipe')
                or (metadata['origin'] == 'llvmpipe' and 'llvmpipe' not in metadata['renderer'])):
            raise ValueError('missing reference renderer/version/origin')
    return manifest, references


def run(args):
    manifest, references = load_manifest(args.manifest, verify_inputs=args.command != 'regenerate')
    captured_inputs = {
        name: digest(read_file(args.capture / PurePosixPath(name).name).replace(b'\r\n', b'\n'))
        for name in manifest['inputs'] if name != 'test/rendering/shader_scene_render_test.cpp'}
    fixture_hash = read_file(args.capture / 'fixture-sha256.txt', 128).decode('ascii').strip()
    if len(fixture_hash) != 64 or any(value not in '0123456789abcdef' for value in fixture_hash):
        raise ValueError('invalid compiled fixture identity')
    captured_inputs['test/rendering/shader_scene_render_test.cpp'] = fixture_hash
    current_inputs = (manifest['inputs'] if args.command == 'compare' else
                      {name: digest(read_file(local_file(ROOT, name)).replace(b'\r\n', b'\n'))
                       for name in manifest['inputs']})
    if captured_inputs != current_inputs:
        raise ValueError('capture inputs differ from the current shader/fixture sources; rebuild and recapture')
    actuals = {fixture['name']: rgb(read_file(args.capture / (fixture['name'] + '.pfm')), True)
               for fixture in FIXTURES}
    dusk = rgb(read_file(args.capture / 'dusk-sky.pfm'), True)
    cutout_error = tree_cutout_error(actuals['tree-cutout'])
    renderer = read_file(args.capture / 'renderer.txt', 1024).decode('utf-8').strip()
    version = read_file(args.capture / 'version.txt', 1024).decode('utf-8').strip()
    if 'llvmpipe' not in renderer or not version or '\x00' in renderer + version:
        raise ValueError('missing actual llvmpipe renderer/version evidence')
    print(f'tree-cutout: maximum categorical RGB error={cutout_error:.8f} (<=0.000001)')
    if cutout_error > .000001:
        raise ValueError('tree cutout/background invariant failed')
    check_scene_invariants(actuals)
    print(f'dusk-sky: maximum adjacent column-mean luminance step={horizontal_step(dusk):.8f}; '
          'tolerance <=0.005 pending issue #918 (diagnostic only; no reference expectation)')
    if args.command == 'compare':
        failed = False
        for fixture in FIXTURES:
            name = fixture['name']
            mean, percentile = metrics(actuals[name], rgb(references[name], False), fixture['mask'])
            print(f'{name}: linear RGB MAE={mean:.8f} (<=0.005), p99={percentile:.8f} (<=0.02)')
            failed |= mean > .005 or percentile > .02
        return int(failed)
    if args.output is None:
        raise ValueError('regenerate requires a fresh --output candidate directory')
    payloads = {name: b'P6\n128 128\n255\n' + bytes(round(value * 255) for value in actual)
                for name, actual in actuals.items()}
    manifest['inputs'] = captured_inputs
    manifest['reference_sha256'] = {name: digest(payload) for name, payload in payloads.items()}
    manifest['reference_capture'] = {name: {'renderer': renderer, 'version': version, 'origin': 'llvmpipe'}
                                     for name in actuals}
    encoded = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode('utf-8')
    # All refusals above precede publication. The new directory owns every write;
    # a failed I/O leaves an unpublished candidate, never an updated baseline.
    args.output.mkdir(exist_ok=False)
    (args.output / 'references').mkdir()
    for fixture in FIXTURES:
        (args.output / fixture['reference']).write_bytes(payloads[fixture['name']])
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
