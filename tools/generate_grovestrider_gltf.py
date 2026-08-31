#!/usr/bin/env python3
""": generates the Grovestrider rigged box-limb quadruped glTF.

Game-content generator (Project Capture). The committed glTF under
data/models/creatures/grovestrider/ is processed by tools/asset_processor
into grovestrider.lmesh (LMS2) + grovestrider.idle.lanim +
grovestrider.walk.lanim, all committed as game data. Re-run this script and
the asset processor only when changing the creature; the outputs are
deterministic.

Usage: python tools/generate_grovestrider_gltf.py
       <build>/bin/asset_processor.exe \
           data/models/creatures/grovestrider/grovestrider.gltf \
           data/models/creatures/grovestrider/grovestrider.lmesh
"""

import base64
import json
import math
import os
import struct

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "models",
                   "creatures", "grovestrider", "grovestrider.gltf")

# ---- Skeleton (model space, +Y up, creature faces +Z) ----
# joints: index, name, parent, local translation, world bind position
JOINTS = [
    ("body",   None,   (0.0,   0.85,  0.0)),
    ("leg_fl", "body", (0.32, -0.25,  0.42)),
    ("leg_fr", "body", (-0.32, -0.25,  0.42)),
    ("leg_bl", "body", (0.32, -0.25, -0.42)),
    ("leg_br", "body", (-0.32, -0.25, -0.42)),
    ("head",   "body", (0.0,   0.25,  0.65)),
]
NAME_TO_INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}


def world_bind(index):
    name, parent, local = JOINTS[index]
    if parent is None:
        return local
    px, py, pz = world_bind(NAME_TO_INDEX[parent])
    return (px + local[0], py + local[1], pz + local[2])


# ---- Mesh: one box per joint ----
def add_box(positions, normals, uvs, joints, weights, indices, joint, mins, maxs):
    faces = [
        ((1, 0, 0),  [(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)]),
        ((-1, 0, 0), [(0, 0, 1), (0, 1, 1), (0, 1, 0), (0, 0, 0)]),
        ((0, 1, 0),  [(0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)]),
        ((0, -1, 0), [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)]),
        ((0, 0, 1),  [(1, 0, 1), (1, 1, 1), (0, 1, 1), (0, 0, 1)]),
        ((0, 0, -1), [(0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)]),
    ]
    for normal, corners in faces:
        base = len(positions) // 3
        for ci, corner in enumerate(corners):
            for c in range(3):
                positions.append(maxs[c] if corner[c] else mins[c])
                normals.append(float(normal[c]))
            uvs.extend([1.0 if ci in (1, 2) else 0.0, 1.0 if ci >= 2 else 0.0])
            joints.extend([joint, 0, 0, 0])
            weights.extend([1.0, 0.0, 0.0, 0.0])
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


positions, normals, uvs, vjoints, vweights, indices = [], [], [], [], [], []
add_box(positions, normals, uvs, vjoints, vweights, indices,
        NAME_TO_INDEX["body"], (-0.35, 0.55, -0.55), (0.35, 1.15, 0.55))
for leg in ("leg_fl", "leg_fr", "leg_bl", "leg_br"):
    wx, wy, wz = world_bind(NAME_TO_INDEX[leg])
    add_box(positions, normals, uvs, vjoints, vweights, indices,
            NAME_TO_INDEX[leg], (wx - 0.09, 0.0, wz - 0.09), (wx + 0.09, wy, wz + 0.09))
add_box(positions, normals, uvs, vjoints, vweights, indices,
        NAME_TO_INDEX["head"], (-0.18, 0.95, 0.50), (0.18, 1.30, 0.95))

# ---- Inverse bind matrices (column-major translate(-world)) ----
inverse_bind = []
for i in range(len(JOINTS)):
    wx, wy, wz = world_bind(i)
    m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -wx, -wy, -wz, 1]
    inverse_bind.extend(float(v) for v in m)


def quat_x(deg):
    a = math.radians(deg) * 0.5
    return (math.sin(a), 0.0, 0.0, math.cos(a))


# ---- Animations ----
# idle (2.4 s): gentle head nod + body bob.
idle_times = [0.0, 0.6, 1.2, 1.8, 2.4]
idle_head = []
for d in (0.0, 9.0, 0.0, -9.0, 0.0):
    idle_head.extend(quat_x(d))
idle_body_t = []
for dy in (0.0, 0.02, 0.0, 0.02, 0.0):
    idle_body_t.extend((0.0, 0.85 + dy, 0.0))

# walk (0.8 s): diagonal leg pairs swing about X, head steady forward dip.
walk_times = [0.0, 0.2, 0.4, 0.6, 0.8]
swing_a, swing_b = [], []
for d in (25.0, 0.0, -25.0, 0.0, 25.0):
    swing_a.extend(quat_x(d))
for d in (-25.0, 0.0, 25.0, 0.0, -25.0):
    swing_b.extend(quat_x(d))
walk_head = []
for d in (4.0, 0.0, 4.0, 0.0, 4.0):
    walk_head.extend(quat_x(d))

# ---- Buffer assembly ----
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
v_joints = append("B", vjoints)
v_weights = append("f", vweights)
v_indices = append("H", indices)
v_ibm = append("f", inverse_bind)
v_idle_t = append("f", idle_times)
v_idle_head = append("f", idle_head)
v_idle_body = append("f", idle_body_t)
v_walk_t = append("f", walk_times)
v_swing_a = append("f", swing_a)
v_swing_b = append("f", swing_b)
v_walk_head = append("f", walk_head)

vertex_count = len(positions) // 3
pos_min = [min(positions[i::3]) for i in range(3)]
pos_max = [max(positions[i::3]) for i in range(3)]

accessors = [
    {"bufferView": v_pos, "componentType": 5126, "count": vertex_count, "type": "VEC3",
     "min": pos_min, "max": pos_max},
    {"bufferView": v_norm, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
    {"bufferView": v_uv, "componentType": 5126, "count": vertex_count, "type": "VEC2"},
    {"bufferView": v_joints, "componentType": 5121, "count": vertex_count, "type": "VEC4"},
    {"bufferView": v_weights, "componentType": 5126, "count": vertex_count, "type": "VEC4"},
    {"bufferView": v_indices, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    {"bufferView": v_ibm, "componentType": 5126, "count": len(JOINTS), "type": "MAT4"},
    {"bufferView": v_idle_t, "componentType": 5126, "count": len(idle_times), "type": "SCALAR",
     "min": [idle_times[0]], "max": [idle_times[-1]]},
    {"bufferView": v_idle_head, "componentType": 5126, "count": len(idle_times), "type": "VEC4"},
    {"bufferView": v_idle_body, "componentType": 5126, "count": len(idle_times), "type": "VEC3"},
    {"bufferView": v_walk_t, "componentType": 5126, "count": len(walk_times), "type": "SCALAR",
     "min": [walk_times[0]], "max": [walk_times[-1]]},
    {"bufferView": v_swing_a, "componentType": 5126, "count": len(walk_times), "type": "VEC4"},
    {"bufferView": v_swing_b, "componentType": 5126, "count": len(walk_times), "type": "VEC4"},
    {"bufferView": v_walk_head, "componentType": 5126, "count": len(walk_times), "type": "VEC4"},
]
A_IDLE_T, A_IDLE_HEAD, A_IDLE_BODY = 7, 8, 9
A_WALK_T, A_SWING_A, A_SWING_B, A_WALK_HEAD = 10, 11, 12, 13

nodes = []
for i, (name, parent, local) in enumerate(JOINTS):
    node = {"name": name, "translation": list(local)}
    children = [j for j, (_, p, _) in enumerate(JOINTS) if p == name]
    if children:
        node["children"] = children
    nodes.append(node)
nodes.append({"name": "grovestrider_mesh", "mesh": 0, "skin": 0})

gltf = {
    "asset": {"version": "2.0", "generator": "tools/generate_grovestrider_gltf.py"},
    "buffers": [{
        "byteLength": len(buffer),
        "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode("ascii"),
    }],
    "bufferViews": views,
    "accessors": accessors,
    "meshes": [{
        "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2,
                           "JOINTS_0": 3, "WEIGHTS_0": 4},
            "indices": 5,
        }]
    }],
    "skins": [{"joints": list(range(len(JOINTS))), "inverseBindMatrices": 6, "skeleton": 0}],
    "animations": [
        {
            "name": "idle",
            "samplers": [
                {"input": A_IDLE_T, "output": A_IDLE_HEAD, "interpolation": "LINEAR"},
                {"input": A_IDLE_T, "output": A_IDLE_BODY, "interpolation": "LINEAR"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": NAME_TO_INDEX["head"], "path": "rotation"}},
                {"sampler": 1, "target": {"node": NAME_TO_INDEX["body"], "path": "translation"}},
            ],
        },
        {
            "name": "walk",
            "samplers": [
                {"input": A_WALK_T, "output": A_SWING_A, "interpolation": "LINEAR"},
                {"input": A_WALK_T, "output": A_SWING_B, "interpolation": "LINEAR"},
                {"input": A_WALK_T, "output": A_WALK_HEAD, "interpolation": "LINEAR"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": NAME_TO_INDEX["leg_fl"], "path": "rotation"}},
                {"sampler": 1, "target": {"node": NAME_TO_INDEX["leg_fr"], "path": "rotation"}},
                {"sampler": 1, "target": {"node": NAME_TO_INDEX["leg_bl"], "path": "rotation"}},
                {"sampler": 0, "target": {"node": NAME_TO_INDEX["leg_br"], "path": "rotation"}},
                {"sampler": 2, "target": {"node": NAME_TO_INDEX["head"], "path": "rotation"}},
            ],
        },
    ],
    "nodes": nodes,
    "scenes": [{"nodes": [0, len(nodes) - 1]}],
    "scene": 0,
}

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", newline="\n") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (%d vertices, %d indices, %d joints)"
      % (OUT, vertex_count, len(indices), len(JOINTS)))
