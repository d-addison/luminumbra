"""Measure actual baseline importer/runtime behavior without claiming fixes."""
from datetime import datetime, timezone
import io
import json
from pathlib import Path
import struct
import subprocess

from common import ROOT, SOURCE, PIN, canonical, digest, file_digest, module
from fixtures import builder, cases, IDENTITY


def multiply(a, b):
    return [sum(a[k * 4 + row] * b[col * 4 + k] for k in range(4))
            for col in range(4) for row in range(4)]


def local_matrix(node):
    if 'matrix' in node:
        return node['matrix']
    x, y, z, w = node.get('rotation', [0, 0, 0, 1])
    scale = node.get('scale', [1, 1, 1])
    matrix = [1-2*(y*y+z*z), 2*(x*y+z*w), 2*(x*z-y*w), 0,
              2*(x*y-z*w), 1-2*(x*x+z*z), 2*(y*z+x*w), 0,
              2*(x*z+y*w), 2*(y*z-x*w), 1-2*(x*x+y*y), 0,
              *node.get('translation', [0, 0, 0]), 1]
    for col in range(3):
        for row in range(3):
            matrix[col*4+row] *= scale[col]
    return matrix


def bounds(positions):
    return {'min': [min(p[i] for p in positions) for i in range(3)],
            'max': [max(p[i] for p in positions) for i in range(3)]}


def main():
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    run = ROOT / 'evidence' / ('compiled-readback-' + stamp)
    run.mkdir(parents=True)
    source_commit = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    if source_commit != PIN:
        raise RuntimeError('Compiled baseline evidence requires the recorded source revision')
    processor = ROOT / 'install/asset-evidence/bin/asset_processor'
    animation = ROOT / 'install/asset-evidence/bin/animation_readback'
    report = {'source_commit': source_commit,
              'scope': 'Linux CPU importer/runtime reading Windows Blender and synthetic GLBs',
              'rendered_fidelity': False, 'production_support_claim': False,
              'binary_hashes_before': {p.name: file_digest(p) for p in (processor, animation)}, 'cases': []}
    def execute(args):
        result = subprocess.run([str(x) for x in args], cwd=run, capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(str(args) + '\n' + result.stdout + result.stderr)
        return {'argv': [str(x) for x in args], 'returncode': result.returncode,
                'stdout': result.stdout, 'stderr': result.stderr}
    parser = module('baseline_validator', 'tools/blender/validate_glb.py')
    native = ROOT / 'probes/native-runs'
    selected = []
    for suite in ('prop', 'plant', 'character'):
        paths = list(native.glob(suite + '-*/outputs/first.glb'))
        selected.append((suite, max(paths, key=lambda p: p.stat().st_mtime), False))
    for name in ('step_animation', 'cubicspline_animation', 'child_before_parent', 'matrix_joint_bind'):
        doc, binary, _ = cases()[name]
        path = run / (name + '.glb')
        builder().write_glb(path, doc, binary)
        selected.append((name, path, True))
    for name, source, synthetic in selected:
        directory = run / name
        directory.mkdir()
        output = directory / 'first.lmesh'
        repeat = directory / 'repeat.lmesh'
        commands = [execute([processor, source, target]) for target in (output, repeat)]
        document, binary = parser.parse_glb(source)
        validator = parser.Validator(source, document, binary)
        validator.validate()
        record = {'case': name, 'input': str(source), 'synthetic': synthetic, 'commands': commands,
                  'input_sha256': file_digest(source), 'output_sha256': file_digest(output),
                  'output_bytes': output.stat().st_size, 'repeat_bytes_equal': output.read_bytes() == repeat.read_bytes(),
                  'validator_findings': validator.findings,
                  'material_alpha_modes': [m.get('alphaMode', 'OPAQUE') for m in document.get('materials', [])]}
        data = output.read_bytes()
        if data[:4] == b'LMSH':
            count, indices = struct.unpack_from('<II', data, 4)
            points = [struct.unpack_from('<3f', data, 28 + i*32) for i in range(count)]
            record.update(vertices=count, triangles=indices//3, compiled_bounds=bounds(points))
            world_points = []
            def walk(index, parent):
                node = document['nodes'][index]
                transform = multiply(parent, local_matrix(node))
                if 'mesh' in node:
                    for primitive in document['meshes'][node['mesh']]['primitives']:
                        positions = validator._decode_accessor_unchecked(primitive['attributes']['POSITION'])
                        for point in positions:
                            world_points.append([sum(transform[col*4+row]*point[col] for col in range(3)) + transform[12+row] for row in range(3)])
                for child in node.get('children', []):
                    walk(child, transform)
            for index in document['scenes'][document.get('scene', 0)]['nodes']:
                walk(index, IDENTITY)
            expected = bounds(world_points)
            record['authored_world_bounds'] = expected
            record['world_bounds_preserved'] = max(abs(record['compiled_bounds'][key][i] - expected[key][i])
                                                   for key in ('min', 'max') for i in range(3)) < 1e-5
            if name == 'plant':
                primitive = document['meshes'][0]['primitives'][0]
                count = document['accessors'][primitive['attributes']['POSITION']]['count']
                index_values = [v[0] for v in validator._decode_accessor_unchecked(primitive['indices'])]
                parents = list(range(count))
                def find(i):
                    while parents[i] != i:
                        i = parents[i]
                    return i
                for start in range(0, len(index_values), 3):
                    a, b, c = index_values[start:start+3]
                    parents[find(b)] = find(a)
                    parents[find(c)] = find(a)
                record['index_connected_components'] = len({find(i) for i in index_values})
                # Exporter splits shared vertices at authored flat-normal seams.
                # Count geometric leaf connectivity separately, without changing data.
                positions = validator._decode_accessor_unchecked(primitive['attributes']['POSITION'])
                by_position = {}
                for index, position in enumerate(positions):
                    if position in by_position:
                        parents[find(index)] = find(by_position[position])
                    else:
                        by_position[position] = index
                record['connected_surfaces_after_exact_position_weld'] = len({find(i) for i in index_values})
                from PIL import Image
                images = []
                for image in document.get('images', []):
                    view = document['bufferViews'][image['bufferView']]
                    start = view.get('byteOffset', 0)
                    pixels = Image.open(io.BytesIO(binary[start:start+view['byteLength']])).convert('RGBA')
                    values = list(pixels.getdata())
                    images.append({'dimensions': list(pixels.size), 'alpha_coverage_at_0_5': sum(p[3] >= 128 for p in values)/len(values)})
                record['images'] = images
        else:
            assert data[:4] == b'LMS2'
            vertices, indices, joints = struct.unpack_from('<III', data, 8)
            record.update(vertices=vertices, triangles=indices//3, joints=joints)
            bind = execute([animation, output])
            record['runtime_bind'] = json.loads(bind['stdout'])
            palette = record['runtime_bind']['palette']
            record['bind_palette_max_identity_error'] = max(abs(v - IDENTITY[i % 16]) for i, v in enumerate(palette))
            clips = list(directory.glob('first.*.lanim'))
            if clips:
                sample = execute([animation, output, clips[0]])
                record['runtime_sample_at_0_5'] = json.loads(sample['stdout'])
                if name in ('step_animation', 'cubicspline_animation'):
                    expected = 0 if name == 'step_animation' else 1
                    actual = record['runtime_sample_at_0_5']['translations'][0][1]
                    record.update(expected_root_y=expected, observed_root_y=actual, interpolation_preserved=abs(expected-actual)<1e-6)
        report['cases'].append(record)
        (run / 'report.json').write_text(json.dumps(report, indent=2))
    report['binary_hashes_after'] = {p.name: file_digest(p) for p in (processor, animation)}
    report['binaries_unchanged'] = report['binary_hashes_before'] == report['binary_hashes_after']
    report['probe_completed'] = True
    (run / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'report': str(run / 'report.json'), 'cases': len(report['cases']),
                      'binaries_unchanged': report['binaries_unchanged'],
                      'findings': [{k:v for k,v in item.items() if k in ('case','world_bounds_preserved','index_connected_components','connected_surfaces_after_exact_position_weld',
                        'bind_palette_max_identity_error','expected_root_y','observed_root_y','interpolation_preserved','images')} for item in report['cases']]}, indent=2))


if __name__ == '__main__':
    main()
