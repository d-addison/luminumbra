"""Actual Blender5.1 evaluated foliage input probe; no renderer or visual approval."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

import bpy


ROOT = Path(__file__).resolve().parents[4]
AUTHORING = ROOT / "tools/blender/authoring"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def source_inputs():
    files = [Path(__file__).resolve(), AUTHORING / "probes/blender_fixture.py"]
    files += [AUTHORING / "extension" / name for name in ("blender_asset.py", "export_worker.py", "logic.py")]
    return {str(path): digest(path) for path in files}


def glb(path):
    data = path.read_bytes()
    magic, version, length = struct.unpack_from("<III", data)
    if (magic, version, length) != (0x46546C67, 2, len(data)):
        raise ValueError("Invalid actual GLB header")
    size, kind = struct.unpack_from("<II", data, 12)
    if kind != 0x4E4F534A:
        raise ValueError("Expected JSON GLB chunk")
    document = json.loads(data[20:20 + size])
    binary_size, binary_kind = struct.unpack_from("<II", data, 20 + size)
    if binary_kind != 0x004E4942 or 28 + size + binary_size != len(data):
        raise ValueError("Expected complete binary GLB chunk")
    return document, data[28 + size:]


def floats(document, binary, index, width):
    accessor = document["accessors"][index]
    if accessor["componentType"] != 5126 or accessor["type"] != "VEC" + str(width) or "sparse" in accessor:
        raise ValueError("Expected direct FLOAT vector accessor")
    view = document["bufferViews"][accessor["bufferView"]]
    offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", width * 4)
    return [struct.unpack_from("<" + "f" * width, binary, offset + i * stride) for i in range(accessor["count"])]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "scope": "actual evaluated Blender geometry/material/UV and GLB input contracts",
              "renderer_qualified": False, "compiled_asset_qualified": False, "visual_approval": False,
              "production_foliage_qualified": False, "checks": []}

    def save():
        (args.output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")

    def check(name, condition, **facts):
        report["checks"].append({"name": name, "passed": bool(condition), **facts})
        if not condition:
            raise RuntimeError(name)

    def refused(name, function, expected):
        try:
            function()
        except ValueError as error:
            check(name, expected in str(error), message=str(error))
        else:
            check(name, False)

    save()
    try:
        before = source_inputs()
        report.update(inputs_before=before, blender=bpy.app.version_string,
                      blender_build=bpy.app.build_hash.decode(), blender_executable=bpy.app.binary_path,
                      blender_sha256=digest(bpy.app.binary_path))
        check("exact Blender5.1 native profile", bpy.app.version == (5, 1, 0) and
              bpy.app.build_hash == b"adfe2921d5f3")
        import io_scene_gltf2
        exporter = Path(io_scene_gltf2.__file__).resolve().parent
        files = sorted([{"path": p.relative_to(exporter).as_posix(), "sha256": digest(p)}
                        for p in exporter.rglob("*.py")], key=lambda row: row["path"])
        exporter_hash = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":"),
                                                   ensure_ascii=False).encode()).hexdigest()
        report["exporter_sha256"] = exporter_hash
        check("exact exporter5.1.18 source", exporter_hash ==
              "5fe9f5ede7e5264b0a1045dc3784e243e645a90cb6073fc73ae56bc7a10de447")
        sys.path.insert(0, str(AUTHORING / "extension"))
        asset = load("blender_asset", AUTHORING / "extension/blender_asset.py")
        worker = load("evaluated_export_worker", AUTHORING / "extension/export_worker.py")
        fixture = load("evaluated_blender_fixture", AUTHORING / "probes/blender_fixture.py")
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        scene = bpy.context.scene
        scene.unit_settings.scale_length = 1
        scene.render.threads_mode, scene.render.threads = "FIXED", 2
        collection = bpy.data.collections.new("EvaluatedFoliage")
        collection["luminumbra.asset_id"] = "fixture.evaluated_foliage"
        scene.collection.children.link(collection)
        bpy.context.view_layer.active_layer_collection = next(
            layer for layer in bpy.context.view_layer.layer_collection.children if layer.collection == collection)
        fixture.plant(42)
        obj = next(obj for obj in collection.all_objects if obj.type == "MESH")
        original = obj.material_slots[0].material
        replacement = fixture.material("EvaluatedLeaf", (0.1, 0.4, 0.2), cutout=True)
        for node in replacement.node_tree.nodes:
            if node.type == "TEX_IMAGE":
                node.image.colorspace_settings.name = "sRGB"
        group = obj.modifiers[0].node_group
        output = next(node for node in group.nodes if node.type == "GROUP_OUTPUT")
        realize = next(node for node in group.nodes if node.type == "REALIZE_INSTANCES")
        assignment = group.nodes.new("GeometryNodeSetMaterial")
        assignment.inputs["Material"].default_value = replacement
        group.links.new(realize.outputs["Geometry"], assignment.inputs["Geometry"])
        group.links.new(assignment.outputs["Geometry"], output.inputs["Geometry"])
        bpy.context.view_layer.update()
        check("GN material is absent from authored slots", replacement not in
              [slot.material for slot in obj.material_slots] and original != replacement)
        check("evaluated material identity follows GN assignment", asset.evaluated_static_materials(collection) == {replacement})
        request = {"profile": "static", "build_profile": "glb-static-prefab-v1"}
        check("live worker helper accepts supported GN material", worker.validate_snapshot(scene, collection, request) ==
              ("static", "glb-static-prefab-v1"))
        mesh_count = len(bpy.data.meshes)
        shader = replacement.node_tree.nodes.get("Principled BSDF")
        shader.inputs["Coat Weight"].default_value = .25
        refused("unsupported GN shader refused by foreground", lambda: asset.validate_prefab_materials(collection), "Principled")
        refused("unsupported GN shader refused by live worker helper", lambda: worker.validate_snapshot(scene, collection, request), "Principled")
        shader.inputs["Coat Weight"].default_value = 0
        removal = group.nodes.new("GeometryNodeRemoveAttribute")
        removal.inputs["Name"].default_value = "UVMap"
        group.links.new(assignment.outputs["Geometry"], removal.inputs["Geometry"])
        group.links.new(removal.outputs["Geometry"], output.inputs["Geometry"])
        bpy.context.view_layer.update()
        check("source UV remains while evaluated UV is removed", bool(obj.data.uv_layers))
        refused("evaluated missing UV refused by foreground", lambda: asset.validate_collection(scene, collection), "evaluated geometry")
        refused("evaluated missing UV refused by live worker helper", lambda: worker.validate_snapshot(scene, collection, request), "evaluated geometry")
        group.links.new(assignment.outputs["Geometry"], output.inputs["Geometry"])
        bpy.context.view_layer.update()
        check("valid output recovers after refused edits", worker.validate_snapshot(scene, collection, request) ==
              ("static", "glb-static-prefab-v1"))
        check("temporary evaluations do not create persistent meshes", len(bpy.data.meshes) == mesh_count)
        snapshot = args.output / "collection-snapshot.blend"
        bpy.data.libraries.write(str(snapshot), {collection}, path_remap="ABSOLUTE", fake_user=True, compress=True)
        with bpy.data.libraries.load(str(snapshot), link=False) as (available, loaded):
            if collection.name not in available.collections:
                raise RuntimeError("Saved collection missing from actual library snapshot")
            loaded.collections = [collection.name]
        appended = loaded.collections[0]
        scene.collection.children.link(appended)
        bpy.context.view_layer.update()
        check("saved and loaded GN snapshot retains supported material", worker.validate_snapshot(scene, appended, request) ==
              ("static", "glb-static-prefab-v1"))
        appended_material = next(iter(asset.evaluated_static_materials(appended)))
        appended_shader = appended_material.node_tree.nodes.get("Principled BSDF")
        appended_shader.inputs["Coat Weight"].default_value = .25
        refused("unsupported material in loaded snapshot is refused", lambda: worker.validate_snapshot(scene, appended, request), "Principled")
        appended_shader.inputs["Coat Weight"].default_value = 0
        bpy.data.collections.remove(appended)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.output / "fixture.blend"))
        settings = {"export_format": "GLB", "export_yup": True, "export_extras": True,
                    "export_normals": True, "export_texcoords": True, "export_gn_mesh": False,
                    "export_apply": True, "export_animations": False, "export_morph": False,
                    "export_skins": False, "use_active_collection": True, "use_active_collection_with_nested": True}
        exported = []
        for name in ("first", "repeat"):
            path = args.output / (name + ".glb")
            check(name + " actual GLB export finished", bpy.ops.export_scene.gltf(filepath=str(path), **settings) == {"FINISHED"})
            exported.append(path)
        check("repeated seeded GLB bytes agree", digest(exported[0]) == digest(exported[1]))
        document, binary = glb(exported[0])
        primitives = [primitive for mesh in document["meshes"] for primitive in mesh["primitives"]]
        materials = [document["materials"][primitive["material"]] for primitive in primitives]
        check("GLB binds evaluated material and persistent ID", all(row["name"] == replacement.name and
              row["extras"]["luminumbra.material_id"] == replacement["luminumbra.material_id"] for row in materials))
        check("GLB retains declared alpha cutoff", all(row["alphaMode"] == "MASK" and row.get("alphaCutoff", .5) == .5 for row in materials))
        check("exact sixteen leaf triangles retained in GLB", sum(document["accessors"][p["indices"]]["count"]
              for p in primitives) == 8 * 2 * 3)
        uv = [coordinate for p in primitives for coordinate in floats(document, binary, p["attributes"]["TEXCOORD_0"], 2)]
        check("GLB evaluated UV domain is retained", set(uv) == {(0., 0.), (1., 0.), (1., 1.), (0., 1.)})
        image = document["images"][0]
        view = document["bufferViews"][image["bufferView"]]
        image_path = args.output / "exported-cutout.png"
        image_path.write_bytes(binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]])
        decoded = bpy.data.images.load(str(image_path), check_existing=False)
        alpha = list(decoded.pixels)[3::4]
        check("exported cutout image preserves14of16 covered texels", tuple(decoded.size) == (4, 4) and
              sum(value > .5 for value in alpha) == 14 and sum(value <= .5 for value in alpha) == 2)
        bpy.data.images.remove(decoded)
        instances = next(node for node in group.nodes if node.type == "INSTANCE_ON_POINTS")
        group.links.new(instances.outputs["Instances"], output.inputs["Geometry"])
        bpy.context.view_layer.update()
        refused("unrealized GN instances remain explicitly unsupported", lambda: asset.validate_collection(scene, collection), "Realize Geometry Nodes")
        group.links.new(assignment.outputs["Geometry"], output.inputs["Geometry"])
        bpy.context.view_layer.update()
        check("supported realized fixture remains recoverable", worker.validate_snapshot(scene, collection, request) ==
              ("static", "glb-static-prefab-v1"))
        after = source_inputs()
        report.update(inputs_after=after, glb_sha256=digest(exported[0]), fixture_sha256=digest(args.output / "fixture.blend"),
                      snapshot_sha256=digest(snapshot), cutout_png_sha256=digest(image_path), settings=settings)
        check("source inputs unchanged", before == after)
        if len(report["checks"]) != 25:
            raise RuntimeError("Expected all 25 native evaluated-input checks")
        report["passed"] = all(row["passed"] for row in report["checks"])
    finally:
        save()


if __name__ == "__main__":
    main()
