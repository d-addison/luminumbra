#!/usr/bin/env python3
"""File-only foliage v3 proof verifier. No renderer, image transformation or approvals."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct


class Refusal(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Refusal(message)


def number(value):
    require(type(value) in (int, float) and math.isfinite(value), "non-finite/nonnumeric evidence")
    return value


def integer(value):
    require(type(value) is int and value >= 0, "expected unsigned integer")
    return value


def fnv(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def local(root, name, limit):
    require(type(name) is str and re.fullmatch(r'[A-Za-z0-9_./-]+', name) is not None,
            "unsafe artifact name")
    path = root / name
    require(not Path(name).is_absolute() and '..' not in Path(name).parts, "escaped artifact")
    check = path
    while check != root:
        require(not check.is_symlink() and not (getattr(check.lstat(), "st_file_attributes", 0) & 0x400),
                "linked artifact")
        check = check.parent
    require(path.is_file() and 0 < path.stat().st_size <= limit, "missing/oversized artifact")
    return path.read_bytes()


def expected_view_projection(camera):
    yaw = math.radians(number(camera['yaw_degrees']))
    pitch = math.radians(number(camera['pitch_degrees']))
    eye = camera['position']
    front = (math.cos(yaw)*math.cos(pitch), math.sin(pitch), math.sin(yaw)*math.cos(pitch))
    right = (-math.sin(yaw), 0, math.cos(yaw))
    up = (-math.cos(yaw)*math.sin(pitch), math.cos(pitch), -math.sin(yaw)*math.sin(pitch))
    view = [[*right, -sum(a*b for a,b in zip(right,eye))],
            [*up, -sum(a*b for a,b in zip(up,eye))],
            [*(-x for x in front), sum(a*b for a,b in zip(front,eye))], [0,0,0,1]]
    # Actual Camera.h finite RH reversed-Z, GL_ZERO_TO_ONE; fixed profile planes.
    scale = 1/math.tan(math.radians(number(camera['vertical_fov_degrees']))/2)
    near, far = .1, 3200
    projection = [[scale/(3840/1600),0,0,0], [0,scale,0,0],
                  [0,0,near/(far-near),far*near/(far-near)], [0,0,-1,0]]
    return [sum(projection[row][k]*view[k][col] for k in range(4))
            for col in range(4) for row in range(4)]


def validate(root: Path):
    root = root.resolve(strict=True)
    raw = local(root, 'foliage-instancing-analysis.json', 2 * 1024 * 1024)
    report = json.loads(raw, parse_constant=lambda _: (_ for _ in ()).throw(Refusal("non-finite JSON")))
    require(report['schema'] == 'luminumbra.foliage_instancing.v3', 'v3 evidence required')
    require(report['profile'] == 'luminumbra.foliage_control.v2', 'wrong workload profile')
    require(not report.get('refusal'), 'producer refused')
    require(report['functional_control']['passed'] is True, 'functional controls failed')
    require(report['qualification']['visual_approved'] is False, 'visual approval is outside this gate')
    require(report['gl_debug']['errors'] == 0, 'GL errors')
    phases = [report['phases'][name] for name in ('calm', 'windy')]
    artifacts = {'foliage-instancing-analysis.json': raw}
    for index, phase in enumerate(phases):
        require(phase['sampled'] is True and phase['phase'] == ('calm', 'windy')[index], 'missing phase')
        require(phase['instance_source'] == 'gpu_readback' and phase['instance_matches_drawn_build'] is True,
                'GPU build/readback required')
        require(integer(phase['build_generation']) > 0 and phase['build_generation'] == phase['instance_generation'] ==
                phase['readback_generation'], 'stale instance generation')
        require(0 < integer(phase['build_frame']) <= integer(phase['instance_available_frame']) <=
                integer(phase['capture_frame']), 'invalid build/frame order')
        require(phase['readback_consumed_frame'] == phase['instance_available_frame'], 'readback join')
        require(phase['width'] == 3840 and phase['height'] == 1600, 'wrong pinned dimensions')
        require(phase['fade_start_m'] == 48 and phase['fade_end_m'] == 92, 'weakened fade workload')
        require(abs(number(phase['density_scale']) - 1.6 * number(report['profile_settings']['requested_density_multiplier'])) < 1e-5,
                'wrong density scale')
        require(abs(number(phase['camera']['yaw_degrees']) - 35) < 1e-4 and
                abs(number(phase['camera']['pitch_degrees']) + 18) < 1e-4 and
                abs(number(phase['camera']['vertical_fov_degrees']) - 60) < 1e-4 and
                abs(number(phase['time_of_day']) - .04) < 1e-6, 'wrong camera/TOD')
        require(phase['shader_time_seconds'] == 1, 'shader phase is not pinned')
        require(phase['wind_input_xz'] == ([0, 0] if index == 0 else [6, 0]), 'wrong wind inputs')
        camera = phase['camera']['position']
        require(len(camera) == 3 and all(math.isfinite(number(x)) for x in camera) and
                abs(camera[1] - number(phase['camera']['terrain_height_m']) - 2.4) < 1e-4, 'camera height')
        data = local(root, phase['screenshot'], 3840 * 1600 * 3 + 4096)
        match = re.match(rb'P6\s+3840\s+1600\s+255(?:\r\n|\n)', data[:4096])
        require(match is not None and len(data) - match.end() == 3840 * 1600 * 3, 'invalid original PPM')
        artifacts[phase['screenshot']] = data
    calm, windy = phases
    require(calm['capture_frame'] < windy['build_frame'] and
            calm['build_generation'] < windy['build_generation'] and
            calm['camera']['position'] == windy['camera']['position'], 'unmatched phases')
    r = report['determinism']
    require(r['completed'] is True and r['output_cleared'] is True and r['byte_equal'] is True,
            'no independently rebuilt output')
    require(0 < integer(r['first_generation']) < integer(r['second_generation']) == calm['build_generation'] and
            0 < integer(r['first_frame']) < integer(r['second_frame']) == calm['build_frame'], 'rebuild provenance')
    require(integer(r['input_hash']) > 0 and r['stride_bytes'] == 36, 'input identity/record format')
    count = integer(r['first_count'])
    require(100000 <= count <= 262144 and count == r['second_count'], 'rebuild counts')
    require(r['first_artifact'] == 'foliage-rebuild-first.bin' and
            r['second_artifact'] == 'foliage-rebuild-second.bin', 'fixed distinct rebuild paths required')
    first = local(root, r['first_artifact'], 262144 * 36)
    second = local(root, r['second_artifact'], 262144 * 36)
    require(len(first) == len(second) == count * 36 and first == second, 'raw rebuild bytes differ')
    require(fnv(first) == r['first_hash'] == r['second_hash'] == calm['instance_snapshot_hash'], 'raw rebuild hashes')
    artifacts[r['first_artifact']] = first; artifacts[r['second_artifact']] = second
    coverage = report['coverage_density']; fade = report['distance_fade']
    require(coverage['instances_total'] == count and coverage['minimum_instances'] == 100000 and
            coverage['normalizer'] == 873813 and fade['fade_start_m'] == 48 and fade['fade_end_m'] == 92,
            'coverage/fade workload identity')
    within = 0
    eye = calm['camera']['position']
    for offset in range(0, len(first), 36):
        x, y, z, half_width, height = struct.unpack_from('<5f', first, offset)
        require(all(math.isfinite(v) for v in (x,y,z,half_width,height)) and half_width > 0 and height > 0,
                'invalid raw instance')
        if math.hypot(x-eye[0], z-eye[2]) <= 92: within += 1
    require(within == count == coverage['instances_within_ring'] and fade['instances_beyond_fade'] == 0 and
            abs(number(coverage['calibrated_count_ratio']) - count/873813) < 1e-9 and
            abs(count/873813-number(coverage['biome_density'])) <= number(coverage['density_band']) and
            0 <= coverage['density_band'] <= .6, 'raw coverage/count band failed')
    require(report['render_pass']['foliage_instances_drawn'] == count and
            1 <= report['render_pass']['foliage_draws'] <= 3, 'draw/count proof')
    motion = report['rendered_motion']
    require(motion['format'] == 'world_xyz,height_t,clip_xyzw.float32' and
            motion['rasterizer_discard'] is False and motion['instances_per_phase'] == 64 and
            motion['vertices_per_instance'] == 12, 'invalid actual-draw vertex format')
    require(motion['windy_instance_artifact'] == 'foliage-windy.bin', 'windy artifact path')
    wind_bytes = local(root, 'foliage-windy.bin', 262144 * 36)
    require(len(wind_bytes) == len(first) and fnv(wind_bytes) == windy['instance_snapshot_hash'], 'windy raw build')
    artifacts['foliage-windy.bin'] = wind_bytes
    # Every instance must preserve geometry and order; ONLY its wind words may change.
    for offset in range(0, len(first), 36):
        require(first[offset:offset+24] == wind_bytes[offset:offset+24] and
                first[offset+32:offset+36] == wind_bytes[offset+32:offset+36], 'wind changed instance geometry')
        expected = 6.0 if first[offset+23] else 0.0
        require(struct.unpack_from('<2f', first, offset+24) == (0.0, 0.0) and
                struct.unpack_from('<2f', wind_bytes, offset+24) == (expected, 0.0), 'instance wind words')
    vertices = motion['phases']
    require(len(vertices) == 2, 'missing vertex phase')
    for index, v in enumerate(vertices):
        phase = phases[index]
        require(v['phase'] == index+1 and v['source_frame'] == phase['capture_frame'] and
                integer(v['available_frame']) >= v['source_frame'] and
                v['generation'] == phase['build_generation'] and
                v['instance_hash'] == phase['instance_snapshot_hash'] and v['shader_time_seconds'] == 1,
                'vertex/image/build/frame join failed')
        require(len(v['vertices']) == 768 and len(v['blade_heights']) == len(v['sways']) == 64,
                'bounded vertex sample size')
        require(0 <= integer(v['first_instance']) <= count - 64, 'sample range outside draw')
        require(len(v['view_projection']) == 16, 'missing actual projection')
        for value in v['view_projection']: number(value)
        expected = expected_view_projection(phase['camera'])
        require(all(abs(x-y) <= .002 for x,y in zip(v['view_projection'],expected)), 'camera/actual draw projection mismatch')
    a, b = vertices
    require(a['first_instance'] == b['first_instance'] and a['view_projection'] == b['view_projection'] and
            a['blade_heights'] == b['blade_heights'] and a['sways'] == b['sways'], 'unmatched vertex controls')
    roots = []; tips = []; controls = []; visible = 0
    for blade in range(64):
        offset = (a['first_instance'] + blade) * 36
        x, y, z, half_width, height = struct.unpack_from('<5f', first, offset)
        facing = struct.unpack_from('<e', first, offset+34)[0]
        sways = first[offset+23] != 0
        require(type(a['sways'][blade]) is bool and a['sways'][blade] == sways and
                a['blade_heights'][blade] == height and 0 < height <= 10, 'raw sample attributes')
        seen = False
        for j in range(12):
            va, vb = a['vertices'][blade*12+j], b['vertices'][blade*12+j]
            require(len(va) == len(vb) == 8, 'vertex stride')
            for value in va+vb: number(value)
            tip = j % 6 in (2, 4, 5)
            require(va[3] == vb[3] == int(tip), 'vertex identity/height class')
            yaw = facing + (1.5707963 if j >= 6 else 0)
            side = -1 if j % 6 in (0, 2, 5) else 1
            expected = [x + math.cos(yaw)*side*half_width, y + (height if tip else -.06),
                        z + math.sin(yaw)*side*half_width]
            require(math.dist(va[:3], expected) <= .0001, 'calm vertices do not belong to selected actual instances')
            for vertex in (va, vb):
                projected = [sum(a['view_projection'][col*4+row] * (*vertex[:3], 1)[col]
                                 for col in range(4)) for row in range(4)]
                require(all(abs(projected[k]-vertex[4+k]) <= .002 for k in range(4)), 'world/clip matrix join')
            distance = math.dist(va[:3], vb[:3])
            if not tip: roots.append(distance)
            elif not sways: controls.append(distance)
            else:
                tips.append(distance)
                require(.00001 < distance <= height*.45+.0001 and abs(va[1]-vb[1]) <= .00001,
                        'wind tip response missing or unbounded')
            if tip and all(v[7] > 0 and abs(v[4]) < v[7] and abs(v[5]) < v[7] and
                           0 < v[6] < v[7] for v in (va, vb)): seen = True
        if sways and seen: visible += 1
    require(roots and max(roots) <= .00001 and controls and max(controls) <= .00001 and
            len(tips) >= 48 and visible >= 8, 'moving roots/control or insufficient visible tips')
    timer = report['gpu_timer']; samples = timer['samples']
    require(timer['budget_ms'] == .6 and timer['instrumented_vertex_frames_excluded'] is True and
            len(samples) == 64, 'weakened timing contract')
    for name in ('gl_vendor', 'gl_renderer', 'gl_version'):
        require(type(timer[name]) is str and bool(timer[name].strip()), 'missing actual GL identity')
    frames = set(); counts = [0, 0]; values = []; previous = 0
    for index, sample in enumerate(samples):
        phase_index = integer(sample['phase']) - 1
        require(phase_index in (0, 1), 'invalid timing phase')
        phase = phases[phase_index]; frame = integer(sample['source_frame'])
        require(sample['query_id'] == index+1 and frame > previous and frame not in frames and
                phase['instance_available_frame'] <= frame < phase['capture_frame'], 'GPU source frame join')
        require(sample['generation'] == sample['instance_generation'] == phase['build_generation'] and
                sample['instance_count'] == count and sample['shader_time_seconds'] == 1, 'GPU workload join')
        value = number(sample['milliseconds']); require(value > 0, 'unavailable GPU timing')
        values.append(value); counts[phase_index] += 1; frames.add(frame); previous = frame
    require(counts == [32, 32] and abs(timer['mean_ms']-sum(values)/64) < 1e-9 and
            timer['maximum_ms'] == max(values), 'GPU aggregates do not match raw frames')
    require(max(values) <= .6, 'actual foliage GPU maximum exceeds unchanged 0.6 ms budget')
    require(report['passed'] is True and report['qualification']['status'] == 'complete' and
            report['qualification']['missing'] == [], 'producer full qualification failed')
    return {'schema': 'luminumbra.foliage_evidence_validation.v1', 'passed': True,
            'visual_approved': False, 'rebuild_bytes': len(first), 'sampled_vertices': 1536,
            'unique_gpu_frames': 64, 'gpu_mean_ms': sum(values)/64, 'gpu_maximum_ms': max(values),
            'scope': 'same-device native renderer; not cross-host or Blender edit latency',
            'artifacts': {name: {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in artifacts.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact_dir', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.artifact_dir), indent=2))
        return 0
    except (Refusal, KeyError, TypeError, ValueError, OSError) as error:
        print(json.dumps({'passed': False, 'refusal': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
