"""Source-generated tiny candidate fixtures; no committed binaries or Blender."""
from copy import deepcopy
import math
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'probes'))
from mock_backend import synthetic_glb
from test_meshgen_config import project_fixture
from luminumbra_author.contracts import canonical, digest
from luminumbra_author.meshgen_backend import MOCK_CAPABILITY, bounded_json
from luminumbra_author.meshgen_config import resolve
from luminumbra_author.meshgen_provenance import build_evidence

EXPORTER = digest(b'explicitly simulated exporter')


def png():
    def chunk(kind, payload): return struct.pack('>I', len(payload))+kind+payload+struct.pack('>I', zlib.crc32(kind+payload)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))+chunk(b'IDAT', zlib.compress(b'\x00\x80\x80\x80'))+chunk(b'IEND', b'')


def artifact():
    raw = synthetic_glb()
    size = struct.unpack_from('<I', raw, 12)[0]
    doc = bounded_json(raw[20:20+size])
    binary = raw[28+size:]
    points = [struct.unpack_from('<3f', binary, i*12) for i in range(8)]
    indices = struct.unpack_from('<36H', binary, 96)
    vertices, normals, uvs = [], [], []
    for at in range(0, 36, 3):
        face = [points[i] for i in indices[at:at+3]]
        a, b, c = face
        ab, ac = [b[i]-a[i] for i in range(3)], [c[i]-a[i] for i in range(3)]
        normal = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
        norm = math.sqrt(sum(v*v for v in normal))
        vertices.extend([(x, y+.5, z) for x, y, z in face]); normals.extend([tuple(v/norm for v in normal)]*3)
        x, y = (at//3 % 4)*.25, (at//3//4)*.25
        uvs.extend([(x+.01, y+.01), (x+.2, y+.01), (x+.01, y+.2)])
    blob = b''.join(struct.pack('<3f', *v) for v in vertices+normals)+b''.join(struct.pack('<2f', *v) for v in uvs)
    doc = {'asset': {'version': '2.0'}, 'buffers': [{'byteLength': len(blob)}],
        'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 432}, {'buffer': 0, 'byteOffset': 432, 'byteLength': 432},
                        {'buffer': 0, 'byteOffset': 864, 'byteLength': 288}],
        'accessors': [{'bufferView': i, 'componentType': 5126, 'type': 'VEC2' if i == 2 else 'VEC3', 'count': 36} for i in range(3)],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}}]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    return glb(doc, blob)


def glb(doc, blob):
    payload = canonical(doc); payload += b' '*(-len(payload)%4)
    blob += b'\0'*(-len(blob)%4)
    return struct.pack('<4sII', b'glTF', 2, 28+len(payload)+len(blob))+struct.pack('<II', len(payload), 0x4e4f534a)+payload+struct.pack('<II', len(blob), 0x004e4942)+blob


def observations(resolved):
    return {'finish': {'blender_version': 'simulated', 'exporter_sha256': EXPORTER,
            'steps': [{'id': step, 'params': {}, 'outcome': 'simulated', 'stats': {}} for step in resolved.effective['finish']['steps']],
            'tool_versions': {'preview': 'simulated-fixture-v1'}},
            'guard': {'hit_rate_before': 1, 'hit_rate_after': 1, 'p10_before': 1, 'p10_after': 1, 'components_survived': True}}


def arguments():
    project = project_fixture(); project['budgets'] = {'target_height_m': 1}
    resolved = resolve(project, 'rock')
    image, raw = png(), synthetic_glb(seed=123)
    request = {'kind': 'backend', 'asset_class': 'rock', 'backend_id': 'mock',
               'image_ref': {'path': 'references/input.png', 'sha256': digest(image)},
               'declared_source': 'own_work', 'seed': 123, 'request_id': 'generate:e4'}
    result = {'sha256': digest(raw), 'bytes': len(raw), 'backend_reported': {'seed': 123, 'steps_run': 0, 'wall_s': 0, 'peak_vram_mib': None}, 'logs_tail': ''}
    return {'request': request, 'resolved': resolved, 'card': deepcopy(MOCK_CAPABILITY), 'result': result,
            'image': image, 'raw_glb': raw, 'artifact': artifact(), 'preview': png(),
            'observations': observations(resolved), 'plan_digest': digest(b'plan'), 'definition_digest': digest(b'recipe'),
            'document_session': 'fixture', 'exporter_sha256': EXPORTER}


def members(): return build_evidence(**arguments())
