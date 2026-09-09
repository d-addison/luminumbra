#!/usr/bin/env python3
"""Read one benchmark artifact; never launches a process or changes a file.

Checks observed linked-program state against both the requested pose/TOD and
reported camera. This is a report-frame check, not evidence that every program
executed a draw or every measured frame had the same state.
"""
import argparse
import json
import math
from pathlib import Path
import render_contract


def finite_vector(value, count, label):
    if not isinstance(value, list) or len(value) != count:
        raise ValueError(f'{label}: unavailable or wrong length')
    if any(not isinstance(x, (float, int)) or isinstance(x, bool) or not math.isfinite(x) for x in value):
        raise ValueError(f'{label}: non-finite/non-numeric value')
    return value


def close(actual, expected, tolerance, label):
    expected = finite_vector(expected, len(expected), label + ' expected')
    actual = finite_vector(actual, len(expected), label)
    error = max(abs(a - e) for a, e in zip(actual, expected))
    if error > tolerance:
        raise ValueError(f'{label}: max error {error:.9g} > {tolerance}; actual={actual}, expected={expected}')
    return error


def unit(v):
    length = math.sqrt(sum(x * x for x in v))
    if length == 0:
        raise ValueError('zero direction')
    return [x / length for x in v]


def cross(a, b):
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def camera_basis(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch)
    forward = unit([math.cos(y)*math.cos(p), math.sin(p), math.sin(y)*math.cos(p)])
    right = unit(cross(forward, [0, 1, 0]))
    up = unit(cross(right, forward))
    return right, up, forward


def validate(data, args):
    schema = data.get('schema')
    if schema not in ('luminumbra.render_benchmark.v2', render_contract.SCHEMA):
        raise ValueError('unsupported render capture schema')
    if schema == 'luminumbra.render_benchmark.v2':
        missing = ['--' + name for name in ('position', 'yaw', 'pitch', 'tod')
                   if getattr(args, name, None) is None]
        if missing:
            raise ValueError('v2 capture requires missing arguments: ' + ', '.join(missing))
    measurement = None
    if schema == render_contract.SCHEMA:
        manifest_path = getattr(args, 'workload_manifest', None)
        workload = json.loads(Path(manifest_path).read_text(encoding='utf-8')) if manifest_path else None
        measurement = render_contract.validate(data, getattr(args, 'qualified_renderer', None),
            getattr(args, 'qualified_vendor', None), getattr(args, 'traversal', None), workload)
        if args.position is None:
            if any(value is not None for value in (args.yaw, args.pitch, args.tod, args.fov)) or any(
                    getattr(args, name, False) for name in ('require_controller', 'require_distinct_controller',
                    'require_geometry', 'require_settled', 'require_camera_chunk')):
                raise ValueError('report-frame checks require --position, --yaw, --pitch and --tod')
            return measurement
    elif getattr(args, 'qualified_renderer', None) or getattr(args, 'qualified_vendor', None):
        raise ValueError('v2 has no adapter identity; cannot qualify an adapter')
    finite_vector(args.position, 3, 'requested position')
    finite_vector([args.yaw, args.pitch, args.tod], 3, 'requested angles/time')
    if not 0 <= args.tod < 1:
        raise ValueError('requested TOD must be in [0, 1)')
    if args.fov is not None:
        finite_vector([args.fov], 1, 'requested FOV')
        if not 0 < args.fov < 180:
            raise ValueError('requested FOV must be in (0, 180)')
    camera = data['camera']
    close(camera['position'], args.position, 1e-5, 'reported/requested position')
    close([camera['yaw'], camera['pitch']], [args.yaw, args.pitch], 1e-5, 'reported/requested angles')
    if args.fov is not None:
        close([camera['fov']], [args.fov], 1e-5, 'reported/requested FOV')
    context = data['capture_context']
    close([context['time_of_day_at_report']], [args.tod], 1e-6, 'reported/requested TOD')
    close(context['last_world_streaming_position'], args.position, 1e-5, 'last actual world update position')
    if args.require_controller and context['controller_present'] is not True:
        raise ValueError('ordinary-play PlayerController was not present')
    if args.require_controller:
        finite_vector(context['controller_position'], 3, 'controller position')
    if args.require_distinct_controller:
        player = finite_vector(context['controller_position'], 3, 'controller position')
        if max(abs(a-b) for a,b in zip(player,args.position)) < 1.0:
            raise ValueError('controller was not at least 1m away from the capture pose')
    programs = data['render_uniforms']
    if programs['matrix_layout'] != 'column_major':
        raise ValueError('unexpected matrix layout')
    if programs['observation'] != 'report_time_shader_state':
        raise ValueError('unexpected observation semantics')
    position = camera['position']
    right, up, forward = camera_basis(camera['yaw'], camera['pitch'])
    expected_view = [right[0], up[0], -forward[0], 0, right[1], up[1], -forward[1], 0,
                     right[2], up[2], -forward[2], 0, -dot(right,position), -dot(up,position), dot(forward,position), 1]
    expected_inverse = right + [0] + up + [0] + [-x for x in forward] + [0] + position + [1]
    errors = {}
    for name in ['geometry', 'instanced_static_mesh']:
        program = programs.get(name, {})
        if program.get('available') is not True:
            raise ValueError(f'{name}: program unavailable')
        errors[name + '_view'] = close(program.get('view'), expected_view, 0.002, name + ' view/report')
        view = program['view']
        observed_position = [-sum(view[col*4+row]*view[12+row] for row in range(3)) for col in range(3)]
        errors[name + '_position'] = close(observed_position, position, 0.002, name + ' observed position/report')
        projection = finite_vector(program.get('projection'), 16, name + ' projection')
        if projection[0] <= 0 or projection[5] <= 0:
            raise ValueError(f'{name}: invalid perspective scale')
        observed_fov = math.degrees(2*math.atan(1/projection[5]))
        close([observed_fov], [camera['fov']], 0.001, name + ' observed FOV/report')
        close([projection[5]/projection[0]], [data['width']/data['height']], 1e-5, name + ' observed aspect/report')
    lighting = programs.get('lighting', {})
    if lighting.get('available') is not True:
        raise ValueError('lighting program unavailable')
    errors['lighting_position'] = close(lighting.get('u_viewPos'), position, 1e-5, 'lighting position/report')
    errors['lighting_inverse_view'] = close(lighting.get('u_inverseView'), expected_inverse, 0.002, 'lighting inverse view/report')
    sun = finite_vector(lighting.get('u_sun.direction'), 3, 'lighting sun direction')
    close([math.sqrt(dot(sun,sun))], [1.0], 1e-5, 'sun direction length')
    # Sun travel direction is normalized (sin(phase), -cos(phase), seasonal_z).
    # atan2 eliminates the seasonal tilt and normalization independently.
    observed_tod = (math.atan2(sun[0], -sun[1]) / (2*math.pi)) % 1.0
    tod_error = abs((observed_tod - args.tod + 0.5) % 1.0 - 0.5)
    if tod_error > 1e-5:
        raise ValueError(f'actual lighting TOD {observed_tod:.9g} != requested {args.tod}; phase error {tod_error}')
    if args.require_geometry and data['streaming']['terrain_draws'] <= 0:
        raise ValueError('no terrain draw reported for the observation frame')
    if args.require_settled:
        for key in ['generation_pending', 'meshing_pending', 'terrain_uploads_pending']:
            if data['streaming'][key] != 0:
                raise ValueError(f'not settled: {key}={data["streaming"][key]}')
    if args.require_camera_chunk and not data['camera_chunk']['resident']:
        raise ValueError('capture camera chunk is not resident')
    result = {'verdict': 'PASS', 'scope': 'report-frame linked uniforms and actual last streaming argument; not per-frame timing or every-program draw proof',
            'observed_sun_time_of_day': observed_tod, 'tod_phase_error': tod_error, 'max_absolute_errors': errors}
    if measurement is not None:
        result['measurement'] = measurement
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact', type=Path)
    parser.add_argument('--position', type=float, nargs=3)
    parser.add_argument('--yaw', type=float)
    parser.add_argument('--pitch', type=float)
    parser.add_argument('--tod', type=float)
    parser.add_argument('--fov', type=float)
    parser.add_argument('--require-controller', action='store_true')
    parser.add_argument('--require-distinct-controller', action='store_true')
    parser.add_argument('--require-geometry', action='store_true')
    parser.add_argument('--require-settled', action='store_true')
    parser.add_argument('--require-camera-chunk', action='store_true')
    parser.add_argument('--qualified-renderer', help='Exact GL_RENDERER required for v3 qualification')
    parser.add_argument('--qualified-vendor', help='Optional exact GL_VENDOR')
    parser.add_argument('--traversal', type=Path, help='Expected committed traversal script')
    parser.add_argument('--workload-manifest', type=Path, help='Expected workload fields as a JSON object')
    args = parser.parse_args()
    try:
        data = json.loads(args.artifact.read_text(encoding='utf-8-sig'))
        result = validate(data, args)
    except (ValueError, KeyError, TypeError, ZeroDivisionError, OSError) as error:
        print(json.dumps({'verdict': 'FAIL', 'reason': str(error)}, indent=2))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
