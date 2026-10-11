#!/usr/bin/env python3
"""Generate explicit synthetic game content for client-process renderer tests.

Only the isolated test root receives this manifest and geometry. This is not an
authored art pack or performance qualification input and never uses acquisition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


PACK_DIRECTORY = 'game-assets/tree-small-02/1.0.0'


def triangle_mesh():
    # Mesh.cpp's LMSH layout: 28-byte header, three 32-byte position/normal/UV
    # vertices and three uint32 indices. A nondegenerate triangle with +Z normal.
    header = struct.pack('<4sII4f', b'LMSH', 3, 3, 0, 0.5, 0, 1)
    vertices = ((-0.5, 0, 0, 0, 0, 1, 0, 0),
                (0.5, 0, 0, 0, 0, 1, 1, 0),
                (0, 1, 0, 0, 0, 1, 0.5, 1))
    return header + b''.join(struct.pack('<8f', *v) for v in vertices) + struct.pack('<3I', 0, 1, 2)


def solid_texture(rgba):
    # load_static_model_texture_set requires exactly 512x512 RGBA and ten mips;
    # a 1x1 stub would parse as LTEX but be refused by the actual material loader.
    header = struct.pack('<4sHHIIB', b'LTEX', 1, 10, 512, 512, 4)
    return header + b''.join(bytes(rgba) * ((512 >> mip) ** 2) for mip in range(10))


def generate(root):
    directory = root / PACK_DIRECTORY
    files = []

    def write(relative, data):
        path = directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        files.append({'path': relative, 'size': len(data),
                      'sha256': hashlib.sha256(data).hexdigest()})

    mesh = triangle_mesh()
    for part in ('trunk', 'branches', 'leaves'):
        for lod in ('', '.lod1', '.lod2'):
            write(f'data/models/trees/tree_small_02_{part}{lod}.lmesh', mesh)
    # Opaque neutral albedo, tangent-space +Z normal, AO=1/roughness=1/metal=0.
    for kind, rgba in (('albedo', (120, 160, 80, 255)),
                       ('normal', (128, 128, 255, 255)),
                       ('arm', (255, 255, 0, 255))):
        texture = solid_texture(rgba)
        for part in ('trunk', 'branch', 'leaves'):
            write(f'data/textures/models/tree_{part}_{kind}_512.ltex', texture)

    manifest = {'schema': 'luminumbra.game.asset-packs.v1',
                '_comment': 'Synthetic renderer test inputs; not authored game art.',
                'packs': {'tree-small-02-runtime': {
                    'version': '1.0.0', 'install_dir': PACK_DIRECTORY,
                    'files': sorted(files, key=lambda item: item['path'])}}}
    config = root / 'config'
    config.mkdir(parents=True, exist_ok=True)
    (config / 'game-asset-packs.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'Synthetic render content: {len(files)} files, {sum(item["size"] for item in files)} bytes; no download')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    generate(parser.parse_args().root)
