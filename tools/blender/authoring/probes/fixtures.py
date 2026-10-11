"""Synthetic GLB probes based on the pinned engine's CPU fixture builder."""
from __future__ import annotations

import copy
from pathlib import Path
import struct

from common import canonical, digest, module
from contracts import ASSET, GRAPH, PROFILE

IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def asset(revision=1):
    return {"schema": ASSET, "asset_id": "fixture.prop", "revision": revision,
            "profile": PROFILE, "dependencies": [], "objects": [
                {"id": "fixture.root", "parent": None, "asset_id": "fixture.mesh",
                 "transform": IDENTITY[:], "components": []}]}


def graph():
    return {"schema": GRAPH, "id": "fixture.battery_heat", "definitions": [
        {"id": "fixture.add.v1", "digest": digest(b"add-definition-fixture"),
         "inputs": {"a": "presentation.float", "b": "presentation.float"},
         "outputs": {"sum": "presentation.float"}},
        {"id": "fixture.read.v1", "digest": digest(b"read-definition-fixture"),
         "inputs": {}, "outputs": {"heat": "presentation.float"}}],
        "nodes": [{"id": "sum", "definition": "fixture.add.v1"},
                  {"id": "battery", "definition": "fixture.read.v1"},
                  {"id": "ambient", "definition": "fixture.read.v1"}],
        "connections": [
            {"from": ["battery", "heat"], "to": ["sum", "a"]},
            {"from": ["ambient", "heat"], "to": ["sum", "b"]}],
        "layout": {"sum": [10, 20]}}


def builder():
    return module("baseline_fixtures", "tools/blender/fixtures/make_fixtures.py")


def base(skinned=False):
    document, binary = builder().make_document(skinned=skinned)
    position = document["accessors"][0]
    position.update(min=[0, 0, 0], max=[1, 1, 0])
    if skinned:
        # Correct tip inverse bind: translate -1 on Y for its +1 rest pose.
        raw = bytearray(binary)
        view = document["bufferViews"][document["accessors"][6]["bufferView"]]
        struct.pack_into("<f", raw, view["byteOffset"] + 64 + 13 * 4, -1)
        binary = bytes(raw)
    return document, binary


def animation(interpolation):
    document, binary = base(True)
    data = bytearray(binary)
    def append(values, kind, count, width):
        while len(data) % 4:
            data.append(0)
        offset = len(data)
        data.extend(struct.pack(f"<{len(values)}f", *values))
        document["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(values) * 4})
        document["accessors"].append({"bufferView": len(document["bufferViews"]) - 1,
                                      "componentType": 5126, "type": kind, "count": count})
        return len(document["accessors"]) - 1
    times = append([0, 1], "SCALAR", 2, 1)
    document["accessors"][times].update(min=[0], max=[1])
    values = [0, 0, 0, 0, 2, 0]
    if interpolation == "CUBICSPLINE":
        values = [0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 2, 0, 0, 2, 0, 0, 0, 0]
    output = append(values, "VEC3", len(values) // 3, 3)
    document["buffers"][0]["byteLength"] = len(data)
    document["animations"] = [{"name": "move", "samplers": [
        {"input": times, "output": output, "interpolation": interpolation}],
        "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]}]
    return document, bytes(data)


def cases():
    result = {"clean_static": (*base(), []), "clean_skin": (*base(True), [])}
    doc, data = base()
    doc["nodes"] = [
        {"name": "parent", "translation": [3, 1, -2], "children": [1, 2],
         "extras": {"luminumbra.object_id": "fixture.parent"}},
        {"name": "instance_a", "mesh": 0, "scale": [1, 2, 1],
         "extras": {"luminumbra.object_id": "fixture.a"}},
        {"name": "instance_b", "mesh": 0, "translation": [-1, 0, 2],
         "extras": {"luminumbra.object_id": "fixture.b"}}]
    doc["extras"] = {"luminumbra": {"schema": "luminumbra.unknown.required.v99", "required": True}}
    result["transformed_instances_unknown_schema"] = (doc, data, ["hierarchy/instances lost", "unknown metadata not validated"])
    doc, data = base(True)
    doc["nodes"][0]["translation"] = [2, 0, 0]
    doc["skins"][0]["joints"] = [1, 0]
    raw = bytearray(data)
    # Keep skin semantics valid after reordering palette: vertices still use root.
    joints_offset = doc["bufferViews"][4]["byteOffset"]
    for i in range(3):
        raw[joints_offset + i * 4] = 1
    bind_offset = doc["bufferViews"][6]["byteOffset"]
    struct.pack_into("<f", raw, bind_offset + 12 * 4, -2)
    struct.pack_into("<f", raw, bind_offset + 64 + 12 * 4, -2)
    first, second = raw[bind_offset:bind_offset + 64], raw[bind_offset + 64:bind_offset + 128]
    raw[bind_offset:bind_offset + 128] = second + first
    result["child_before_parent"] = (doc, bytes(raw), ["palette needs topological ordering"])
    doc, data = base(True)
    doc["nodes"][1].pop("translation")
    doc["nodes"][1]["matrix"] = IDENTITY[:]
    doc["nodes"][1]["matrix"][13] = 1
    result["matrix_joint_bind"] = (doc, data, ["matrix-form joint local bind not imported"])
    for interpolation in ("LINEAR", "STEP", "CUBICSPLINE"):
        result[interpolation.lower() + "_animation"] = (*animation(interpolation),
            [] if interpolation == "LINEAR" else [f"{interpolation} interpolation not faithfully imported"])
    doc, data = base(True)
    doc["skins"][0].pop("inverseBindMatrices")
    doc["nodes"] = [{"name": f"joint_{i}", "children": [i + 1] if i < 128 else []} for i in range(129)]
    doc["nodes"].append({"name": "mesh", "mesh": 0, "skin": 0})
    doc["skins"][0]["joints"] = list(range(129))
    doc["scenes"][0]["nodes"] = [0, 129]
    result["joint_budget_129"] = (doc, data, ["validator 128 differs from LMS2 256"])
    doc, data = base()
    doc["extensionsRequired"] = ["LUMINUMBRA_unknown_required"]
    doc["extensionsUsed"] = ["LUMINUMBRA_unknown_required"]
    result["unknown_required_extension"] = (doc, data, ["must refuse"])
    doc, data = base()
    doc["materials"] = [{"name": "LeafCutout", "alphaMode": "MASK", "alphaCutoff": 0.5,
                         "doubleSided": True, "pbrMetallicRoughness": {
                             "baseColorFactor": [0.12, 0.6, 0.08, 1], "metallicFactor": 0, "roughnessFactor": 0.8}}]
    doc["meshes"][0]["primitives"][0]["material"] = 0
    doc["nodes"] = [{"name": f"leaf_{i}", "mesh": 0, "translation": [i * 0.25, i * 0.5, 0],
                     "extras": {"luminumbra.object_id": f"fixture.leaf.{i}"}} for i in range(4)]
    doc["scenes"][0]["nodes"] = list(range(4))
    result["plant_instances"] = (doc, data, ["synthetic material/instance contract only; no cutout image or coverage measurement"])
    return result


def run(output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    baseline = module("baseline_validator", "tools/blender/validate_glb.py")
    records = []
    for name, (document, binary, caveats) in cases().items():
        first, second = output / f"{name}.glb", output / f"{name}.repeat.glb"
        builder().write_glb(first, document, binary)
        builder().write_glb(second, copy.deepcopy(document), bytes(binary))
        parsed_document, parsed_binary = baseline.parse_glb(first)
        repeated_document, repeated_binary = baseline.parse_glb(second)
        meaningful_binary = parsed_binary[:parsed_document["buffers"][0]["byteLength"]]
        repeated_binary = repeated_binary[:repeated_document["buffers"][0]["byteLength"]]
        validator = baseline.Validator(first, parsed_document, parsed_binary)
        profile = validator.validate()
        records.append({"fixture": name, "origin": "CPU synthetic; not Blender export",
                        "baseline_profile": profile, "findings": validator.findings,
                        "bytes_sha256": digest(first.read_bytes()),
                        "exact_repeat": first.read_bytes() == second.read_bytes(),
                        "canonical_document_and_buffer_sha256": digest(canonical(parsed_document) + meaningful_binary),
                        "canonical_repeat": canonical(parsed_document) == canonical(repeated_document)
                                            and meaningful_binary == repeated_binary,
                        "caveats": caveats})
    return records
