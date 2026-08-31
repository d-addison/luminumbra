#!/usr/bin/env python3
"""Generate deterministic GLB fixtures for validate_glb.py."""

from __future__ import annotations

import argparse
import copy
import json
import struct
from pathlib import Path
from typing import Any


JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
MAX_JOINTS = 128


class GlbBuilder:
    def __init__(self) -> None:
        self.binary = bytearray()
        self.views: list[dict[str, int]] = []
        self.accessors: list[dict[str, Any]] = []

    def accessor(
        self,
        format_char: str,
        values: list[float | int],
        accessor_type: str,
        component_type: int,
        *,
        count: int,
        normalized: bool = False,
    ) -> int:
        while len(self.binary) % 4:
            self.binary.append(0)
        offset = len(self.binary)
        packed = struct.pack(f"<{len(values)}{format_char}", *values)
        self.binary.extend(packed)
        self.views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(packed)})
        accessor: dict[str, Any] = {
            "bufferView": len(self.views) - 1,
            "componentType": component_type,
            "count": count,
            "type": accessor_type,
        }
        if normalized:
            accessor["normalized"] = True
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def make_document(*, skinned: bool, weights: list[float] | None = None, joints: list[int] | None = None) -> tuple[dict[str, Any], bytes]:
    builder = GlbBuilder()
    positions = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
    normals = [0.0, 0.0, 1.0] * 3
    texcoords = [0.0, 0.0, 1.0, 0.0, 0.0, 1.0]
    indices = [0, 1, 2]
    position_accessor = builder.accessor("f", positions, "VEC3", 5126, count=3)
    normal_accessor = builder.accessor("f", normals, "VEC3", 5126, count=3)
    texcoord_accessor = builder.accessor("f", texcoords, "VEC2", 5126, count=3)
    index_accessor = builder.accessor("H", indices, "SCALAR", 5123, count=3)
    attributes: dict[str, int] = {
        "POSITION": position_accessor,
        "NORMAL": normal_accessor,
        "TEXCOORD_0": texcoord_accessor,
    }
    nodes: list[dict[str, Any]] = [{"name": "static_mesh", "mesh": 0}]
    skins: list[dict[str, Any]] = []
    if skinned:
        joint_values = joints or [0, 0, 0, 0] * 3
        weight_values = weights or [1.0, 0.0, 0.0, 0.0] * 3
        joint_accessor = builder.accessor("B", joint_values, "VEC4", 5121, count=3)
        weight_accessor = builder.accessor("f", weight_values, "VEC4", 5126, count=3)
        inverse_bind = [
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0,
        ] * 2
        inverse_bind_accessor = builder.accessor("f", inverse_bind, "MAT4", 5126, count=2)
        attributes.update(JOINTS_0=joint_accessor, WEIGHTS_0=weight_accessor)
        nodes = [
            {"name": "root", "children": [1]},
            {"name": "tip", "translation": [0.0, 1.0, 0.0]},
            {"name": "skinned_mesh", "mesh": 0, "skin": 0},
        ]
        skins = [{"joints": [0, 1], "skeleton": 0, "inverseBindMatrices": inverse_bind_accessor}]

    document: dict[str, Any] = {
        "asset": {"version": "2.0", "generator": "tools/blender/fixtures/make_fixtures.py"},
        "buffers": [{"byteLength": len(builder.binary)}],
        "bufferViews": builder.views,
        "accessors": builder.accessors,
        "meshes": [{"name": "triangle", "primitives": [{"attributes": attributes, "indices": index_accessor}]}],
        "nodes": nodes,
        "scenes": [{"nodes": [0] if not skinned else [0, 2]}],
        "scene": 0,
    }
    if skins:
        document["skins"] = skins
    return document, bytes(builder.binary)


def write_glb(path: Path, document: dict[str, Any], binary: bytes) -> None:
    document = copy.deepcopy(document)
    document["buffers"][0]["byteLength"] = len(binary)
    json_bytes = json.dumps(document, sort_keys=True, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((-len(json_bytes)) % 4)
    binary += b"\0" * ((-len(binary)) % 4)
    total_length = 12 + 8 + len(json_bytes) + 8 + len(binary)
    data = bytearray(struct.pack("<4sII", b"glTF", 2, total_length))
    data.extend(struct.pack("<II", len(json_bytes), JSON_CHUNK))
    data.extend(json_bytes)
    data.extend(struct.pack("<II", len(binary), BIN_CHUNK))
    data.extend(binary)
    path.write_bytes(data)


def generate(output_dir: Path) -> dict[str, str]:
    output_dir.mkdir(parents=True, exist_ok=True)
    fixtures: dict[str, tuple[dict[str, Any], bytes]] = {}

    clean_static = make_document(skinned=False)
    clean_skinned = make_document(skinned=True)
    fixtures["clean_static.glb"] = clean_static
    fixtures["clean_skinned.glb"] = clean_skinned

    document, binary = make_document(skinned=False)
    del document["meshes"][0]["primitives"][0]["attributes"]["NORMAL"]
    fixtures["missing_normal.glb"] = (document, binary)

    document, binary = make_document(skinned=False)
    del document["meshes"][0]["primitives"][0]["indices"]
    fixtures["nonindexed.glb"] = (document, binary)

    document, binary = make_document(skinned=False)
    document["extensionsUsed"] = ["KHR_draco_mesh_compression"]
    document["meshes"][0]["primitives"][0]["extensions"] = {
        "KHR_draco_mesh_compression": {"bufferView": 0, "attributes": {"POSITION": 0}}
    }
    fixtures["unsupported_extension.glb"] = (document, binary)

    document, binary = make_document(skinned=True)
    document["meshes"].append(copy.deepcopy(document["meshes"][0]))
    static_primitive = document["meshes"][1]["primitives"][0]
    del static_primitive["attributes"]["JOINTS_0"]
    del static_primitive["attributes"]["WEIGHTS_0"]
    document["nodes"].append({"name": "static_part", "mesh": 1})
    document["scenes"][0]["nodes"].append(3)
    fixtures["mixed_geometry.glb"] = (document, binary)

    bad_weights = [0.5, 0.0, 0.0, 0.0] + [1.0, 0.0, 0.0, 0.0] * 2
    fixtures["bad_weights.glb"] = make_document(skinned=True, weights=bad_weights)

    bad_joints = [2, 0, 0, 0] + [0, 0, 0, 0] * 2
    fixtures["joint_out_of_range.glb"] = make_document(skinned=True, joints=bad_joints)

    document, binary = make_document(skinned=True)
    document["skins"].append(copy.deepcopy(document["skins"][0]))
    fixtures["multiple_skins.glb"] = (document, binary)

    document, binary = make_document(skinned=True)
    document["skins"][0].pop("inverseBindMatrices")
    document["nodes"] = [
        {"name": f"joint_{index}", "children": [index + 1] if index < MAX_JOINTS else []}
        for index in range(MAX_JOINTS + 1)
    ]
    document["nodes"].append({"name": "skinned_mesh", "mesh": 0, "skin": 0})
    document["skins"][0]["joints"] = list(range(MAX_JOINTS + 1))
    document["skins"][0]["skeleton"] = 0
    document["scenes"][0]["nodes"] = [0, MAX_JOINTS + 1]
    fixtures["too_many_joints.glb"] = (document, binary)

    for name, (document, binary) in fixtures.items():
        write_glb(output_dir / name, document, binary)
    return {name: str(output_dir / name) for name in sorted(fixtures)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "generated",
    )
    args = parser.parse_args()
    generated = generate(args.output_dir)
    print(json.dumps({"fixtures": generated}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
