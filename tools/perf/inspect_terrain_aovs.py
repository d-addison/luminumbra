#!/usr/bin/env python3
"""Inspect raw terrain AOVs without launching the renderer or changing artifacts.

Requires numpy. Legacy forward-depth packets require an explicit option; depth
conventions are never inferred from pixel values. Raw packet validation does not
establish executable/input provenance, performance acceptance or visual approval.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np

LEGACY_DEPTH_DESCRIPTION = 'float32 OpenGL window depth [0,1], clear=1; bottom-up'
DEPTH_DESCRIPTION = ('float32 reversed-Z OpenGL window depth [0,1], '
                     'near=1, far/clear=0; bottom-up')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def depth_convention(manifest, legacy_forward=False):
    convention = manifest.get('depth_convention')
    legacy = convention is None
    if legacy:
        require(legacy_forward, 'Depth convention missing; historical packets require --legacy-forward-depth')
        require(manifest['attachments']['depth.pfm'] == LEGACY_DEPTH_DESCRIPTION,
                'Unknown legacy depth declaration')
        # The historical production camera used these fixed clipping planes.
        convention = dict(schema='luminumbra.depth_convention.v1', direction='forward',
                          clip_depth_range='negative_one_to_one', clear_value=1.0,
                          near_plane_depth=0.0, far_plane_depth=1.0, near_plane_m=0.1,
                          far_plane_m=3200.0, storage_format='GL_DEPTH_COMPONENT24')
    require(isinstance(convention, dict), 'Invalid depth convention')
    expected = dict(schema='luminumbra.depth_convention.v1', direction='reversed',
                    clip_depth_range='zero_to_one', clear_value=0.0, near_plane_depth=1.0,
                    far_plane_depth=0.0, storage_format='GL_DEPTH_COMPONENT32F')
    if legacy:
        expected.update(direction='forward', clip_depth_range='negative_one_to_one',
                        clear_value=1.0, near_plane_depth=0.0, far_plane_depth=1.0,
                        storage_format='GL_DEPTH_COMPONENT24')
    for key, value in expected.items():
        require(convention.get(key) == value and not isinstance(convention.get(key), bool),
                'Unsupported or inconsistent depth convention: ' + key)
    for key in ('near_plane_m', 'far_plane_m'):
        value = convention.get(key)
        require(type(value) in (int, float) and math.isfinite(value) and value > 0,
                'Invalid clipping plane: ' + key)
    require(convention['near_plane_m'] < convention['far_plane_m'], 'Invalid clipping interval')
    if not legacy:
        require(manifest['attachments']['depth.pfm'] == DEPTH_DESCRIPTION,
                'Depth attachment description contradicts its convention')
    projection = np.asarray(manifest['gbuffer_projection'], dtype=float).reshape(4, 4, order='F')
    require(np.isfinite(projection).all(), 'Non-finite projection')
    for distance, expected_depth in ((convention['near_plane_m'], convention['near_plane_depth']),
                                     (convention['far_plane_m'], convention['far_plane_depth'])):
        clip = projection @ np.array([0, 0, -distance, 1])
        require(clip[3] > 0, 'Projection puts a clipping plane behind the camera')
        depth = clip[2] / clip[3]
        if legacy:
            depth = depth * 0.5 + 0.5
        require(abs(depth - expected_depth) <= 1e-5,
                'Projection endpoints contradict the declared depth direction')
    return dict(convention, legacy_explicit=legacy)


def coverage_mask(depth, convention):
    require(np.isfinite(depth).all() and ((depth >= 0) & (depth <= 1)).all(),
            'Depth samples must be finite window depths in [0,1]')
    return depth < 1 if convention['direction'] == 'forward' else depth > 0


def plane(path, dims, magic, channels):
    require(isinstance(dims, list) and len(dims) == 2 and
            all(type(v) is int and v > 0 for v in dims), 'Invalid plane dimensions')
    require(dims[0] * dims[1] <= 16 * 1024 * 1024, 'Plane exceeds producer bound')
    with path.open('rb') as stream:
        require(stream.readline(4096) == magic + b'\n', 'Unexpected plane magic: ' + path.name)
        require(tuple(map(int, stream.readline(4096).split())) == tuple(dims),
                'Plane dimensions disagree with manifest: ' + path.name)
        encoding = stream.readline(4096).strip()
        if magic in (b'PF', b'Pf'):
            require(encoding in (b'-1.0', b'1.0'), 'Unexpected PFM scale')
            dtype = np.dtype('<f4' if encoding == b'-1.0' else '>f4')
        else:
            require(encoding == b'255', 'Unexpected byte-plane maximum')
            dtype = np.dtype('u1')
        offset = stream.tell()
    width, height = dims
    require(path.stat().st_size == offset + width * height * channels * dtype.itemsize,
            'Truncated or trailing plane payload: ' + path.name)
    data = np.memmap(path, mode='r', offset=offset, dtype=dtype, shape=(height, width, channels))
    if magic in (b'PF', b'Pf'):
        require(np.isfinite(data).all(), 'Non-finite plane: ' + path.name)
        data = data[::-1]
    return data


def inspect(directory, legacy_forward=False):
    manifest_path = directory / 'manifest.json'
    require(manifest_path.stat().st_size <= 16 * 1024 * 1024, 'Oversized AOV manifest')
    manifest = json.loads(manifest_path.read_text())
    require(manifest['schema'] == 'luminumbra.terrain_coverage_capture.v1' and
            manifest['complete'] is True and manifest['enabled'] is True, 'Incomplete AOV capture')
    require(manifest['matrix_layout'] == 'column_major', 'Unknown matrix layout')
    require(manifest['capture_phase'] ==
            'same_rendered_frame_after_postprocessing_before_present_unmeasured', 'Unknown capture phase')
    require(type(manifest['render_frame']) is int and manifest['render_frame'] >= 0, 'Invalid render frame')
    for name in ('view', 'gbuffer_projection'):
        values = np.asarray(manifest[name], dtype=float)
        require(values.shape == (16,) and np.isfinite(values).all(), 'Invalid matrix: ' + name)
    convention = depth_convention(manifest, legacy_forward)
    arrays = {}
    for name, magic, channels in [('color.ppm', b'P6', 3), ('depth.pfm', b'Pf', 1),
                                  ('position.pfm', b'PF', 3), ('normal.pfm', b'PF', 3),
                                  ('albedo.pfm', b'PF', 3), ('material.pgm', b'P5', 1)]:
        dims = manifest['color_dimensions'] if name == 'color.ppm' else manifest['gbuffer_dimensions']
        arrays[name] = plane(directory / name, dims, magic, channels)
    require(set(manifest['attachments']) == set(arrays), 'Incomplete attachment declarations')
    solid = coverage_mask(arrays['depth.pfm'][:, :, 0], convention)
    require(((arrays['albedo.pfm'] >= 0) & (arrays['albedo.pfm'] <= 1)).all(), 'Invalid albedo range')
    normals = arrays['normal.pfm'][solid]
    require(not len(normals) or np.max(np.abs(np.linalg.norm(normals, axis=1) - 1)) < 1e-4,
            'Covered-pixel normals are not normalized')
    far = manifest['far']
    require(far['wanted'] == far['resident_wanted'] + far['missing_after_eviction'],
            'Far residency accounting mismatch')
    report = dict(render_frame=manifest['render_frame'], depth_convention=convention,
                  manifest_sha256=hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                  coverage_fraction=float(solid.mean()), covered_pixels=int(solid.sum()),
                  clear_depth_pixels=int((~solid).sum()),
                  attachments={name: dict(bytes=(directory / name).stat().st_size,
                      sha256=hashlib.sha256((directory / name).read_bytes()).hexdigest()) for name in arrays})
    return manifest, arrays, report


def validate_benchmark(manifest, benchmark):
    require(benchmark['schema'] == 'luminumbra.render_benchmark.v2', 'Unknown benchmark schema')
    require([benchmark['width'], benchmark['height']] == manifest['color_dimensions'] and
            [benchmark['internal_width'], benchmark['internal_height']] == manifest['gbuffer_dimensions'],
            'Benchmark/AOV dimensions mismatch')
    coverage = benchmark['terrain_coverage']
    require(coverage['capture_complete'] is True, 'Benchmark capture incomplete')
    rows = coverage['frame_observations']
    frames = [row['render_frame'] for row in rows]
    require(all(type(frame) is int for frame in frames) and
            all(a < b for a, b in zip(frames, frames[1:])), 'Unordered or duplicate observations')
    captures = [row for row in rows if row['benchmark_phase'] == 'capture']
    require(len(captures) == 1 and captures[0]['render_frame'] == manifest['render_frame'] and
            captures[0]['far'] == manifest['far'], 'Benchmark capture-frame/AOV state mismatch')
    return dict(capture_frame=manifest['render_frame'], observation_count=len(rows))


def compare(first, second):
    ma, a, ra = first
    mb, b, rb = second
    require(ra['depth_convention'] == rb['depth_convention'], 'Cannot compare different depth conventions')
    for key in ('view', 'gbuffer_projection', 'gbuffer_dimensions', 'color_dimensions', 'time_of_day'):
        require(np.allclose(ma[key], mb[key], rtol=0, atol=1e-7), 'Comparison mismatch: ' + key)
    require(ma['render_frame'] == mb['render_frame'], 'Comparison frame mismatch')
    require(ma['taau_enabled'] is False and mb['taau_enabled'] is False and
            ma['jitter_ndc'] == mb['jitter_ndc'] == [0.0, 0.0], 'Comparison requires unjittered non-temporal captures')
    sa = coverage_mask(a['depth.pfm'][:, :, 0], ra['depth_convention'])
    sb = coverage_mask(b['depth.pfm'][:, :, 0], rb['depth_convention'])
    return dict(newly_covered_pixels=int((~sa & sb).sum()), newly_clear_pixels=int((sa & ~sb).sum()),
                limitation='Separate runs can differ in residency; establish input/binary identity and inspect region records before assigning a cause.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('aov_directory', type=Path)
    parser.add_argument('--benchmark', type=Path)
    parser.add_argument('--compare', type=Path, help='Second AOV directory with matching camera/frame/depth convention')
    parser.add_argument('--legacy-forward-depth', action='store_true')
    args = parser.parse_args(argv)
    try:
        first = inspect(args.aov_directory, args.legacy_forward_depth)
        report = dict(verdict='PASS', first=first[2])
        if args.benchmark:
            report['benchmark_join'] = validate_benchmark(first[0], json.loads(args.benchmark.read_text()))
        if args.compare:
            second = inspect(args.compare, args.legacy_forward_depth)
            report.update(second=second[2], comparison=compare(first, second))
        report['scope'] = 'Raw packet depth/format and requested joins only; no executable/input provenance, performance acceptance or visual approval.'
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        print(json.dumps(dict(verdict='FAIL', reason=str(error)), indent=2))
        return 1
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
