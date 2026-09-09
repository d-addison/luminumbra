#!/usr/bin/env python3
"""Distribution-aware render measurement validity; contains no performance budgets."""
import math
import re
import struct
from pathlib import Path
import perf

SCHEMA = 'luminumbra.render_benchmark.v3'
METRICS = ('frame_wall_ms', 'cpu_submit_ms', 'present_ms', 'gpu_frame_ms', 'gpu_pass_sum_ms')
PASSES = ('shadow', 'gbuffer', 'ssao', 'ssao_blur', 'lighting', 'water', 'skybox',
          'particle', 'foliage', 'aerial', 'final_blit')
STATS = ('p50', 'p95', 'p99', 'max', 'mad')
GPU_EPSILON_MS = 0.05  # Timestamp precision allowance, not a performance threshold.


def number(value, label, minimum=0):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < minimum:
        raise ValueError(f'{label}: unavailable, non-finite or out of range')
    return value


def integer(value, label, minimum=0, maximum=18000):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f'{label}: invalid integer')
    return value


def equal_number(actual, expected, label, tolerance=1e-7):
    number(actual, label)
    if not math.isclose(actual, expected, rel_tol=1e-7, abs_tol=tolerance):
        raise ValueError(f'{label}: {actual} != {expected}')


def traversal_manifest(text):
    tokens = iter(text.split())
    def key(expected):
        if next(tokens) != expected:
            raise ValueError(f'traversal expected {expected}')
    try:
        key('luminumbra.traversal.v1')
        result = {}
        for name, convert in [('duration_seconds', float), ('tick_rate', int), ('speed_mps', float),
                              ('seed', int), ('preset', str), ('preset_revision', int),
                              ('preset_identity', str), ('content_identity', str),
                              ('expected_ticks', int), ('cold_cache', str)]:
            key(name)
            result[name] = convert(next(tokens))
        key('points')
        count = integer(int(next(tokens)), 'traversal points', 2, 1024)
        path = []
        for _ in range(count):
            key('point')
            point = [float(next(tokens)) for _ in range(5)]
            if any(not math.isfinite(x) for x in point) or any(abs(x) > 32000 for x in point[:3]) or abs(point[4]) >= 90:
                raise ValueError('invalid traversal camera')
            path.append(point)
        if next(tokens, None) is not None:
            raise ValueError('unknown traversal field')
    except (StopIteration, OverflowError) as error:
        raise ValueError('incomplete traversal manifest') from error
    integer(result['expected_ticks'], 'expected_ticks', 1)
    integer(result['seed'], 'seed', 0, 2**32 - 1)
    if (result['tick_rate'] != 30 or result['preset_revision'] != 6 or
        result['cold_cache'] != 'true' or not result['preset'] or
        not re.fullmatch(r'[0-9a-f]{16}', result['preset_identity']) or
        not re.fullmatch(r'[0-9]{1,20}', result['content_identity']) or
        any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_-' for c in result['preset'])):
        raise ValueError('unsupported traversal settings')
    number(result['duration_seconds'], 'duration', 1e-12)
    number(result['speed_mps'], 'speed', 1e-12)
    if abs(result['duration_seconds'] * 30 - result['expected_ticks']) > 1e-9:
        raise ValueError('traversal duration/ticks disagree')
    lengths = [math.dist(a[:3], b[:3]) for a, b in zip(path, path[1:])]
    if min(lengths) <= 0 or sum(lengths) + 1e-9 < result['duration_seconds'] * result['speed_mps']:
        raise ValueError('invalid traversal path length')
    result['cold_cache'] = True
    result['path'] = path
    return result


def camera_sample(manifest, tick):
    remaining = manifest['speed_mps'] * tick / manifest['tick_rate']
    path = manifest['path']
    for i, (a, b) in enumerate(zip(path, path[1:])):
        length = math.dist(a[:3], b[:3])
        if remaining <= length or i == len(path) - 2:
            fraction = min(1, max(0, remaining / length))
            point = [x * (1 - fraction) + y * fraction for x, y in zip(a, b)]
            return {'tick': tick, 'position': point[:3], 'yaw': point[3], 'pitch': point[4]}
        remaining -= length
    raise ValueError('empty traversal')


def float32(value):
    return struct.unpack('f', struct.pack('f', value))[0]


def validate(data, qualified_renderer=None, qualified_vendor=None, traversal=None, workload=None):
    if data.get('schema') != SCHEMA:
        raise ValueError('unsupported render measurement schema')
    if data.get('gpu_timers_supported') is not True:
        raise ValueError('GPU timers unavailable')
    count = integer(data['frames'], 'frames', 1)
    warmup = integer(data['warmup_frames'], 'warmup_frames', 1)
    adapter = data['adapter']
    for key in ('vendor', 'renderer', 'version'):
        if not isinstance(adapter[key], str) or not adapter[key].strip():
            raise ValueError(f'adapter {key} unavailable')
    if not qualified_renderer:
        raise ValueError('v3 requires --qualified-renderer')
    if adapter['renderer'] != qualified_renderer or (qualified_vendor and adapter['vendor'] != qualified_vendor):
        raise ValueError('adapter does not match the qualified GPU')
    profile = data['profile']
    if profile['name'] not in ('legacy', 'quality', 'performance'):
        raise ValueError('unknown profile')
    scale = number(profile['render_scale'], 'profile scale', 0.5)
    if scale > 1:
        raise ValueError('invalid render scale')
    if profile['name'] != 'legacy':
        equal_number(scale, 1.0 if profile['name'] == 'quality' else 0.67, 'profile scale')
    equal_number(data['render_scale'], scale, 'reported scale')
    for axis in ('width', 'height'):
        size = integer(data[axis], axis, 1, 16384)
        if profile[axis] != size or data['internal_' + axis] != math.floor(float32(size * float32(scale)) + 0.5):
            raise ValueError('profile/capture dimensions disagree')
    overrides = data['debug_overrides']
    for kind in ('active', 'declared'):
        if not isinstance(overrides[kind], dict) or any(not isinstance(v, str) for v in overrides[kind].values()):
            raise ValueError('invalid override declaration')
    if overrides['active'] != overrides['declared']:
        raise ValueError('undeclared or mismatched render/calendar debug override')
    rows = data['frame_samples']
    if not isinstance(rows, list) or len(rows) != count:
        raise ValueError('incomplete frame sample coverage')
    for index, row in enumerate(rows):
        frame = warmup + index
        if type(row['frame']) is not int or type(row['gpu_frame']) is not int or row['frame'] != frame or row['gpu_frame'] != frame:
            raise ValueError('GPU result is not tagged with its originating frame')
        for metric in METRICS:
            number(row[metric], metric)
        if row['gpu_frame_ms'] + GPU_EPSILON_MS < row['gpu_pass_sum_ms']:
            raise ValueError('whole-frame GPU time is shorter than the pass sum')
        if set(row['passes']) != set(PASSES):
            raise ValueError('incomplete pass timer coverage')
        total = 0
        for name in PASSES:
            timer = row['passes'][name]
            if type(timer['issued']) is not bool:
                raise ValueError('invalid pass timer status')
            if timer['issued']:
                total += number(timer['ms'], name + ' timer')
            elif timer['ms'] is not None:
                raise ValueError('unissued timer must be unavailable, not zero')
        if not any(timer['issued'] for timer in row['passes'].values()):
            raise ValueError('no render pass timers issued')
        equal_number(row['gpu_pass_sum_ms'], total, 'pass sum')
    for metric in METRICS:
        values = [row[metric] for row in rows]
        block = data['distribution'][metric]
        if block['unit'] != 'ms' or type(block['sample_count']) is not int or block['sample_count'] != count or type(block['unavailable_count']) is not int or block['unavailable_count'] != 0:
            raise ValueError('incomplete timer coverage')
        expected = perf.summarize(values, 'ms')
        for stat in STATS:
            equal_number(block[stat], expected[stat], metric + '.' + stat)
        equal_number(data['avg'][metric], sum(values) / count, metric + ' mean')
    for name in PASSES:
        expected = sum(row['passes'][name]['ms'] or 0 for row in rows) / count
        equal_number(data['avg_ms'][name], expected, name + ' mean')
    equal_number(data['avg_ms']['total'], data['avg']['gpu_pass_sum_ms'], 'legacy total')
    equal_number(data['avg_ms']['ssao_total'], data['avg_ms']['ssao'] + data['avg_ms']['ssao_blur'], 'ssao total')
    excluded = data['excluded_windows']
    if not isinstance(excluded, list):
        raise ValueError('excluded_windows must be an array')
    ranges = []
    for window in excluded:
        first = integer(window['first_frame'], 'excluded start', 0, 2**32 - 1)
        last = integer(window['last_frame'], 'excluded end', first, 2**32 - 1)
        if not isinstance(window['reason'], str) or not window['reason'].strip():
            raise ValueError('excluded window needs a reason')
        if first < warmup + count and last >= warmup:
            raise ValueError('excluded window overlaps measured samples')
        if any(first <= b and last >= a for a, b in ranges):
            raise ValueError('overlapping excluded windows')
        ranges.append((first, last))
    if sorted(ranges) != [(0, warmup - 1), (warmup + count, warmup + count)]:
        raise ValueError('warmup/report exclusions are incomplete')
    observed = data['workload']
    integer(observed['seed'], 'workload seed', 0, 2**32 - 1)
    if observed['expected_frames'] != count or observed['preset_revision'] != 6 or not isinstance(observed['preset'], str) or not observed['preset']:
        raise ValueError('workload manifest does not match')
    if (not isinstance(observed.get('preset_identity'), str) or
        not re.fullmatch(r'[0-9a-f]{16}', observed['preset_identity']) or
        not isinstance(observed.get('content_identity'), str) or
        not re.fullmatch(r'[0-9]{1,20}', observed['content_identity'])):
        raise ValueError('missing or invalid workload content identities')
    if workload is not None:
        if not isinstance(workload, dict):
            raise ValueError('workload manifest must be an object')
        for key, value in workload.items():
            if observed.get(key) != value:
                raise ValueError(f'workload manifest does not match: {key}')
    if observed['kind'] == 'traversal':
        if traversal is None or observed['script'] != Path(traversal).read_bytes().decode('utf-8'):
            raise ValueError('traversal manifest does not match --traversal')
        manifest = traversal_manifest(observed['script'])
        for key in ('duration_seconds', 'tick_rate', 'speed_mps', 'seed', 'preset', 'preset_revision', 'preset_identity', 'content_identity', 'expected_ticks', 'cold_cache'):
            if observed[key] != manifest[key]:
                raise ValueError(f'traversal manifest does not match: {key}')
        if observed['actual_ticks'] != observed['expected_ticks'] or count != observed['expected_ticks']:
            raise ValueError('traversal tick count mismatch')
        if len(observed['camera_samples']) != count:
            raise ValueError('incomplete traversal camera samples')
        elapsed = number(observed['measured_duration_seconds'], 'measured traversal duration')
        if sum(row['frame_wall_ms'] for row in rows) / 1000 + 1e-6 < elapsed:
            raise ValueError('traversal duration exceeds measured wall coverage')
        previous_tick = 0
        for actual in observed['camera_samples']:
            tick = integer(actual['tick'], 'camera tick', previous_tick, manifest['expected_ticks'])
            if tick != previous_tick + 1:
                raise ValueError('traversal camera must advance one simulation tick per frame')
            expected = camera_sample(manifest, tick)
            previous_tick = tick
            if len(actual['position']) != 3:
                raise ValueError('invalid traversal camera sample')
            for a, e in zip(actual['position'] + [actual['yaw'], actual['pitch']], expected['position'] + [expected['yaw'], expected['pitch']]):
                if not isinstance(a, (float, int)) or not math.isfinite(a) or not math.isclose(a, e, rel_tol=1e-7, abs_tol=0.002):
                    raise ValueError('traversal camera differs from script')
        if observed['camera_samples'][0]['tick'] != 1 or previous_tick != manifest['expected_ticks']:
            raise ValueError('traversal camera samples do not cover the complete path')
    elif observed['kind'] != 'fixed_view' or traversal is not None:
        raise ValueError('unknown or mismatched workload')
    return {'verdict': 'PASS', 'scope': 'v3 measurement validity; no performance thresholds', 'frames': count}
