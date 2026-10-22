#!/usr/bin/env python3
"""Distribution-aware render measurement validity; contains no performance budgets."""
import functools
import json
import math
import re
import struct
from pathlib import Path

from fixture_paths import read_bytes
import perf

SCHEMA = 'luminumbra.render_benchmark.v3'
WORKLOAD_SCHEMA = 'luminumbra.render_benchmark.v4'
SCHEMAS = (SCHEMA, WORKLOAD_SCHEMA)
WORKLOADS = ('cave', 'edit', 'region_crossing', 'save_load', 'menu_entry')
ACTION_SEQUENCES = {
    'edit': ['dig', 'fill'], 'region_crossing': [], 'save_load': ['save', 'reload'],
    'menu_entry': ['menu_open', 'settings_open', 'settings_close', 'menu_close',
                   'main_menu', 'enter_world'],
}
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
    if not isinstance(text, str) or not text.isascii():
        raise ValueError('traversal fields must use ASCII tokens and whitespace')
    if len(text.encode("utf-8")) > 1024 * 1024:
        raise ValueError("traversal manifest exceeds 1 MiB")
    tokens = iter(text.split())
    def key(expected):
        if next(tokens) != expected:
            raise ValueError(f'traversal expected {expected}')
    try:
        version = next(tokens)
        if version not in ('luminumbra.traversal.v1', 'luminumbra.traversal.v2'):
            raise ValueError('unsupported traversal version')
        scripted = version.endswith('.v2')
        result = {}
        if scripted:
            key('workload')
            result['kind'] = next(tokens)
            if result['kind'] not in WORKLOADS:
                raise ValueError('unsupported scripted workload')
        for name, convert in [('duration_seconds', float), ('tick_rate', int), ('speed_mps', float),
                              ('seed', int), ('preset', str), ('preset_revision', int),
                              ('preset_identity', str), ('content_identity', str),
                              ('expected_ticks', int), ('cold_cache', str)]:
            key(name)
            result[name] = convert(next(tokens))
        key('points')
        count = integer(int(next(tokens)), 'traversal points', 2, 1024)
        path = []
        point_ticks = []
        for _ in range(count):
            key('point')
            if scripted:
                tick = integer(int(next(tokens)), 'camera key tick')
                if (not point_ticks and tick != 0) or (point_ticks and tick <= point_ticks[-1]):
                    raise ValueError('invalid camera key tick')
                point_ticks.append(tick)
            point = [float(next(tokens)) for _ in range(5)]
            if any(not math.isfinite(x) for x in point) or any(abs(x) > 32000 for x in point[:3]) or abs(point[4]) >= 90:
                raise ValueError('invalid traversal camera')
            path.append(point)
        if scripted:
            if point_ticks[-1] != result['expected_ticks']:
                raise ValueError('camera keys must cover every script tick')
            result['point_ticks'] = point_ticks
            key('actions')
            action_count = integer(int(next(tokens)), 'action count', 0, 64)
            actions = []
            for _ in range(action_count):
                key('action')
                tick = integer(int(next(tokens)), 'action tick', 0, result['expected_ticks'] - 1)
                name = next(tokens)
                position = [float(next(tokens)) for _ in range(3)]
                radius = number(float(next(tokens)), 'action radius')
                edit = name in ('dig', 'fill')
                if (any(not math.isfinite(v) or abs(v) > 32000 for v in position) or
                    (edit and not 0 < radius <= 16) or
                    (not edit and (radius != 0 or any(position))) or
                    (tick == 0 and (result['kind'] != 'cave' or name != 'dig')) or
                    (actions and tick < actions[-1]['tick'])):
                    raise ValueError('invalid scripted action')
                actions.append(dict(tick=tick, name=name, position=position, radius=radius))
            names = [action['name'] for action in actions]
            if result['kind'] == 'cave':
                if not actions or any(a['tick'] != 0 or a['name'] != 'dig' for a in actions):
                    raise ValueError('cave requires tick-zero tunnel preparation')
            elif names != ACTION_SEQUENCES[result['kind']]:
                raise ValueError('actions do not match workload')
            elif any(a['tick'] >= b['tick'] for a, b in zip(actions, actions[1:])):
                raise ValueError('actions require distinct ticks')
            result['actions'] = actions
        if next(tokens, None) is not None:
            raise ValueError('unknown traversal field')
    except (StopIteration, OverflowError) as error:
        raise ValueError('incomplete traversal manifest') from error
    integer(result['expected_ticks'], 'expected_ticks', 1)
    integer(result['seed'], 'seed', 0, 2**32 - 1)
    if (result['tick_rate'] != 30 or result['preset_revision'] not in (6, 7, 8) or
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
    if not scripted and (min(lengths) <= 0 or sum(lengths) + 1e-9 < result['duration_seconds'] * result['speed_mps']):
        raise ValueError('invalid traversal path length')
    result['cold_cache'] = True
    result['path'] = path
    return result


def camera_sample(manifest, tick):
    integer(tick, 'camera tick', 0, manifest['expected_ticks'])
    if 'point_ticks' in manifest:
        for index, end in enumerate(manifest['point_ticks'][1:], 1):
            if tick <= end:
                start = manifest['point_ticks'][index - 1]
                fraction = (tick - start) / (end - start)
                point = [a + (b - a) * fraction for a, b in
                         zip(manifest['path'][index - 1], manifest['path'][index])]
                return dict(tick=tick, position=point[:3], yaw=point[3], pitch=point[4])
        raise ValueError('camera tick outside manifest')
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
    return struct.unpack('<f', struct.pack('<f', value))[0]


@functools.lru_cache(maxsize=1)
def render_registry():
    """Read the authored registry used to generate the native runtime tables."""
    schema = json.loads((Path(__file__).resolve().parents[2] /
                         'src/luminumbra_common/core/ConfigSchema.json').read_text(encoding='utf-8'))
    keys = {key['enum']: key['json_name'] for key in schema['keys'] if key['section'] == 'render'}
    return {name: {p['json_name']: p for p in schema['params'] if p['owner'] == owner}
            for owner, name in keys.items()}


def render_identity(settings):
    """FNV-1a over typed canonical bytes; mirrors RenderConfiguration.h."""
    digest = 14695981039346656037
    def emit(payload):
        nonlocal digest
        for byte in payload:
            digest = ((digest ^ byte) * 1099511628211) & (2**64 - 1)
    def visit(value):
        if isinstance(value, dict):
            emit(b'{')
            for key in sorted(value):
                if not isinstance(key, str):
                    raise ValueError('render configuration keys must be strings')
                visit(key)
                visit(value[key])
            emit(b'}')
        elif isinstance(value, list):
            emit(b'[')
            for item in value:
                visit(item)
            emit(b']')
        elif isinstance(value, str):
            payload = value.encode('utf-8')
            emit(b's' + str(len(payload)).encode('ascii') + b':' + payload)
        elif type(value) is bool:
            emit(b't' if value else b'f')
        elif type(value) is float and math.isfinite(value):
            emit(b'd' + struct.pack('<d', value))
        elif type(value) is int:
            emit(b'i' + str(value).encode('ascii') + b';')
        elif value is None:
            emit(b'n')
        else:
            raise ValueError('unsupported or non-finite render configuration value')
    visit(settings)
    return f'fnv1a64:{digest:016x}'


def validate_render_identity(data):
    settings = data['settings']
    if (not isinstance(settings['simulation_identity'], str) or
        not re.fullmatch(r'[0-9a-f]{16}', settings['simulation_identity'])):
        raise ValueError('invalid effective simulation identity')
    content = settings['content_files']
    if not isinstance(content, dict) or not content or len(content) > 65539:
        raise ValueError('missing or excessive workload content manifest')
    # The complete data/resource inventory is conservative: indirect model,
    # texture, species, particle and UI dependencies participate too.
    for required in ('data/common/systems.json', 'data/common/foliage/scatter_set.json',
                     'config/game-asset-packs.json'):
        if required not in content:
            raise ValueError('missing workload content digest')
    if not any(path.startswith('res/') for path in content):
        raise ValueError('missing workload resource digests')
    if not any(path.startswith('game-assets/') for path in content):
        raise ValueError('missing workload asset pack digests')
    for path, digest in content.items():
        if path == 'assets/audio' and digest is None:
            if any(isinstance(name, str) and name.startswith('assets/audio/') for name in content):
                raise ValueError('absent audio tree contains workload content')
            continue
        if (not isinstance(path, str) or len(path.encode('utf-8')) > 4096 or
            not path.startswith(('data/', 'res/', 'config/', 'game-assets/', 'assets/audio/', 'inputs/')) or
            '\\' in path or ':' in path or
            any(part in ('', '.', '..') for part in path.split('/')) or
            not isinstance(digest, str) or not re.fullmatch(r'fnv1a64:[0-9a-f]{16}', digest)):
            raise ValueError('invalid workload content digest')
    systems = settings['effective_render']['systems']
    registry = render_registry()
    if not isinstance(systems, dict) or set(systems) != set(registry):
        raise ValueError('incomplete resolved render registry')
    for name, params in registry.items():
        entry = systems[name]
        if type(entry['enabled']) is not bool or set(entry['params']) != set(params):
            raise ValueError('incomplete resolved render flags or parameters')
        for key, param in params.items():
            value = entry['params'][key]
            values = value if param['type'] == 'vec3' else [value]
            if not isinstance(values, list) or len(values) != (3 if param['type'] == 'vec3' else 1):
                raise ValueError('invalid resolved render parameter')
            for number_value in values:
                number(number_value, 'resolved render parameter', -math.inf)
    if settings['render_overrides'] != data['debug_overrides']['active']:
        raise ValueError('render identity overrides disagree')
    expected = render_identity(settings)
    if data['render_identity'] != expected:
        raise ValueError('render identity does not match resolved configuration')
    return expected


def require_matching_render_identity(first, second):
    # Older reports remain valid individually, but lack a complete render identity
    # and cannot establish that two captures used the same resolved settings.
    if any(data.get('schema') != WORKLOAD_SCHEMA for data in (first, second)):
        raise ValueError('capture comparison requires complete v4 render identities')
    if validate_render_identity(first) != validate_render_identity(second):
        raise ValueError('capture render identity differs')
    # Presentation phases include warmup even though settings identity does not.
    offsets = [integer(data['warmup_frames'], 'warmup_frames', 1) for data in (first, second)]
    if offsets[0] != offsets[1]:
        raise ValueError('capture warmup/presentation offset differs')


def validate(data, qualified_renderer=None, qualified_vendor=None, traversal=None, workload=None):
    if data.get('schema') not in SCHEMAS:
        raise ValueError('unsupported render measurement schema')
    if data['schema'] == WORKLOAD_SCHEMA:
        validate_workload(data, traversal_manifest(data['workload']['script']))
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
        if (not any(timer['issued'] for timer in row['passes'].values()) and
            not (data['schema'] == WORKLOAD_SCHEMA and
                 data['workload']['kind'] == 'menu_entry' and
                 data['workload']['observations'][index]['ui_document'] in
                 ('pause', 'settings', 'main_menu'))):
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
    if observed['expected_frames'] != count or observed['preset_revision'] not in (6, 7, 8) or not isinstance(observed['preset'], str) or not observed['preset']:
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
    if observed['kind'] == 'traversal' or (data['schema'] == WORKLOAD_SCHEMA and observed['kind'] in WORKLOADS):
        if traversal is None or observed['script'] != read_bytes(Path(traversal).parent, Path(traversal)).decode('utf-8'):
            raise ValueError('traversal manifest does not match --traversal')
        manifest = traversal_manifest(observed['script'])
        if (data['schema'] == WORKLOAD_SCHEMA) != ('point_ticks' in manifest):
            raise ValueError('workload script/artifact versions disagree')
        if observed['kind'] != manifest.get('kind', 'traversal'):
            raise ValueError('workload kind differs from script')
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
    return {'verdict': 'PASS', 'scope': 'render measurement validity; no performance thresholds', 'frames': count}


def script_steps(text):
    """Expand the existing traversal format into bounded, deterministic tick steps."""
    manifest = traversal_manifest(text)
    actions = manifest.get('actions', [])
    by_tick = {}
    for action in actions:
        by_tick.setdefault(action['tick'], []).append(action)
    return [dict(camera=camera_sample(manifest, tick), actions=by_tick.get(tick, []))
            for tick in range(manifest['expected_ticks'] + 1)]


def distribution(block, values):
    if not values or block['unit'] != 'ms':
        raise ValueError('window requires frame samples in ms')
    for value in values:
        number(value, 'window frame time')
    if (type(block['sample_count']) is not int or block['sample_count'] != len(values) or
        type(block['unavailable_count']) is not int or block['unavailable_count'] != 0):
        raise ValueError('incomplete window sample counts')
    expected = perf.summarize(values, 'ms')
    for stat in STATS:
        equal_number(block[stat], expected[stat], 'window.' + stat)


def validate_workload(data, manifest):
    """V4 adds strict settings, separate windows, and observed action/state evidence."""
    observed = data['workload']
    kind = observed['kind']
    if kind not in WORKLOADS or manifest.get('kind') != kind:
        raise ValueError('unknown or mismatched scripted workload')
    count = integer(data['frames'], 'frames', 1)
    warmup = integer(data['warmup_frames'], 'warmup', 1)
    settings = data['settings']
    validate_render_identity(data)
    if type(settings['gns_enabled']) is not bool:
        raise ValueError('GNS setting must be boolean')
    regions = settings['active_regions']
    if type(regions['enabled']) is not bool:
        raise ValueError('active-region setting must be boolean')
    integer(regions['work_limit'], 'region work limit', 0, 2**64 - 1)
    integer(regions['hold_ticks'], 'region hold ticks', 1, 2**32 - 1)
    integer(regions['reduced_shift'], 'region reduced shift', 1, 30)
    if (settings['graphics_preset_status'] != 'named' or
        not isinstance(settings['graphics_preset'], str) or not settings['graphics_preset']):
        raise ValueError('invalid graphics preset provenance')
    render = settings['effective_render']
    equal_number(render['render_scale'], data['render_scale'], 'effective render scale')
    for axis in ('width', 'height'):
        if type(render[axis]) is not int or render[axis] != data[axis]:
            raise ValueError('effective render dimensions disagree')
    for key in ('vsync', 'taau'):
        if type(render[key]) is not bool:
            raise ValueError('invalid effective render setting')
    if render['vsync']:
        raise ValueError('benchmark requires uncapped presentation')
    integer(render['cloud_quality'], 'cloud quality', 0, 2)
    number(render['fov'], 'field of view', 1)
    if number(render['ui_scale'], 'UI scale', 0.5) > 2.5:
        raise ValueError('invalid effective UI scale')
    if render['fov'] >= 180 or render['window_mode'] not in ('windowed', 'borderless', 'fullscreen', 'headless'):
        raise ValueError('invalid effective field of view or window mode')
    if render['window_mode'] == 'headless':
        raise ValueError('headless v4 captures cannot qualify native rendering')
    integer(render['ssao_quality'], 'ssao quality', 0, 3)
    integer(render['volumetric_quality'], 'volumetric quality', 0, 2)
    windows = data['windows']
    number(windows['startup']['elapsed_ms'], 'startup elapsed')
    for name in ('startup', 'warmup', 'measured'):
        foliage = windows[name]['foliage']
        slots = integer(foliage['candidate_slots'], 'foliage candidate slots', 0, 2**64 - 1)
        candidates = integer(foliage['candidates'], 'actual foliage candidates', 0, slots)
        integer(foliage['instances'], 'actual foliage instances', 0, candidates)
        if foliage['backend'] not in ('cpu', 'gpu') or foliage['observation'] != 'window_end':
            raise ValueError('invalid foliage observation')
    samples = windows['warmup']['frame_wall_samples_ms']
    if not isinstance(samples, list) or len(samples) != warmup:
        raise ValueError('incomplete warmup samples')
    distribution(windows['warmup']['distribution'], samples)
    distribution(windows['measured']['distribution'], [r['frame_wall_ms'] for r in data['frame_samples']])
    if windows['measured']['distribution'] != data['distribution']['frame_wall_ms']:
        raise ValueError('measured window distribution differs')
    actions = observed['actions']
    if not isinstance(actions, list) or len(actions) != len(manifest['actions']):
        raise ValueError('incomplete action evidence')
    for actual, expected in zip(actions, manifest['actions']):
        if (type(actual['tick']) is not int or actual['tick'] != expected['tick'] or
            actual['name'] != expected['name'] or actual['succeeded'] is not True):
            raise ValueError('action evidence differs from script')
        integer(actual['modified_chunks'], 'modified chunks', 1 if kind == 'edit' else 0, 2**32 - 1)
    rows = observed['observations']
    if not isinstance(rows, list) or len(rows) != count:
        raise ValueError('incomplete workload observations')
    state, paused = 'gameplay', False
    document = 'hud'
    event_index = 0
    transitions = []
    underground = 0
    for tick, row in enumerate(rows, 1):
        if (type(row['tick']) is not int or row['tick'] != tick or
            type(row['frame']) is not int or row['frame'] != warmup + tick - 1):
            raise ValueError('workload observation frame/tick mismatch')
        equal_number(row['presentation_seconds'], (warmup + tick) / manifest['tick_rate'],
                     'script presentation time')
        while event_index < len(actions) and actions[event_index]['tick'] <= tick:
            name = actions[event_index]['name']
            if name == 'main_menu':
                state, document = 'main_menu', 'main_menu'
            elif name == 'enter_world':
                state, document = 'gameplay', 'hud'
            elif name == 'menu_open':
                paused, document = True, 'pause'
            elif name == 'menu_close':
                paused, document = False, 'hud'
            elif name == 'settings_open':
                document = 'settings'
            elif name == 'settings_close':
                document = 'pause'
            event_index += 1
        if (not isinstance(row['ui_document'], str) or
            (kind == 'menu_entry' and row['ui_document'] != document)):
            raise ValueError('visible UI document differs from script')
        if row['state'] != state or type(row['paused']) is not bool or row['paused'] != paused:
            raise ValueError('observed menu state differs from script')
        integer(row['foliage_build_generation'], 'foliage build generation', 0, 2**64 - 1)
        integer(row['terrain_uploads'], 'terrain uploads', 0, 2**32 - 1)
        for key in ('stream_queue_depth', 'stream_generation_batches', 'stream_meshing_batches',
                    'stream_pending_lod_chunks', 'terrain_upload_candidates', 'water_upload_candidates'):
            integer(row[key], key, 0, 2**64 - 1)
        for key in ('settled', 'underground', 'camera_in_air', 'foliage_pending', 'world_ready',
                    'stream_promotion_pending', 'region_scheduler_fault'):
            if type(row[key]) is not bool:
                raise ValueError('invalid workload observation flag')
        expected_settled = (state == 'gameplay' and row['world_ready'] and
                            not row['region_scheduler_fault'] and
                            row['stream_generation_batches'] == 0 and row['stream_meshing_batches'] == 0 and
                            row['stream_pending_lod_chunks'] == 0 and not row['stream_promotion_pending'] and
                            row['stream_queue_depth'] == 0 and row['terrain_upload_candidates'] == 0 and
                            row['water_upload_candidates'] == 0 and not row['foliage_pending'])
        if row['settled'] != expected_settled:
            raise ValueError('settled marker disagrees with observed work queues')
        if row['far_requested'] not in ('legacy', 'ladder') or row['far_effective'] not in ('legacy', 'ladder'):
            raise ValueError('invalid far mode observation')
        for key in ('cave_requested', 'cave_effective'):
            if row[key] is not None:
                integer(row[key], key, 0, 1)
        if row['far_effective'] == 'legacy' and row['cave_effective'] is not None:
            raise ValueError('legacy far mode cannot have an effective cave policy')
        region = row['region']
        position = camera_sample(manifest, tick)['position']
        if (not isinstance(region, list) or len(region) != 2 or
            any(type(v) is not int for v in region) or
            region != [math.floor(float32(position[0]) / 512), math.floor(float32(position[2]) / 512)]):
            raise ValueError('region does not match scripted camera')
        if not isinstance(row['transitions'], list):
            raise ValueError('invalid region transition evidence')
        for transition in row['transitions']:
            integer(transition['from'], 'region from', 0, 2)
            integer(transition['to'], 'region to', 0, 2)
            if (not isinstance(transition['region'], list) or len(transition['region']) != 2 or
                any(type(v) is not int for v in transition['region']) or
                transition['from'] == transition['to']):
                raise ValueError('invalid region transition')
            transitions.append(transition)
        if row['underground']:
            underground += 1
            if kind == 'cave' and not row['camera_in_air']:
                raise ValueError('cave camera must traverse live voxel air')
            if kind == 'cave' and (row['far_requested'] != 'ladder' or row['far_effective'] != 'ladder' or
                                   row['cave_effective'] is None or row['cave_requested'] != row['cave_effective']):
                raise ValueError('cave requires an active matching far cave policy')
    far = settings['far_mode']
    for key in ('cave_requested', 'cave_effective'):
        if far[key] is not None:
            integer(far[key], key, 0, 1)
    for key, observation in (('requested', 'far_requested'), ('effective', 'far_effective'),
                             ('cave_requested', 'cave_requested'), ('cave_effective', 'cave_effective')):
        if far[key] != rows[-1][observation]:
            raise ValueError('far mode settings differ from observed mode')
    if kind == 'cave' and underground == 0:
        raise ValueError('cave workload never entered underground')
    if kind == 'region_crossing':
        if (not regions['enabled'] or regions['work_limit'] != 0 or regions['hold_ticks'] != 30 or
            regions['reduced_shift'] != 1 or len({tuple(row['region']) for row in rows}) < 2 or
            not any(t['to'] == 0 for t in transitions) or not any(t['from'] == 0 for t in transitions)):
            raise ValueError('region crossing requires activation and deactivation evidence')
    if kind == 'edit':
        last_edit = actions[-1]['tick']
        held = [camera_sample(manifest, tick) for tick in range(last_edit, count + 1)]
        poses = [row['position'] + [row['yaw'], row['pitch']] for row in held]
        if len(poses) < 2 or any(pose != poses[0] for pose in poses):
            raise ValueError('edit workload must hold the camera after edits')
    if kind == 'save_load':
        reload = observed['reload']
        request = warmup + actions[-1]['tick'] - 1
        if not isinstance(reload, dict):
            # Report the actual gates from the retained capture. Counts describe
            # frames with pending work, not sums of queue sizes. The validated
            # observation limit bounds this diagnostic pass as well.
            pending = dict.fromkeys(('stream_queue_depth', 'stream_generation_batches',
                                     'stream_meshing_batches', 'stream_pending_lod_chunks',
                                     'stream_promotion_pending', 'terrain_upload_candidates',
                                     'water_upload_candidates', 'foliage_pending',
                                     'region_scheduler_fault', 'world_not_ready',
                                     'outside_gameplay'), 0)
            frames = 0
            for row in rows:
                if row['frame'] < request:
                    continue
                frames += 1
                for key in pending:
                    if key == 'world_not_ready':
                        active = not row['world_ready']
                    elif key == 'outside_gameplay':
                        active = row['state'] != 'gameplay'
                    else:
                        active = bool(row[key])
                    pending[key] += int(active)
            gates = ', '.join(f'{key}={value}' for key, value in pending.items())
            raise ValueError('save/load workload did not settle after reload '
                             f'({frames} observed frames; {gates})')
        if type(reload['request_frame']) is not int or reload['request_frame'] != request:
            raise ValueError('reload request does not match script')
        settled = integer(reload['first_settled_frame'], 'first settled frame', request, warmup + count - 1)
        first = next((r['frame'] for r in rows if r['frame'] >= request and r['settled']), None)
        if first != settled:
            raise ValueError('reload must report the first settled frame')
        elapsed = number(reload['elapsed_ms'], 'reload elapsed')
        coverage = sum(r['frame_wall_ms'] for r in data['frame_samples'][request - warmup:settled - warmup + 1])
        if elapsed > coverage + 1e-6:
            raise ValueError('reload duration exceeds measured coverage')
        # Request and settled timestamps lie inside their respective frame
        # intervals. Every intervening interval is included in full.
        complete = sum(r['frame_wall_ms'] for r in
                       data['frame_samples'][request - warmup + 1:settled - warmup])
        if elapsed + 1e-6 < complete:
            raise ValueError('reload duration omits complete frame intervals')
        if not any(camera_sample(manifest, tick)['position'] != camera_sample(manifest, tick - 1)['position']
                   for tick in range(settled - warmup + 2, count + 1)):
            raise ValueError('save/load workload did not resume its camera path')
    elif observed['reload'] is not None:
        raise ValueError('unexpected reload observation')


def validate_terrain_benchmark(manifest, benchmark):
    def require(condition, reason):
        if not condition:
            raise ValueError(reason)

    # V3/v4 preserve the capture-frame attachment contract while adding a separate
    # measurement contract. This join does not validate its timing distributions.
    require(benchmark['schema'] in ('luminumbra.render_benchmark.v2', *SCHEMAS), 'Unknown benchmark schema')
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
