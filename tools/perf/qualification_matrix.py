#!/usr/bin/env python3
"""Qualification matrix: strict loader, cell expansion, and client/checker argv builders.

Pure library: no process launch and no file writes at import or call time. The
committed matrix is qualification/matrix.json; plan_qualification.py is the dry-run
CLI built on top of this module.
"""
import json
import math
from pathlib import Path
import re

SCHEMA = 'luminumbra.qualification_matrix.v1'
PROFILE_IDS = ('quality', 'performance')
TOP_KEYS = frozenset(('schema', 'qualified_renderer', 'floor_ms', 'target_ms', 'profiles',
                      'displays', 'world', 'time_of_day', 'warmup_frames',
                      'fixed_view_measured_frames', 'workloads'))
PROFILE_KEYS = frozenset(('id', 'resolution_scale'))
PROFILE_SCALES = {'quality': 1.0, 'performance': 0.67}  # Hard-coded in main_client.cpp
DISPLAY_KEYS = frozenset(('id', 'width', 'height', 'refresh_hz'))
WORLD_KEYS = frozenset(('kind', 'note'))
FLOOR_MS = 16.67
TARGET_MS = 8.33
FIXED_KEYS = frozenset(('id', 'kind', 'pose', 'pose_provenance'))
TRAVERSAL_KEYS = frozenset(('id', 'kind', 'script', 'blocked_by'))
POSE_KEYS = ('x', 'y', 'z', 'yaw', 'pitch')
WORKLOAD_ID = re.compile(r'[a-z0-9-]+')
VALIDATOR = 'tools/perf/validate_render_capture.py'


class MatrixError(ValueError):
    """Raised for any malformed matrix, unknown cell, or cell that cannot run."""


def _reject_constant(name):
    raise MatrixError(f'matrix: non-standard JSON constant {name}')


def _no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise MatrixError(f'matrix: duplicate JSON key {key!r}')
        result[key] = value
    return result


def load_matrix(path):
    try:
        text = Path(path).read_text(encoding='utf-8')
    except (OSError, UnicodeError) as error:
        raise MatrixError(f'matrix: cannot read {path}: {error}')
    try:
        matrix = json.loads(text, object_pairs_hook=_no_duplicate_keys,
                            parse_constant=_reject_constant)
    except MatrixError:
        raise
    except ValueError as error:
        raise MatrixError(f'matrix: invalid JSON: {error}')
    validate_matrix(matrix)
    return matrix


def validate_matrix(matrix):
    if not isinstance(matrix, dict):
        raise MatrixError('matrix: top level must be a JSON object')
    if matrix.get('schema') != SCHEMA:
        raise MatrixError(f'matrix: schema must be {SCHEMA!r}, got {matrix.get("schema")!r}')
    keys = set(matrix)
    if keys != TOP_KEYS:
        raise MatrixError(f'matrix: missing top-level keys {sorted(TOP_KEYS - keys)}, '
                          f'unexpected keys {sorted(keys - TOP_KEYS)}')
    _validate_renderer(matrix['qualified_renderer'])
    _validate_timing(matrix['floor_ms'], matrix['target_ms'])
    _validate_world(matrix['world'])
    _validate_time_of_day(matrix['time_of_day'])
    _validate_profiles(matrix['profiles'])
    _validate_displays(matrix['displays'])
    _validate_frames(matrix['warmup_frames'], 'warmup_frames')
    _validate_frames(matrix['fixed_view_measured_frames'], 'fixed_view_measured_frames')
    _validate_workloads(matrix['workloads'])


def _require_exact(obj, expected, label):
    if not isinstance(obj, dict):
        raise MatrixError(f'matrix: {label} must be an object')
    keys = set(obj)
    if keys != expected:
        raise MatrixError(f'matrix: {label} missing keys {sorted(expected - keys)}, '
                          f'unexpected keys {sorted(keys - expected)}')


def _validate_renderer(value):
    if not isinstance(value, str) or not value or value != value.strip():
        raise MatrixError('matrix: qualified_renderer must be a non-empty string without '
                          'leading or trailing spaces')


def _validate_timing(floor_ms, target_ms):
    for label, value in (('floor_ms', floor_ms), ('target_ms', target_ms)):
        if type(value) is not float:
            raise MatrixError(f'matrix: {label} must be a float')
    if floor_ms != FLOOR_MS or target_ms != TARGET_MS:
        raise MatrixError(f'matrix: floor_ms must be {FLOOR_MS} and target_ms {TARGET_MS}, '
                          f'got {floor_ms!r} and {target_ms!r}')
    if not floor_ms >= target_ms > 0:
        raise MatrixError('matrix: require floor_ms >= target_ms > 0')


def _validate_world(world):
    _require_exact(world, WORLD_KEYS, 'world')
    if world['kind'] != 'fresh':
        raise MatrixError(f"matrix: world kind must be 'fresh', got {world['kind']!r}")
    if not isinstance(world['note'], str):
        raise MatrixError('matrix: world note must be a string')


def _validate_time_of_day(value):
    # The client fixes benchmark time of day at 0.04 (main_client.cpp set_time_of_day).
    if type(value) is not float or value != 0.04:
        raise MatrixError(f'matrix: time_of_day must be 0.04 (client-fixed), got {value!r}')


def _validate_profiles(profiles):
    if not isinstance(profiles, list):
        raise MatrixError('matrix: profiles must be a list')
    for profile in profiles:
        _require_exact(profile, PROFILE_KEYS, 'profile')
    ids = [profile['id'] for profile in profiles]
    if sorted(ids, key=str) != sorted(PROFILE_IDS):
        raise MatrixError(f'matrix: profiles must have exactly the ids {list(PROFILE_IDS)}, '
                          f'got {ids!r}')
    for profile in profiles:
        scale = profile['resolution_scale']
        expected = PROFILE_SCALES[profile['id']]
        if type(scale) is not float or scale != expected:
            raise MatrixError(f"matrix: profile {profile['id']!r} resolution_scale must be "
                              f'{expected}')


def _validate_bounded_int(value, minimum, maximum, label):
    if type(value) is not int or not minimum <= value <= maximum:
        raise MatrixError(f'matrix: {label} must be an integer in {minimum}..{maximum}, '
                          f'got {value!r}')


def _validate_displays(displays):
    if not isinstance(displays, list) or not displays:
        raise MatrixError('matrix: displays must be a non-empty list')
    seen = set()
    for display in displays:
        _require_exact(display, DISPLAY_KEYS, 'display')
        display_id = display['id']
        _validate_bounded_int(display['width'], 1, 16384, f'display {display_id!r} width')
        _validate_bounded_int(display['height'], 1, 16384, f'display {display_id!r} height')
        refresh = display['refresh_hz']
        if refresh is not None:
            _validate_bounded_int(refresh, 1, 1000, f'display {display_id!r} refresh_hz')
        expected = f"{display['width']}x{display['height']}"
        if display_id != expected:
            raise MatrixError(f'matrix: display id {display_id!r} must be {expected!r}')
        if display_id in seen:
            raise MatrixError(f'matrix: duplicate display id {display_id!r}')
        seen.add(display_id)


def _validate_frames(value, label):
    if type(value) is not int or not 1 <= value <= 18000:
        raise MatrixError(f'matrix: {label} must be an integer in 1..18000, got {value!r}')


def _validate_workloads(workloads):
    if not isinstance(workloads, list) or not workloads:
        raise MatrixError('matrix: workloads must be a non-empty list')
    seen = set()
    for workload in workloads:
        if not isinstance(workload, dict):
            raise MatrixError('matrix: workload entries must be objects')
        workload_id = workload.get('id')
        if not isinstance(workload_id, str) or WORKLOAD_ID.fullmatch(workload_id) is None:
            raise MatrixError(f'matrix: workload id {workload_id!r} must match [a-z0-9-]+')
        if workload_id in seen:
            raise MatrixError(f'matrix: duplicate workload id {workload_id!r}')
        seen.add(workload_id)
        kind = workload.get('kind')
        if kind == 'fixed_view':
            _validate_fixed_view(workload)
        elif kind == 'traversal':
            _validate_traversal(workload)
        else:
            raise MatrixError(f'matrix: workload {workload_id!r} kind must be fixed_view or '
                              f'traversal, got {kind!r}')


def _require_keys(workload, expected):
    keys = set(workload)
    if keys != expected:
        raise MatrixError(f'matrix: workload {workload.get("id")!r} missing keys '
                          f'{sorted(expected - keys)}, unexpected keys {sorted(keys - expected)}')


def _validate_fixed_view(workload):
    _require_keys(workload, FIXED_KEYS)
    pose = workload['pose']
    if pose is not None:
        _validate_pose(pose, workload['id'])
    provenance = workload['pose_provenance']
    if not isinstance(provenance, str) or not provenance:
        raise MatrixError(f'matrix: workload {workload["id"]!r} pose_provenance must be a '
                          'non-empty string')


def _validate_pose(pose, workload_id):
    if not isinstance(pose, dict) or set(pose) != set(POSE_KEYS):
        raise MatrixError(f'matrix: workload {workload_id!r} pose must be null or an object '
                          f'with exactly the keys {list(POSE_KEYS)}')
    for key in POSE_KEYS:
        value = pose[key]
        if type(value) is not float or not math.isfinite(value):
            raise MatrixError(f'matrix: workload {workload_id!r} pose {key} must be a finite '
                              'float')


def _validate_traversal(workload):
    _require_keys(workload, TRAVERSAL_KEYS)
    script = workload['script']
    blocked_by = workload['blocked_by']
    if (script is None) == (blocked_by is None):
        raise MatrixError(f'matrix: traversal {workload["id"]!r} needs exactly one of '
                          'script or blocked_by')
    for label, value in (('script', script), ('blocked_by', blocked_by)):
        if value is not None and not isinstance(value, str):
            raise MatrixError(f'matrix: traversal {workload["id"]!r} {label} must be a string')


def cells(matrix):
    """Expand to workload x profile x display cells in matrix order."""
    result = []
    for workload in matrix['workloads']:
        fixed = workload['kind'] == 'fixed_view'
        pose = workload.get('pose') if fixed else None
        for profile in matrix['profiles']:
            for display in matrix['displays']:
                result.append(dict(
                    id=f"{workload['id']}.{profile['id']}.{display['id']}",
                    workload=workload['id'],
                    kind=workload['kind'],
                    profile=profile['id'],
                    display=display['id'],
                    width=display['width'],
                    height=display['height'],
                    refresh_hz=display['refresh_hz'],
                    pose=None if pose is None else dict(pose),
                    pose_provenance=workload['pose_provenance'] if fixed else None,
                    script=None if fixed else workload['script'],
                    blocked_by=None if fixed else workload['blocked_by'],
                    warmup_frames=matrix['warmup_frames'],
                    measured_frames=matrix['fixed_view_measured_frames'] if fixed else None,
                    time_of_day=matrix['time_of_day']))
    return result


def readiness(cell, root):
    if cell['kind'] == 'fixed_view':
        if cell['pose'] is None:
            return ('unevaluated', 'pose-unrecorded')
        return ('ready', None)
    if cell['blocked_by'] is not None:
        return ('unevaluated', f"blocked: {cell['blocked_by']}")
    if cell['script'] is None or not (Path(root) / cell['script']).is_file():
        return ('unevaluated', 'traversal-script-missing')
    return ('ready', None)


def _require_ready(cell, root):
    state, reason = readiness(cell, root)
    if state != 'ready':
        raise MatrixError(f"cell {cell['id']!r} is not ready: {reason}")


def build_command(cell, *, client, artifact, root):
    """Return the exact render-benchmark client argv for a ready cell."""
    _require_ready(cell, root)
    command = [str(client), '--no-audio', '--no-menu-backdrop', '--render-benchmark',
               str(artifact), '--render-benchmark-schema', 'v3',
               '--capture-size', f"{cell['width']}x{cell['height']}",
               '--perf-profile', cell['profile'],
               '--render-benchmark-warmup', str(cell['warmup_frames'])]
    if cell['kind'] == 'fixed_view':
        pose = cell['pose']
        command += ['--render-benchmark-frames', str(cell['measured_frames']),
                    '--cam-pos', f"{pose['x']!r},{pose['y']!r},{pose['z']!r}",
                    '--cam-yaw', repr(pose['yaw']),
                    '--cam-pitch', repr(pose['pitch'])]
    else:
        command += ['--traversal', str(Path(root) / cell['script'])]
    return command


def checker_command(cell, *, artifact, qualified_renderer, root, python):
    """Return the validate_render_capture.py argv that checks a ready cell's artifact."""
    _require_ready(cell, root)
    command = [str(python), str(Path(root) / VALIDATOR), str(artifact),
               '--qualified-renderer', qualified_renderer]
    if cell['kind'] == 'fixed_view':
        pose = cell['pose']
        command += ['--position', repr(pose['x']), repr(pose['y']), repr(pose['z']),
                    '--yaw', repr(pose['yaw']), '--pitch', repr(pose['pitch']),
                    '--tod', repr(cell['time_of_day'])]
    else:
        command += ['--traversal', str(Path(root) / cell['script'])]
    return command
