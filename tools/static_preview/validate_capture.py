#!/usr/bin/env python3
"""Validate static-preview files against independent process/build and request pins.

No renderer execution or image editing. Expectations must come from the external
runner/build manifest, never be synthesized from capture.json. project_roots maps
request prefab IDs to trusted collected project copies (native paths may differ).
"""
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


def require(ok, message):
    if not ok:
        raise Refusal(message)


def number(value):
    require(type(value) in (int, float) and math.isfinite(value), 'nonfinite/nonnumeric value')
    return value


def uint(value, maximum=(1 << 64) - 1):
    require(type(value) is int and 0 <= value <= maximum, 'invalid unsigned integer')
    return value


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(value, length=64):
    require(type(value) is str and re.fullmatch('[0-9a-f]{' + str(length) + '}', value), 'invalid digest')
    return value


def unlinked(path):
    require(not path.is_symlink() and not (getattr(path.lstat(), 'st_file_attributes', 0) & 0x400),
            'symlink/reparse input refused')


def root_path(path):
    path = Path(path).absolute()
    for part in (path, *path.parents):
        unlinked(part)
    require(path.is_dir(), 'expected directory')
    return path


def local(root, name, limit):
    require(type(name) is str and re.fullmatch(r'[A-Za-z0-9_. /-]+', name), 'unsafe member name')
    relative = Path(name)
    require(not relative.is_absolute() and '..' not in relative.parts, 'escaped member')
    path = root
    for part in relative.parts:
        path = path / part
        unlinked(path)
    require(path.is_file(), 'missing regular member')
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    require(0 < len(data) <= limit, 'empty/oversized member')
    return data


def document(data):
    def pairs(rows):
        result = {}
        for key, value in rows:
            require(key not in result, 'duplicate JSON field')
            result[key] = value
        return result
    result = json.loads(data, object_pairs_hook=pairs,
                        parse_constant=lambda _: (_ for _ in ()).throw(Refusal('nonfinite JSON')))
    require(type(result) is dict, 'JSON object required')
    return result


def matrix(value, size=16):
    require(type(value) is list and len(value) == size, 'matrix shape')
    return [number(v) for v in value]


def product(a, b):
    return [sum(a[k*4+r] * b[c*4+k] for k in range(4)) for c in range(4) for r in range(4)]


def determinant(a):
    return (a[0]*(a[5]*a[10]-a[9]*a[6]) - a[4]*(a[1]*a[10]-a[9]*a[2]) +
            a[8]*(a[1]*a[6]-a[5]*a[2]))


def affine(value):
    a = matrix(value)
    require([a[3], a[7], a[11], a[15]] == [0, 0, 0, 1] and
            all(abs(v) <= 1e30 for v in a) and abs(determinant(a)) > 1e-12, 'invalid affine transform')
    return a


def normals(a):
    # Cofactor matrix / determinant is inverse transpose (column-major output).
    d = determinant(a)
    return [(a[5]*a[10]-a[9]*a[6])/d, (a[8]*a[6]-a[4]*a[10])/d,
            (a[4]*a[9]-a[8]*a[5])/d, (a[9]*a[2]-a[1]*a[10])/d,
            (a[0]*a[10]-a[8]*a[2])/d, (a[8]*a[1]-a[0]*a[9])/d,
            (a[1]*a[6]-a[5]*a[2])/d, (a[4]*a[2]-a[0]*a[6])/d,
            (a[0]*a[5]-a[4]*a[1])/d]


def near_values(a, b, tolerance=1e-9):
    return len(a) == len(b) and all(abs(number(x)-number(y)) <= tolerance*max(1, abs(y))
                                   for x, y in zip(a, b))


def f32(value):
    value = struct.unpack('<f', struct.pack('<f', number(value)))[0]
    require(math.isfinite(value), 'unrepresentable GPU value')
    return value


def camera_check(camera, frame):
    width, height = uint(camera['width'], 4096), uint(camera['height'], 4096)
    require(width > 0 and height > 0 and uint(camera['revision']) > 0, 'invalid camera extent/revision')
    view = affine(camera['view']); projection = matrix(camera['projection'])
    require(all(abs(v) <= 1e6 for v in view) and abs(determinant(view)-1) <= 1e-6, 'camera view handedness/range')
    for c in range(3):
        for r in range(3):
            require(abs(sum(view[c*4+k]*view[r*4+k] for k in range(3)) - (c == r)) <= 1e-6,
                    'camera view must be rigid')
    n, f = number(camera['near_plane']), number(camera['far_plane'])
    require(.001 <= n < f <= 1e6, 'invalid clipping planes')
    expected = [0.0]*16
    expected[0] = projection[0]; expected[5] = projection[5]
    expected[10] = n/(f-n); expected[11] = -1; expected[14] = f*n/(f-n)
    require(.01 < expected[0] < 1000 and .01 < expected[5] < 1000 and
            abs(expected[5]/expected[0]-width/height) <= 1e-6*max(1,width/height), 'camera aspect/FOV')
    require(all(abs(x-y) <= max(1e-12, abs(y)*2e-6) for x,y in zip(projection, expected)),
            'camera reversed perspective mismatch')
    require(frame['camera'] == camera, 'request camera relabelled')
    for field, source in [('actual_view', view), ('actual_projection', projection)]:
        values = matrix(frame[field])
        # JSON float serialization may round its decimal spelling, but must round-trip
        # to exactly the GPU float bits cast from the externally pinned request.
        require(all(struct.pack('<f', x) == struct.pack('<f', y) for x,y in zip(values, source)),
                'actual GPU matrix differs from request')
        for value in values: f32(value)
    return width, height


def load_prefabs(request, expectations):
    prefabs = {}
    require(1 <= len(request['prefabs']) <= 8 and type(expectations['project_roots']) is dict, 'prefab extent')
    for item in request['prefabs']:
        identity = item['id']
        require(type(identity) is str and identity not in prefabs, 'duplicate prefab ID')
        generation, pinned = digest(item['generation_id'], 32), digest(item['manifest_sha256'])
        root = root_path(expectations['project_roots'][identity])
        prefix = '.luminumbra-author/generations/' + generation + '/'
        raw = local(root, prefix+'manifest.json', 1024*1024)
        require(sha(raw) == pinned, 'pinned generation manifest changed')
        manifest = document(raw)
        require(manifest['schema'] == 'luminumbra.authoring.generation.v1' and manifest['job_id'] == generation,
                'generation manifest identity')
        total = 0; descriptor = None
        require(1 <= len(manifest['outputs']) <= 8192, 'manifest output extent')
        for name, info in manifest['outputs'].items():
            size = uint(info['bytes'], 256*1024*1024); total += size
            require(total <= 256*1024*1024, 'generation byte budget')
            payload = local(root, prefix+name, size)
            require(len(payload) == size and sha(payload) == digest(info['sha256']), 'generation member identity')
            if name == 'prefab.json': descriptor = document(payload)
        require(descriptor is not None and descriptor['schema'] == 'luminumbra.asset.prefab.v1', 'prefab descriptor schema')
        nodes = descriptor['nodes']; require(1 <= len(nodes) <= 4096, 'node extent')
        by_id = {n['id']: n for n in nodes}; require(len(by_id) == len(nodes), 'duplicate node')
        prefabs[identity] = (item, descriptor, manifest, by_id)
    require(set(prefabs) == set(expectations['project_roots']), 'unexpected project root pin')
    return prefabs


def scene_draws(instances, prefabs):
    result = {}; index_count = 0
    for identity, instance in sorted(instances.items()):
        item, descriptor, manifest, nodes = prefabs[instance['prefab']]
        worlds = {}; pending = dict(nodes)
        while pending:
            ready = [key for key, node in pending.items() if node['parent'] is None or node['parent'] in worlds]
            require(ready, 'cyclic/missing parent')
            for key in ready:
                node = pending.pop(key)
                world = affine(product(instance['placement'] if node['parent'] is None else worlds[node['parent']],
                                       instance['updates'].get(key, affine(node['local_matrix']))))
                worlds[key] = world
                for primitive, draw in enumerate(descriptor['meshes'][node['mesh']] if 'mesh' in node else []):
                    require(len(result) < 1024, 'draw extent')
                    require(descriptor['materials'][draw['material']]['alpha_mode'] in ('OPAQUE', 'MASK'), 'unsupported material')
                    result[(identity, key, primitive)] = {
                        'instance_id':identity, 'node_id':key, 'primitive_index':primitive,
                        'material_id':draw['material'], 'model':world, 'normal':normals(world),
                        'reverse_front_face':determinant(world)<0, 'generation_id':item['generation_id'],
                        'manifest_sha256':item['manifest_sha256']}
                    index_count += uint(manifest['outputs'][draw['file']]['triangles'])*3
    return result, index_count


def validate(artifact_dir, request_path, expectations_path):
    root = root_path(artifact_dir)
    ep = Path(expectations_path).absolute(); rp = Path(request_path).absolute()
    expectations = document(local(root_path(ep.parent), ep.name, 2*1024*1024))
    request_raw = local(root_path(rp.parent), rp.name, 1024*1024); request = document(request_raw)
    require(request['schema'] == 'luminumbra.static_preview.request.v1', 'request schema')
    require(sha(request_raw) == digest(expectations['request_sha256']), 'request pin mismatch')
    raw = local(root, 'capture.json', 2*1024*1024)
    require(sha(raw) == digest(expectations['capture_sha256']), 'external capture receipt pin mismatch')
    capture = document(raw)
    require(capture['schema'] == 'luminumbra.static_preview.capture.v1' and capture['status'] == 'complete' and
            capture['shutdown_complete'] is True and capture['visual_approved'] is False and capture['watchdog_seconds'] == 60,
            'capture incomplete/approval/watchdog mismatch')
    require(capture['profile'] == 'static-perspective-opaque-mask-v1', 'capture profile')
    require(expectations['exit_code'] == 0 and type(expectations['exit_code']) is int and
            expectations['timed_out'] is False and expectations['cancelled'] is False and
            uint(expectations['pid']) > 0 and capture['pid'] == expectations['pid'], 'external process failed')
    for key in ('source_commit','source_input_sha256','executable_sha256','module_sha256','request_sha256'):
        digest(expectations[key], 40 if key == 'source_commit' else 64)
        require(capture[key] == expectations[key], 'external '+key+' pin mismatch')
    require(type(expectations['source_dirty']) is bool and capture['source_dirty'] is expectations['source_dirty'], 'source dirtiness pin')
    resources = {'static_asset.vert','static_asset.frag','lighting_pass.vert','lighting_pass.frag','config_constants.gen.glsl'}
    require(set(capture['resources']) == set(expectations['resources']) == resources, 'resource set')
    for name in resources:
        require(capture['resources'][name] == digest(expectations['resources'][name]), 'external resource hash mismatch')
    prefabs = load_prefabs(request, expectations)
    require(capture['generations'] == {k:{'generation_id':v[0]['generation_id'],'manifest_sha256':v[0]['manifest_sha256']}
                                       for k,v in prefabs.items()}, 'generation receipt join')
    require(1 <= len(request['instances']) <= 64, 'instance extent')
    instances = {}
    for item in request['instances']:
        require(item['id'] not in instances and item['prefab'] in prefabs, 'instance identity')
        instances[item['id']] = {'prefab':item['prefab'], 'placement':affine(item['placement']), 'updates':{}}
    revision = len(instances); previous_camera = 0; previous_draws = {}; gpu = None
    frames = request['frames']; require(1 <= len(frames) <= 8 and len(capture['frames']) == len(frames), 'frame count')
    allowed = {'capture.json'} | {'frame-'+str(i+1) for i in range(len(frames))}
    require({p.name for p in root.iterdir()} == allowed, 'unexpected failure/hang/partial capture artifacts')
    hashes = {'capture.json':sha(raw)}; summaries=[]
    for i, (command, reference) in enumerate(zip(frames, capture['frames'])):
        directory = 'frame-'+str(i+1)
        require(reference['directory'] == directory, 'frame directory sequence')
        raw = local(root, directory+'/frame.json', 2*1024*1024)
        require(sha(raw) == digest(reference['receipt_sha256']), 'frame receipt digest')
        frame = document(raw); hashes[directory+'/frame.json'] = sha(raw)
        require(uint(frame['sequence']) == i+1 and frame['origin'] == 'bottom-left' and
                frame['deferred_position_format'] == 'RGB32F', 'frame sequence/origin/position format')
        width, height = camera_check(command['camera'], frame)
        require(command['camera']['revision'] > previous_camera, 'camera revision not increasing')
        previous_camera = command['camera']['revision']
        updates = command.get('updates', []); require(type(updates) is list and len(updates) <= 65536, 'update extent')
        unique = set()
        for update in updates:
            key = (update['instance_id'],update['node_id'])
            require(key not in unique and key[0] in instances, 'duplicate/unknown update')
            unique.add(key); instance = instances[key[0]]
            require(key[1] in prefabs[instance['prefab']][3], 'unknown node update')
            instance['updates'][key[1]] = affine(update['local'])
        if updates: revision += 1
        require(uint(frame['scene_revision']) == revision, 'scene revision mismatch')
        expected_draws, indices = scene_draws(instances, prefabs)
        require(type(frame['instances']) is list and len(frame['instances']) == len(expected_draws), 'draw record count')
        seen=set()
        for actual in frame['instances']:
            key=(actual['instance_id'],actual['node_id'],uint(actual['primitive_index']))
            require(key in expected_draws and key not in seen, 'draw identity mismatch'); seen.add(key)
            expected = expected_draws[key]
            require(set(actual) == set(expected), 'draw fields')
            for name in expected:
                if name in ('model','normal'):
                    require(near_values(matrix(actual[name], len(expected[name])),expected[name]), 'draw '+name+' mismatch')
                else:
                    if name == 'reverse_front_face': require(type(actual[name]) is bool, 'winding must be boolean')
                    require(actual[name] == expected[name], 'draw '+name+' mismatch')
        require(uint(frame['draws']) == len(expected_draws) and uint(frame['indices']) == indices, 'actual draw/count mismatch')
        changed=sum(key not in previous_draws or row['model'] != previous_draws[key]['model'] for key,row in expected_draws.items())
        require(uint(frame['updated_instances']) == changed, 'dirty transform count mismatch'); previous_draws=expected_draws
        if i: require(uint(frame['mesh_uploads']) == 0 and uint(frame['texture_uploads']) == 0, 'transform/camera edit rebuilt geometry')
        else: uint(frame['mesh_uploads'],1024);uint(frame['texture_uploads'],5120)
        require(number(frame['synchronous_render_readback_ms']) >= 0, 'invalid duration')
        identity=frame['gpu']
        require(set(identity) == {'vendor','renderer','version'} and all(type(v) is str and v.strip() for v in identity.values()), 'GPU identity')
        if gpu is None: gpu=identity
        require(gpu == identity, 'GPU changed between frames')
        if 'gpu' in expectations: require(identity == expectations['gpu'], 'external GPU identity')
        else:
            required=expectations['required_renderer']; require(type(required) is str and required.strip() and required in identity['renderer'], 'required renderer absent')
        payloads={}
        for name,filename,size in [('color','color.rgba8',width*height*4),('depth','depth.f32le',width*height*4),('coverage','coverage.u8',width*height)]:
            info=frame[name];require(info['file'] == filename and uint(info['bytes']) == size, 'plane name/size')
            data=local(root,directory+'/'+filename,size)
            require(len(data) == size and sha(data) == digest(info['sha256']), 'raw plane digest/size')
            hashes[directory+'/'+filename]=sha(data);payloads[name]=data
        require({p.name for p in (root/directory).iterdir()} == {'frame.json','color.rgba8','depth.f32le','coverage.u8'}, 'unexpected frame member')
        require(frame['color']['encoding'] == 'production-inspection-power-2.2' and frame['color']['alpha'] == 'straight-coverage', 'color encoding')
        d=frame['depth'];require(d['range'] == [0,1] and d['near'] == 1 and d['clear'] == 0 and d['projection'] == 'RH-ZO-reversed-finite', 'depth convention')
        covered=0
        for pixel,(depth,) in enumerate(struct.iter_unpack('<f',payloads['depth'])):
            require(math.isfinite(depth) and 0 <= depth <= 1, 'invalid raw reversed depth')
            coverage=payloads['coverage'][pixel];require(coverage in (0,1) and coverage == int(depth > 0), 'depth/coverage mismatch')
            require(payloads['color'][pixel*4+3] == coverage*255, 'alpha/coverage mismatch');covered+=coverage
        require(covered == uint(frame['coverage']['covered_pixels']), 'covered pixel count')
        minimum=expectations.get('minimum_covered_pixels',[1]*len(frames))
        require(len(minimum) == len(frames) and covered >= uint(minimum[i],width*height), 'insufficient expected coverage')
        summaries.append({'sequence':i+1,'scene_revision':revision,'covered_pixels':covered,'draws':len(expected_draws)})
    return {'schema':'luminumbra.static_preview.validation.v1','passed':True,'visual_approved':False,
            'source_commit':capture['source_commit'],'frames':summaries,'gpu':gpu,'artifact_sha256':hashes,
            'expectations_sha256':sha(local(root_path(ep.parent),ep.name,2*1024*1024)),
            'scope':'file validation joined to caller-supplied independent pins; no renderer execution, visual approval or latency qualification'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact_dir',type=Path);parser.add_argument('--request',required=True,type=Path)
    parser.add_argument('--expect',required=True,type=Path);args=parser.parse_args()
    try:
        print(json.dumps(validate(args.artifact_dir,args.request,args.expect),indent=2));return 0
    except (Refusal,KeyError,TypeError,ValueError,OverflowError,OSError,RecursionError,struct.error) as error:
        print(json.dumps({'passed':False,'refusal':str(error)}));return 1


if __name__ == '__main__':
    raise SystemExit(main())
