#!/usr/bin/env python3
"""Read one benchmark artifact; never launches a process or changes a file.

Checks observed linked-program state against both the requested pose/TOD and
reported camera. This is a report-frame check, not evidence that every program
executed a draw or every measured frame had the same state.
With --enforce-floor a v3 capture is additionally held to the 16.67 ms frame_wall p99 floor (exit 4).
"""
import argparse
import json
import math
from pathlib import Path
import struct
import render_budget
import render_contract
import perf


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


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def unit(v):
    # GLM's vec3 normalize multiplies by inversesqrt(dot(v, v)).
    squared = dot32(v, v)
    if squared == 0 or not math.isfinite(squared):
        raise ValueError('zero direction')
    reciprocal = f32(1 / f32(math.sqrt(squared)))
    return [f32(x * reciprocal) for x in v]


def cross(a, b):
    return [f32(f32(a[1]*b[2])-f32(a[2]*b[1])),
            f32(f32(a[2]*b[0])-f32(a[0]*b[2])),
            f32(f32(a[0]*b[1])-f32(a[1]*b[0]))]


def dot32(a, b):
    products = [f32(x*y) for x, y in zip(a, b)]
    return f32(f32(products[0] + products[1]) + products[2])


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def camera_basis(yaw, pitch):
    radians = f32(math.pi / 180)
    y, p = f32(f32(yaw) * radians), f32(f32(pitch) * radians)
    # Camera::updateCameraVectors stores the trigonometric products in vec3.
    forward = unit([f32(math.cos(y)*math.cos(p)), f32(math.sin(p)),
                    f32(math.sin(y)*math.cos(p))])
    right = unit(cross(forward, [0, 1, 0]))
    up = unit(cross(right, forward))
    return right, up, forward


def camera_view(position, yaw, pitch):
    """Camera::GetViewMatrix: float32 lookAt(Position, Position + Front, Up)."""
    position = [f32(value) for value in position]
    _, up, front = camera_basis(yaw, pitch)
    center = [f32(eye + direction) for eye, direction in zip(position, front)]
    forward = unit([f32(target - eye) for target, eye in zip(center, position)])
    right = unit(cross(forward, up))
    up = cross(right, forward)
    return [right[0], up[0], -forward[0], 0, right[1], up[1], -forward[1], 0,
            right[2], up[2], -forward[2], 0,
            -dot32(right, position), -dot32(up, position), dot32(forward, position), 1]


def inverse_view(view):
    """Float32 GLM mat4 inverse, including its cofactor/determinant ordering."""
    return inverse_view_with_tolerances(view)[0]


def inverse_view_with_tolerances(view):
    """Inverse and componentwise bounds for different float32 evaluation orders."""
    view = [f32(value) for value in view]
    m = [view[index:index + 4] for index in range(0, 16, 4)]
    def minor(a, b, row, other):
        return f32(f32(m[a][row] * m[b][other]) - f32(m[b][row] * m[a][other]))

    factors = []
    factor_magnitudes = []
    for row, other in ((2, 3), (1, 3), (1, 2), (0, 3), (0, 2), (0, 1)):
        value = minor(2, 3, row, other)
        factors.append([value, value, minor(1, 3, row, other), minor(1, 2, row, other)])
        magnitudes = [abs(m[a][row] * m[b][other]) + abs(m[b][row] * m[a][other])
                      for a, b in ((2, 3), (1, 3), (1, 2))]
        factor_magnitudes.append([magnitudes[0], *magnitudes])
    vectors = [[m[1][row], m[0][row], m[0][row], m[0][row]] for row in range(4)]
    inverse = []
    cofactor_magnitudes = []
    for column, terms in enumerate((((1, 0), (2, 1), (3, 2)),
                                    ((0, 0), (2, 3), (3, 4)),
                                    ((0, 1), (1, 3), (3, 5)),
                                    ((0, 2), (1, 4), (2, 5)))):
        for row in range(4):
            products = [f32(vectors[v][row] * factors[f][row]) for v, f in terms]
            value = f32(f32(products[0] - products[1]) + products[2])
            inverse.append(value if (row + column) % 2 == 0 else -value)
            cofactor_magnitudes.append(sum(abs(vectors[v][row]) * factor_magnitudes[f][row]
                                           for v, f in terms))
    products = [f32(m[0][row] * inverse[row * 4]) for row in range(4)]
    determinant = f32(f32(products[0] + products[1]) + f32(products[2] + products[3]))
    if determinant == 0 or not math.isfinite(determinant):
        raise ValueError('singular camera view')
    reciprocal = f32(1 / determinant)
    result = [f32(value * reciprocal) for value in inverse]
    determinant_magnitude = sum(abs(m[0][row]) * cofactor_magnitudes[row * 4] for row in range(4))
    # Each cofactor/determinant/reciprocal path has at most 12 rounded operations.
    # Bound two evaluations by gamma(32), with float32 unit roundoff 2**-24.
    # Absolute term magnitudes retain cancellation error without allowing an
    # unrelated rotation mismatch or another program's matrix to buy slack.
    gamma = (32 * 2**-24) / (1 - 32 * 2**-24)
    tolerances = [0.002 + gamma * (magnitude + abs(value) * determinant_magnitude) / abs(determinant)
                  for value, magnitude in zip(result, cofactor_magnitudes)]
    return result, tolerances


def view_position(view):
    # This diagnostic is evaluated in Python precision for both matrices. A
    # float32 lookAt is not perfectly orthogonal, so it need not recover Position.
    return [-sum(view[col*4+row]*view[12+row] for row in range(3)) for col in range(3)]


def translation_tolerance(position, rotation_error, operations):
    # For t = -R*p, |delta t| <= max(|delta R|) * sum(|p|).
    # Spend only the observed rotation difference, capped at eight float32
    # epsilons (trig products, normalization and cross products). A bad rotation
    # cannot buy arbitrary translation slack, and translation-only corruption
    # keeps the absolute floor. The factors below allow float dot/cofactor
    # rounding (4) and propagation through R^T*t (8) or the GLM inverse (16).
    return 0.002 + operations * min(rotation_error, 8 * 2**-23) * sum(abs(x) for x in position)


def close_camera_matrix(actual, expected, position, label, source_rotation_error=0, operations=4):
    actual = finite_vector(actual, 16, label)
    # Position magnitude must never loosen the rotation or homogeneous entries.
    rotation = close(actual[:12], expected[:12], 0.002, label)
    homogeneous = close(actual[15:], expected[15:], 0.002, label)
    rotation_error = max(abs(actual[col*4+row] - expected[col*4+row])
                         for col in range(3) for row in range(3))
    tolerance = translation_tolerance(position, max(rotation_error, source_rotation_error), operations)
    translation = close(actual[12:15], expected[12:15], tolerance, label)
    return max(rotation, homogeneous, translation), rotation_error


def validate_cell(data, args):
    """Bind a v3 capture to any expected cell parameters supplied by the caller."""
    for name, path in (('profile', ('profile', 'name')),
                       ('width', ('width',)), ('height', ('height',)),
                       ('warmup_frames', ('warmup_frames',)),
                       ('frames', ('frames',)), ('workload_kind', ('workload', 'kind'))):
        expected = getattr(args, name, None)
        if expected is None:
            continue
        option = '--' + name.replace('_', '-')
        if data.get('schema') != render_contract.SCHEMA:
            raise ValueError(f'{option} requires a v3 capture')
        actual = data
        for key in path:
            actual = actual[key]
        if type(actual) is not type(expected) or actual != expected:
            raise ValueError(f'{option} mismatch: actual={actual!r}, expected={expected!r}')


def validate(data, args):
    schema = data.get('schema')
    if schema not in ('luminumbra.render_benchmark.v2', render_contract.SCHEMA):
        raise ValueError('unsupported render capture schema')
    validate_cell(data, args)
    if schema == 'luminumbra.render_benchmark.v2':
        missing = ['--' + name for name in ('position', 'yaw', 'pitch', 'tod')
                   if getattr(args, name, None) is None]
        if missing:
            raise ValueError('v2 capture requires missing arguments: ' + ', '.join(missing))
    if schema == 'luminumbra.render_benchmark.v2':
        perf.validate_far_world_reporting(data, getattr(args, 'ladder_baseline', False))
    measurement = None
    if schema == render_contract.SCHEMA:
        manifest_path = getattr(args, 'workload_manifest', None)
        workload = json.loads(Path(manifest_path).read_text(encoding='utf-8')) if manifest_path else None
        measurement = render_contract.validate(data, getattr(args, 'qualified_renderer', None),
            getattr(args, 'qualified_vendor', None), getattr(args, 'traversal', None), workload,
            ladder_baseline=getattr(args, 'ladder_baseline', False))
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
    # The client stores requested camera coordinates and angles as float32.
    # Keep the absolute tolerance while comparing the values the client can store.
    try:
        pose = struct.unpack('<5f', struct.pack('<5f', *args.position, args.yaw, args.pitch))
        position, angles = list(pose[:3]), list(pose[3:])
    except OverflowError as error:
        raise ValueError('requested pose is outside float32 range') from error
    if not 0 <= args.tod < 1:
        raise ValueError('requested TOD must be in [0, 1)')
    if args.fov is not None:
        finite_vector([args.fov], 1, 'requested FOV')
        if not 0 < args.fov < 180:
            raise ValueError('requested FOV must be in (0, 180)')
    camera = data['camera']
    close(camera['position'], position, 1e-5, 'reported/requested position')
    close([camera['yaw'], camera['pitch']], angles, 1e-5, 'reported/requested angles')
    if args.fov is not None:
        close([camera['fov']], [args.fov], 1e-5, 'reported/requested FOV')
    context = data['capture_context']
    close([context['time_of_day_at_report']], [args.tod], 1e-6, 'reported/requested TOD')
    close(context['last_world_streaming_position'], position, 1e-5, 'last actual world update position')
    if args.require_controller and context['controller_present'] is not True:
        raise ValueError('ordinary-play PlayerController was not present')
    if args.require_controller:
        finite_vector(context['controller_position'], 3, 'controller position')
    if args.require_distinct_controller:
        player = finite_vector(context['controller_position'], 3, 'controller position')
        if max(abs(a-b) for a,b in zip(player,position)) < 1.0:
            raise ValueError('controller was not at least 1m away from the capture pose')
    programs = data['render_uniforms']
    if programs['matrix_layout'] != 'column_major':
        raise ValueError('unexpected matrix layout')
    if programs['observation'] != 'report_time_shader_state':
        raise ValueError('unexpected observation semantics')
    position = camera['position']
    expected_view = camera_view(position, camera['yaw'], camera['pitch'])
    expected_inverse = inverse_view(expected_view)
    expected_position = view_position(expected_view)
    errors = {}
    source_rotation_error = 0
    for name in ['geometry', 'instanced_static_mesh']:
        program = programs.get(name, {})
        if program.get('available') is not True:
            raise ValueError(f'{name}: program unavailable')
        errors[name + '_view'], rotation_error = close_camera_matrix(
            program.get('view'), expected_view, position, name + ' view/report')
        source_rotation_error = max(source_rotation_error, rotation_error)
        view = program['view']
        # lookAt writes this row exactly. Together with the rotation check it
        # keeps the inverse well conditioned, independent of world position.
        close([view[index] for index in (3, 7, 11, 15)], [0, 0, 0, 1], 0, name + ' affine view')
        for row in range(3):
            basis = [f32(view[col*4+row]) for col in range(3)]
            expected_translation = -dot32(basis, position)
            # Three products and two additions, for either evaluation order.
            # Each row gets only its own float32 rounding bound.
            magnitude = sum(abs(value * coordinate) for value, coordinate in zip(basis, position))
            tolerance = 0.002 + (10 * 2**-24) / (1 - 10 * 2**-24) * magnitude
            close([view[12+row]], [expected_translation], tolerance, name + ' view translation/position')
        observed_position = view_position(view)
        errors[name + '_position'] = close(observed_position, expected_position,
            translation_tolerance(position, rotation_error, 8), name + ' observed position/report')
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
    errors['lighting_inverse_view'], _ = close_camera_matrix(
        lighting.get('u_inverseView'), expected_inverse, position, 'lighting inverse view/report',
        source_rotation_error, operations=16)
    # The report-pose envelope above permits correlated source rounding only.
    # Bind lighting to each captured source using componentwise float32 bounds;
    # a source's observed rotation error cannot loosen these consistency checks.
    for name in ('geometry', 'instanced_static_mesh'):
        inverse, tolerances = inverse_view_with_tolerances(programs[name]['view'])
        for index, (value, tolerance) in enumerate(zip(inverse, tolerances)):
            close([lighting['u_inverseView'][index]], [value], tolerance,
                  f'lighting inverse view/{name}[{index}]')
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
    parser.add_argument('--ladder-baseline', action='store_true',
                        help='Require every measured frame to be settled, unsuppressed Ladder')
    parser.add_argument('--require-camera-chunk', action='store_true')
    parser.add_argument('--qualified-renderer', help='Exact GL_RENDERER required for v3 qualification')
    parser.add_argument('--qualified-vendor', help='Optional exact GL_VENDOR')
    parser.add_argument('--profile', choices=('legacy', 'quality', 'performance'),
                        help='Expected v3 performance profile')
    parser.add_argument('--width', type=int, help='Expected v3 capture width')
    parser.add_argument('--height', type=int, help='Expected v3 capture height')
    parser.add_argument('--warmup-frames', type=int, help='Expected v3 warmup frame count')
    parser.add_argument('--frames', type=int, help='Expected v3 measured frame count')
    parser.add_argument('--workload-kind', choices=('fixed_view', 'traversal'),
                        help='Expected v3 workload kind')
    parser.add_argument('--traversal', type=Path, help='Expected committed traversal script')
    parser.add_argument('--workload-manifest', type=Path, help='Expected workload fields as a JSON object')
    parser.add_argument('--enforce-floor', action='store_true',
                        help='v3 only: exit 4 when frame_wall_ms p99 exceeds 16.67 ms')
    parser.add_argument('--verdict-out', type=Path,
                        help='Write the floor verdict JSON here (requires --enforce-floor)')
    args = parser.parse_args()
    if args.verdict_out is not None and not args.enforce_floor:
        parser.error('--verdict-out requires --enforce-floor')
    try:
        data = json.loads(args.artifact.read_text(encoding='utf-8-sig'))
        result = validate(data, args)
        if args.enforce_floor:
            if data.get('schema') != render_contract.SCHEMA:
                print(json.dumps({'verdict': 'FAIL', 'reason': '--enforce-floor requires a v3 capture'}, indent=2))
                return 1
            floor = render_budget.evaluate(data)
            result['floor'] = floor
            if args.verdict_out is not None:
                args.verdict_out.write_text(json.dumps(floor, indent=2) + '\n', encoding='utf-8')
    except (ValueError, KeyError, TypeError, ZeroDivisionError, OSError, render_budget.BudgetInputError) as error:
        print(json.dumps({'verdict': 'FAIL', 'reason': str(error)}, indent=2))
        return 1
    print(json.dumps(result, indent=2))
    if args.enforce_floor and not render_budget.verdict_passes(result['floor']):
        return 4
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
