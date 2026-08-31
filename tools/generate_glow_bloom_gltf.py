#!/usr/bin/env python3
""": generates the glow-bloom light-stimulus prop glTF.

Game-content generator (Project Capture). A small crystal bloom: three
stretched octahedral shards leaning out of a common base. Processed by
tools/asset_processor into a v1.lmesh (the renderer paints it with the
emissive LUT material declared in the archetype data).

Usage: python tools/generate_glow_bloom_gltf.py
       <build>/bin/asset_processor.exe \
           data/models/props/glow_bloom/glow_bloom.gltf \
           data/models/props/glow_bloom/glow_bloom.lmesh
"""

import base64
import json
import math
import os
import struct

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "models",
                   "props", "glow_bloom", "glow_bloom.gltf")

positions, normals, uvs, indices = [], [], [], []


def add_shard(base, height, radius, lean):
    """Octahedral shard: square waist at base+height*0.35, apex up, tip down."""
    bx, by, bz = base
    lx, lz = lean
    waist_y = by + height * 0.35
    apex = (bx + lx * height, by + height, bz + lz * height)
    tip = (bx, by, bz)
    waist = [
        (bx + radius, waist_y, bz),
        (bx, waist_y, bz + radius),
        (bx - radius, waist_y, bz),
        (bx, waist_y, bz - radius),
    ]
    for i in range(4):
        a = waist[i]
        b = waist[(i + 1) % 4]
        for tri in ((a, b, apex), (b, a, tip)):
            v0, v1, v2 = tri
            ux, uy, uz = (v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2])
            wx, wy, wz = (v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2])
            nx, ny, nz = (uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx)
            length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            nx, ny, nz = nx / length, ny / length, nz / length
            start = len(positions) // 3
            for v in tri:
                positions.extend(v)
                normals.extend((nx, ny, nz))
            uvs.extend((0.0, 0.0, 1.0, 0.0, 0.5, 1.0))
            indices.extend((start, start + 1, start + 2))


add_shard((0.0, 0.0, 0.0), 1.5, 0.22, (0.0, 0.0))
add_shard((0.25, 0.0, 0.1), 1.0, 0.15, (0.25, 0.1))
add_shard((-0.2, 0.0, -0.15), 0.8, 0.13, (-0.2, -0.2))

buffer = bytearray()
views = []


def append(fmt_char, values):
    while len(buffer) % 4:
        buffer.append(0)
    offset = len(buffer)
    packed = struct.pack("<%d%s" % (len(values), fmt_char), *values)
    buffer.extend(packed)
    views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(packed)})
    return len(views) - 1


v_pos = append("f", positions)
v_norm = append("f", normals)
v_uv = append("f", uvs)
v_idx = append("H", indices)

vertex_count = len(positions) // 3
pos_min = [min(positions[i::3]) for i in range(3)]
pos_max = [max(positions[i::3]) for i in range(3)]

gltf = {
    "asset": {"version": "2.0", "generator": "tools/generate_glow_bloom_gltf.py"},
    "buffers": [{
        "byteLength": len(buffer),
        "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode("ascii"),
    }],
    "bufferViews": views,
    "accessors": [
        {"bufferView": v_pos, "componentType": 5126, "count": vertex_count, "type": "VEC3",
         "min": pos_min, "max": pos_max},
        {"bufferView": v_norm, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
        {"bufferView": v_uv, "componentType": 5126, "count": vertex_count, "type": "VEC2"},
        {"bufferView": v_idx, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    ],
    "meshes": [{
        "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3,
        }]
    }],
    "nodes": [{"name": "glow_bloom", "mesh": 0}],
    "scenes": [{"nodes": [0]}],
    "scene": 0,
}

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", newline="\n") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (%d vertices, %d indices)" % (OUT, vertex_count, len(indices)))
