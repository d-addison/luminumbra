#!/usr/bin/env python3
"""Validate deferred diagnostic bundles and bounded, qualified ROI image metrics.

Accept a bundle directory, completion JSON, or recursive capture tree. Exit codes:
0 valid, 1 invalid bundle, 2 usage or operating-system I/O error. Nonfinite AOV
samples are findings (exit 1), even though the wire format preserves them.
"""
import argparse
from contextlib import ExitStack
import json
import math
import os
from pathlib import Path
import re
import runpy
import stat
import struct
import sys

# Shared stdlib-only descriptor validation covers POSIX and Windows handle paths.
open_regular = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'perf' /
                                  'fixture_paths.py'))['open_regular']
MAX_JSON = 16 * 1024 * 1024
# Unsupported records can fill the table bound; allow another 64 KiB for eight
# escaped filenames, 32 camera floats and the fixed completion fields.
MAX_COMPLETION = MAX_JSON + 64 * 1024
MAX_PIXELS = 16777216
MAX_ENTRIES = 100000
SUFFIX = '.diagnostics.json'
WEATHER = ('precipitation', 'fog', 'cloud')
DEFAULT_ROIS = Path(__file__).with_name('baselines') / 'diagnostic-rois.json'
PLANES = {'albedo': 3, 'normal': 3, 'depth': 1, 'material': 1}
ENCODINGS = dict(albedo='linear_rgb_float32', normal='signed_view_xyz_float32',
                 depth='hardware_depth_float32', material='integral_0_255_float32',
                 ids='LVID_1_RGBA32UI_little_endian')


class Invalid(ValueError):
    """A complete or partial artifact violates the published contract."""


def require(condition, reason):
    if not condition:
        raise Invalid(reason)


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def leaf(value):
    return (isinstance(value, str) and 0 < len(value.encode('utf-8')) <= 1024
            and value not in ('.', '..') and not any(c in value for c in '/\\:')
            and all(ord(c) >= 32 and ord(c) != 127 for c in value))


def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, 'duplicate JSON key')
        result[key] = value
    return result


def reject_constant(_):
    raise Invalid('nonstandard JSON number')


def json_integer(text):
    # Schema-1 uint64/float32 values need at most 39 decimal digits. Cap work
    # before int() even on Python versions without a built-in digit limit.
    require(len(text) <= 128, 'JSON integer bound')
    return int(text)


def document(stream, limit=MAX_JSON):
    require(0 < os.fstat(stream.fileno()).st_size <= limit, 'JSON size bound')
    data = stream.read(limit + 1)
    require(len(data) <= limit, 'JSON size bound')
    try:
        value = json.loads(data, object_pairs_hook=pairs, parse_constant=reject_constant,
                           parse_int=json_integer)
    except Invalid:
        raise
    except (ValueError, UnicodeError, RecursionError) as error:
        raise Invalid('malformed JSON') from error
    require(isinstance(value, dict), 'JSON object required')
    return value


def schema(value):
    require(type(value.get('schema')) is int and value['schema'] == 1,
            'unknown schema version')


def finite(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def matrix(value):
    return (isinstance(value, list) and len(value) == 16
            and all(finite(v) for v in value))


def tables(table, metadata):
    schema(table)
    meshes, draws = table.get('meshes'), table.get('draws')
    require(isinstance(meshes, list) and isinstance(draws, list)
            and len(meshes) <= 65536 and len(draws) <= 65536, 'table entry bound')
    for index, mesh in enumerate(meshes, 1):
        require(isinstance(mesh, dict) and type(mesh.get('id')) is int
                and mesh['id'] == index and integer(mesh.get('provenance'), 0, 4),
                'mesh record')
        path = mesh.get('path')
        require(isinstance(path, str) and len(path.encode('utf-8')) <= 1024
                and ((not path and mesh['provenance'] == 4)
                     or (path and all(leaf(part) for part in path.split('/')))),
                'mesh path')
    joined, unsupported = {}, []
    for index, draw in enumerate(draws, 1):
        require(isinstance(draw, dict) and type(draw.get('id')) is int
                and draw['id'] == index and integer(draw.get('mesh'), 1, len(meshes))
                and integer(draw.get('lod'), 0, 255)
                and integer(draw.get('ownership'), 1, 3)
                and integer(draw.get('pass'), 1, 4), 'draw identity')
        require(isinstance(draw.get('chunk'), list) and len(draw['chunk']) == 3
                and all(integer(v, -2147483648, 2147483647) for v in draw['chunk'])
                and isinstance(draw.get('shader_variant'), str)
                and 0 < len(draw['shader_variant'].encode('utf-8')) <= 1024
                and draw.get('lod_convention') == 'zero_for_non_lod', 'draw metadata')
        supported = meshes[draw['mesh'] - 1]['provenance'] != 4
        require(type(draw.get('supported')) is bool and draw['supported'] == supported,
                'unsupported provenance')
        if not supported:
            unsupported.append(draw)
        joined[index] = (index, draw['mesh'], draw['lod'],
                         draw['ownership'] | draw['pass'] << 8)
    require(metadata.get('unsupported_producers') == unsupported
            and type(metadata.get('coverage_complete')) is bool
            and metadata['coverage_complete'] == (not unsupported), 'unsupported records')
    require(all(draw['pass'] != 1 for draw in unsupported), 'unsupported deferred producer')
    return joined


def header(stream, width, height, channels):
    # Three bounded header lines, then an exact dimension-derived payload size.
    magic = stream.readline(128)
    dims = stream.readline(128)
    scale = stream.readline(128)
    require(magic == (b'PF\n' if channels == 3 else b'Pf\n'), 'PFM channel header')
    require(dims == f'{width} {height}\n'.encode('ascii'), 'PFM dimensions')
    require(scale == b'-1.0\n', 'PFM little endian scale')
    require(os.fstat(stream.fileno()).st_size == stream.tell() + width * height * channels * 4,
            'PFM payload size')
    return stream.tell()


def read_exact(stream, count):
    data = stream.read(count)
    require(len(data) == count, 'partial payload read')
    return data


def aov(stream, offset, width, height, channels, coordinates):
    count = width * height * channels
    nonfinite = 0
    bad_material = False
    # At most 4096 bytes per scan chunk, independent of image dimensions.
    while count:
        batch = min(count, 1024)
        for (value,) in struct.iter_unpack('<f', read_exact(stream, batch * 4)):
            if not math.isfinite(value):
                nonfinite += 1
            if channels == 1 and (not math.isfinite(value) or value < 0
                                  or value > 255 or value != math.trunc(value)):
                bad_material = True
        count -= batch
    require(stream.read(1) == b'', 'trailing payload bytes')
    samples = []
    for x, y in coordinates:
        stream.seek(offset + ((height - 1 - y) * width + x) * channels * 4)
        samples.append(struct.unpack('<' + 'f' * channels, read_exact(stream, channels * 4)))
    return nonfinite, bad_material, samples


def identities(stream, width, height, joined, coordinates):
    magic, version, w, h = struct.unpack('<4sIII', read_exact(stream, 16))
    require(magic == b'LVID' and version == 1, 'unknown ID format version')
    require((w, h) == (width, height), 'ID dimensions')
    require(os.fstat(stream.fileno()).st_size == 16 + width * height * 16,
            'ID payload size')
    remaining = width * height
    while remaining:
        batch = min(remaining, 256)
        for pixel in struct.iter_unpack('<4I', read_exact(stream, batch * 16)):
            require(pixel == (0, 0, 0, 0) or
                    (pixel[0] in joined and joined[pixel[0]] == pixel
                     and pixel[3] >> 8 == 1), 'ID draw join')
        remaining -= batch
    require(stream.read(1) == b'', 'trailing payload bytes')
    samples = []
    for x, y in coordinates:
        stream.seek(16 + (y * width + x) * 16)
        samples.append(struct.unpack('<4I', read_exact(stream, 16)))
    return samples


def same_float(value, expected):
    if not math.isfinite(expected):
        return value is None
    # Match the binary32 value serialized by the producer, not decimal spelling.
    if not finite(value):
        return False
    try:
        return struct.pack('<f', value) == struct.pack('<f', expected)
    except (OverflowError, struct.error):
        return False


def probe(document_value, x, y, width, height, identity, samples):
    schema(document_value)
    expected = dict(origin='top_left', width=width, height=height, x=x, y=y,
                    background=identity == (0, 0, 0, 0), id=list(identity),
                    draw=identity[0], mesh=identity[1], lod=identity[2],
                    ownership=identity[3] & 255, **{'pass': identity[3] >> 8})
    require(all(type(document_value.get(k)) is type(v) and document_value[k] == v
                and (not isinstance(v, list) or
                     all(type(item) is int for item in document_value[k]))
                for k, v in expected.items()), 'probe identity or coordinates')
    for field, plane in (('albedo_linear', 'albedo'), ('normal_view', 'normal'),
                         ('depth_hardware', 'depth'), ('material', 'material')):
        expected_values = samples[plane]
        values = document_value.get(field)
        if len(expected_values) == 1:
            values = [values]
        require(field in document_value and isinstance(values, list) and len(values) == len(expected_values)
                and all(same_float(v, e) for v, e in zip(values, expected_values)),
                'probe plane ' + plane)
    nonfinite = any(not math.isfinite(v) for values in samples.values() for v in values)
    require(type(document_value.get('non_finite')) is bool
            and document_value['non_finite'] == nonfinite, 'probe nonfinite flag')


def load_rois(path):
    """Bound configuration work to 32 metrics and 1 MiB of JSON."""
    path = Path(path).absolute()
    with open_regular(path.parent, path) as stream:
        config = document(stream, 1024 * 1024)
    schema(config)
    rois = config.get('rois')
    require(isinstance(rois, list) and len(rois) <= 32, 'ROI count bound')
    names, count = set(), 0
    for roi in rois:
        require(isinstance(roi, dict) and leaf(roi.get('name'))
                and len(roi['name']) <= 80 and roi['name'] not in names, 'ROI name')
        names.add(roi['name'])
        rect = roi.get('rect')
        require(isinstance(rect, list) and len(rect) == 4 and all(finite(v) for v in rect)
                and 0 <= rect[0] < rect[2] <= 1 and 0 <= rect[1] < rect[3] <= 1,
                'normalized ROI rectangle')
        require(type(roi.get('enforce', False)) is bool, 'ROI enforcement')
        metrics = roi.get('metrics')
        require(isinstance(metrics, list) and bool(metrics), 'ROI metric list')
        count += len(metrics)
        require(count <= 32, 'metric count bound')
        for metric in metrics:
            require(isinstance(metric, dict), 'metric object')
            kind, channel = metric.get('kind'), metric.get('channel')
            require(isinstance(kind, str) and isinstance(channel, str), 'metric kind/channel')
            bounds = metric.get('thresholds')
            require(isinstance(bounds, list) and len(bounds) == 2
                    and all(finite(v) for v in bounds) and bounds[0] <= bounds[1],
                    'metric thresholds')
            if kind == 'coverage':
                require(channel == 'ids' and metric.get('field') in
                        ('background', 'draw', 'mesh', 'lod', 'ownership', 'pass'), 'ID class')
                maximum = dict(background=1, draw=65536, mesh=65536, lod=255,
                               ownership=3, **{'pass': 4})[metric['field']]
                require(integer(metric.get('value'), 0, maximum), 'ID class value')
            elif kind == 'validity':
                require(channel in PLANES, 'validity channel')
            elif kind == 'weather':
                require(channel in WEATHER, 'weather channel')
            elif kind == 'edge':
                require(channel in PLANES or channel in WEATHER, 'edge channel')
                require(metric.get('axis') in ('x', 'y'), 'edge axis')
            else:
                raise Invalid('unknown metric kind')
            require(bounds[0] >= 0 and (kind == 'edge' or bounds[1] <= 1),
                    'metric threshold range')
    return rois


def rectangle(rect, width, height):
    # Half-open ROI includes every pixel touched by the normalized rectangle.
    return (math.floor(rect[0] * width), math.floor(rect[1] * height),
            math.ceil(rect[2] * width), math.ceil(rect[3] * height))


def pixels_row(stream, offset, width, height, channels, y, left, right, ids=False):
    row = y if ids else height - 1 - y
    size = channels * 4
    stream.seek(offset + (row * width + left) * size)
    # One row: <= 262144 bytes (IDs) or 196608 bytes (RGB), width <= 16384.
    return list(struct.iter_unpack('<' + ('I' if ids else 'f') * channels,
                                  read_exact(stream, (right - left) * size)))


def metric_scan(stream, offset, width, height, channels, rect, metric):
    left, top, right, bottom = rect
    kind = metric['kind']
    total, count, worst, location, previous = 0.0, 0, -1.0, None, None
    minimum, maximum = math.inf, -math.inf
    for y in range(top, bottom):
        row = pixels_row(stream, offset, width, height, channels, y, left, right,
                         kind == 'coverage')
        for index, pixel in enumerate(row):
            x = left + index
            if kind == 'coverage':
                field = metric['field']
                value = (int(pixel == (0, 0, 0, 0)) if field == 'background' else
                         dict(draw=pixel[0], mesh=pixel[1], lod=pixel[2],
                              ownership=pixel[3] & 255, **{'pass': pixel[3] >> 8})[field])
                score = float(value == metric['value'])
            elif kind == 'validity':
                valid = all(math.isfinite(v) for v in pixel)
                if metric['channel'] == 'depth':
                    valid = valid and 0 <= pixel[0] <= 1
                elif metric['channel'] == 'material':
                    valid = valid and 0 <= pixel[0] <= 255 and pixel[0] == math.trunc(pixel[0])
                elif metric['channel'] == 'normal':
                    valid = valid and all(-1 <= v <= 1 for v in pixel)
                score = float(valid)
            elif kind == 'weather':
                if not all(math.isfinite(v) for v in pixel):
                    return None
                score = pixel[0]
                minimum, maximum = min(minimum, score), max(maximum, score)
            else:
                neighbor = (row[index - 1] if index else None) if metric['axis'] == 'x' else (
                    previous[index] if previous is not None else None)
                if neighbor is None:
                    continue
                if not all(math.isfinite(v) for v in pixel + neighbor):
                    return None
                score = max(abs(v - n) for v, n in zip(pixel, neighbor))
            count += 1
            total += score
            severity = (max(metric['thresholds'][0] - score, score - metric['thresholds'][1])
                        if kind != 'edge' else score)
            if severity > worst:
                worst, location = severity, [x, y]
        previous = row
    if not count:
        return None
    value = worst if kind == 'edge' else total / count
    return dict(value=value, samples=count, worst_pixel=location,
                **(dict(minimum=minimum, maximum=maximum) if kind == 'weather' else {}))


def image_metrics(streams, offsets, width, height, rois, crops=None):
    """At most 32 bounded ROI scans; missing/undefined values stay unavailable."""
    results = []
    for roi in rois:
        rect = rectangle(roi['rect'], width, height)
        for metric in roi['metrics']:
            channel, kind = metric['channel'], metric['kind']
            result = dict(roi=roi['name'], metric=dict(metric), rect=list(rect),
                          qualification='qualified' if roi.get('enforce', False) else 'advisory',
                          status='unavailable', value=None, worst_pixel=None)
            if channel in streams:
                channels = 4 if channel == 'ids' else PLANES.get(channel, 1)
                measured = metric_scan(streams[channel], offsets[channel], width, height,
                                       channels, rect, metric)
                if measured is not None:
                    low, high = metric['thresholds']
                    result.update(measured)
                    passed = (low <= measured['minimum'] and measured['maximum'] <= high
                              if kind == 'weather' else low <= measured['value'] <= high)
                    result['status'] = 'pass' if passed else 'fail'
            if crops is not None and result['worst_pixel'] is not None:
                x, y = result['worst_pixel']
                box = (max(0, x - 16), max(0, y - 16), min(width, x + 17), min(height, y + 17))
                pixels = [pixels_row(streams['albedo'], offsets['albedo'], width, height,
                                     3, row, box[0], box[2]) for row in range(box[1], box[3])]
                crops.append(dict(result=result, box=list(box), pixels=pixels))
            results.append(result)
    return results


def validate(completion, members, tree_root=None, rois=None, metrics=None, crops=None):
    """Return per-plane findings; malformed bundles raise Invalid immediately."""
    root, name = tree_root or completion.parent, completion.name
    prefix = name[:-len(SUFFIX)]
    require(leaf(prefix), 'capture filename')
    with ExitStack() as stack:
        snapshots = []

        def opened(filename):
            try:
                stream = stack.enter_context(open_regular(root, completion.parent / filename))
                info = os.fstat(stream.fileno())
                snapshots.append((stream, (info.st_size, info.st_mtime_ns, info.st_ctime_ns)))
                return stream
            except FileNotFoundError as error:
                raise Invalid('missing member: ' + filename) from error

        meta_stream = opened(name)
        meta = document(meta_stream, MAX_COMPLETION)
        require(integer(meta.get('schema'), 1, 2), 'unknown schema version')
        encodings = meta.get('encodings')
        contributions = ([c for c in WEATHER if isinstance(encodings, dict) and c in encodings]
                         if meta['schema'] == 2 else [])
        require(meta['schema'] == 1 or bool(contributions), 'unknown schema version')
        expected_encodings = dict(ENCODINGS, **{c: 'linear_contribution_float32' for c in contributions})
        require(meta.get('status') == 'complete' and meta.get('identity_complete') is True,
                'completion status')
        width, height = meta.get('width'), meta.get('height')
        require(integer(width, 1, 16384) and integer(height, 1, 16384)
                and width * height <= MAX_PIXELS, 'dimension bound')
        require(meta.get('origin') == 'top_left' and meta.get('surface') == 'deferred_only'
                and encodings == expected_encodings, 'attachment encodings')
        camera = meta.get('camera')
        require(isinstance(camera, dict) and matrix(camera.get('view'))
                and matrix(camera.get('projection')) and camera.get('matrix_order') == 'column_major'
                and type(camera.get('reversed_z')) is bool
                and integer(meta.get('frame'), 0, 18446744073709551615), 'frame camera')
        filenames = meta.get('files')
        require(isinstance(filenames, list) and 7 + len(contributions) <= len(filenames) <= 8 + len(contributions)
                and all(leaf(v) for v in filenames) and len(set(filenames)) == len(filenames),
                'manifest filenames')
        base = [prefix + '.' + plane + '.pfm' for plane in PLANES]
        base += [prefix + '.ids.bin', prefix + '.draws.json']
        base += [prefix + '.' + c + '.pfm' for c in contributions]
        extras = filenames[len(base):-1]
        coordinates = []
        for extra in extras:
            match = re.fullmatch(re.escape(prefix) + r'\.probe-([0-9]{1,5})-([0-9]{1,5})\.json', extra)
            require(match is not None, 'probe filename')
            x, y = map(int, match.groups())
            require(x < width and y < height, 'probe coordinate bound')
            coordinates.append((x, y))
        require(filenames == base + extras + [name] and set(filenames) == set(members),
                'manifest members')
        streams = {filename: opened(filename) for filename in filenames[:-1]}
        require(all(os.fstat(s.fileno()).st_mtime_ns <= os.fstat(meta_stream.fileno()).st_mtime_ns
                    for s in streams.values()), 'completion written last')
        manifest = meta.get('capture_manifest')
        require(leaf(manifest) and manifest == str(Path(prefix).with_suffix('.manifest.json')),
                'capture manifest name')
        capture = opened(prefix)
        require(os.fstat(capture.fileno()).st_size > 0, 'empty capture')
        capture_manifest = opened(manifest)
        require(all(os.fstat(s.fileno()).st_mtime_ns <= os.fstat(meta_stream.fileno()).st_mtime_ns
                    for s in (capture, capture_manifest)), 'completion written last')
        capture_meta = document(capture_manifest, 1024 * 1024)
        schema(capture_meta)
        resolution = capture_meta.get('resolution')
        require(capture_meta.get('status') == 'complete'
                and isinstance(resolution, dict)
                and type(resolution.get('width')) is int and type(resolution.get('height')) is int
                and capture_meta.get('resolution') == dict(width=width, height=height)
                and isinstance(capture_meta.get('camera'), dict)
                and all(matrix(capture_meta['camera'].get(k)) and capture_meta['camera'][k] == camera[k]
                        for k in ('view', 'projection')),
                'capture manifest mismatch')
        joined = tables(document(streams[prefix + '.draws.json']), meta)
        findings, samples, offsets, planes = [], {}, {}, {}
        for plane, channels in PLANES.items():
            stream = streams[prefix + '.' + plane + '.pfm']
            offset = header(stream, width, height, channels)
            offsets[plane], planes[plane] = offset, stream
            count, bad_material, samples[plane] = aov(stream, offset, width, height, channels, coordinates)
            if count:
                findings.append(f'{plane}: {count} nonfinite samples')
            require(plane != 'material' or not bad_material,
                    '; '.join(findings + ['material finite integral range']))
        ids = identities(streams[prefix + '.ids.bin'], width, height, joined, coordinates)
        for index, ((x, y), filename) in enumerate(zip(coordinates, extras)):
            probe(document(streams[filename]), x, y, width, height, ids[index],
                  {plane: values[index] for plane, values in samples.items()})
        for channel in contributions:
            stream = streams[prefix + '.' + channel + '.pfm']
            offsets[channel] = header(stream, width, height, 1)
            planes[channel] = stream
            count, _, _ = aov(stream, offsets[channel], width, height, 1, [])
            if count:
                findings.append(f'{channel}: {count} nonfinite samples')
        planes['ids'], offsets['ids'] = streams[prefix + '.ids.bin'], 16
        if rois is not None:
            results = image_metrics(planes, offsets, width, height, rois, crops)
            if metrics is not None:
                metrics.extend(results)
            for result in results:
                if result['qualification'] == 'qualified' and result['status'] != 'pass':
                    findings.append(f"{result['roi']}: {result['metric']['kind']} {result['status']}")
        for stream, before in snapshots:
            after = os.fstat(stream.fileno())
            require(before == (after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                    'member changed during validation')
        return findings


def discover(selected):
    """Enumerate at most 100,000 entries, without following directory links."""
    selected = selected.resolve()
    if selected.name.endswith(SUFFIX):
        parent, wanted = selected.parent, selected.name[:-len(SUFFIX)]
    else:
        parent, wanted = selected, None
    # The selected root may be an alias; descendant links are always refused.
    parent = parent.resolve(strict=True)
    pending, groups, entries = [parent], {}, 0
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as iterator:
            for entry in iterator:
                entries += 1
                require(entries <= MAX_ENTRIES, 'capture tree entry bound')
                match = re.fullmatch(r'(.+)\.(?:albedo\.pfm|normal\.pfm|depth\.pfm|'
                                     r'material\.pfm|precipitation\.pfm|fog\.pfm|cloud\.pfm|ids\.bin|draws\.json|probe-.*\.json|'
                                     r'diagnostics\.json(?:\.pending)?)', entry.name)
                if wanted is not None and (match is None or match[1] != wanted):
                    continue
                path = Path(entry.path)
                info = os.lstat(path)
                reparse = (getattr(info, 'st_file_attributes', 0) or 0) & 0x400
                require(not stat.S_ISLNK(info.st_mode) and not reparse,
                        'linked tree entry')
                if entry.is_dir(follow_symlinks=False):
                    if wanted is None:
                        pending.append(path)
                    continue
                if match:
                    groups.setdefault((directory, match[1]), []).append(entry.name)
    require(bool(groups), 'no diagnostic bundles found')
    return sorted((directory / (prefix + SUFFIX), members)
                  for (directory, prefix), members in groups.items())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--rois', type=Path, default=DEFAULT_ROIS)
    args = parser.parse_args(argv)
    invalid = False
    try:
        rois = load_rois(args.rois)
        selected = args.path.resolve()
        bundles = discover(selected)
        tree_root = selected.parent if selected.name.endswith(SUFFIX) else selected
        for completion, members in bundles:
            try:
                metrics = []
                findings = validate(completion, members, tree_root, rois, metrics)
                for result in metrics:
                    print(json.dumps(dict(bundle=str(completion), **result), allow_nan=False, sort_keys=True))
            except (Invalid, ValueError) as error:
                findings = [str(error)]
            for finding in findings:
                print(f'{completion}: {finding}')
            invalid |= bool(findings)
        if not invalid:
            print(f'diagnostic capture: {len(bundles)} valid bundle(s)')
        return int(invalid)
    except (Invalid, ValueError) as error:
        print(f'diagnostic capture: {error}', file=sys.stderr)
        return 1
    except (OSError, RuntimeError) as error:
        print(f'diagnostic capture I/O: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
