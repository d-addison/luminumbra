"""QUEUED: run only in an isolated, coordinated Blender 5.1.0 process."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import sys

import bpy


def tag(obj, identity):
    obj["luminumbra.object_id"] = identity
    obj["luminumbra.asset_id"] = "fixture." + obj.data.name if obj.data else "fixture.collection"


def material(name, color, cutout=False):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    shader = mat.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1)
    shader.inputs["Metallic"].default_value = 0.1
    shader.inputs["Roughness"].default_value = 0.6
    # Asymmetric color/alpha chart, packed in the GLB by the exporter.
    image = bpy.data.images.new(name + "Chart", width=4, height=4, alpha=True)
    pixels = []
    for y in range(4):
        for x in range(4):
            alpha = 0 if cutout and ((x == 0 and y == 0) or (x == 3 and y == 3)) else 1
            pixels.extend((x / 3, y / 3, 0.2, alpha))
    image.pixels = pixels
    image.pack()
    texture = mat.node_tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    mat.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
    if cutout:
        threshold = mat.node_tree.nodes.new("ShaderNodeMath")
        threshold.operation = "GREATER_THAN"
        threshold.inputs[1].default_value = 0.5
        mat.node_tree.links.new(texture.outputs["Alpha"], threshold.inputs[0])
        mat.node_tree.links.new(threshold.outputs[0], shader.inputs["Alpha"])
    mat["luminumbra.material_id"] = "fixture.material." + name
    return mat


def prop():
    parent = bpy.data.objects.new("Prefab", None)
    bpy.context.collection.objects.link(parent)
    parent.location = (3, -2, 1)
    parent.rotation_euler.z = 0.35
    tag(parent, "fixture.prefab")
    bpy.ops.mesh.primitive_cube_add(size=1)
    obj = bpy.context.object
    obj.name = "UVProp"
    obj.scale = (1, 2, 0.5)
    obj.parent = parent
    obj.data.materials.append(material("Prop", (0.5, 0.3, 0.1)))
    obj.modifiers.new("Triangulate", "TRIANGULATE")
    tag(obj, "fixture.prop.a")
    other = obj.copy()
    bpy.context.collection.objects.link(other)
    other.location = (2, 0, 1)
    tag(other, "fixture.prop.b")
    return {"object_instances": 2, "shared_mesh": True, "asymmetric_transform": True}


def plant(seed):
    rng = random.Random(seed)
    mesh = bpy.data.meshes.new("LeafMesh")
    mesh.from_pydata([(0, 0, 0), (0.6, 0.1, 0), (0.5, 1, 0.07), (0, 0.8, 0.02)], [], [(0, 1, 2), (0, 2, 3)])
    uv = mesh.uv_layers.new(name="UVMap")
    coords = [(0, 0), (1, 0), (1, 1), (0, 1)]
    for loop in mesh.loops:
        uv.data[loop.index].uv = coords[loop.vertex_index]
    obj = bpy.data.objects.new("ProceduralLeaves", mesh)
    bpy.context.collection.objects.link(obj)
    mesh.materials.append(material("Leaf", (0.1, 0.5, 0.1), cutout=True))
    tag(obj, "fixture.plant")
    group = bpy.data.node_groups.new("FixtureLeafScatter", "GeometryNodeTree")
    group.interface.new_socket(name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    group.interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    source = group.nodes.new("NodeGroupInput")
    output = group.nodes.new("NodeGroupOutput")
    line = group.nodes.new("GeometryNodeMeshLine")
    line.inputs["Count"].default_value = 8
    line.inputs["Offset"].default_value = (0.07, 0, 0.2)
    instances = group.nodes.new("GeometryNodeInstanceOnPoints")
    instances.inputs["Rotation"].default_value = (0.1, 0.2, rng.uniform(0.2, 0.4))
    realize = group.nodes.new("GeometryNodeRealizeInstances")
    group.links.new(source.outputs["Geometry"], instances.inputs["Instance"])
    group.links.new(line.outputs["Mesh"], instances.inputs["Points"])
    group.links.new(instances.outputs["Instances"], realize.inputs["Geometry"])
    group.links.new(realize.outputs["Geometry"], output.inputs["Geometry"])
    modifier = obj.modifiers.new("FixtureScatter", "NODES")
    modifier.node_group = group
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    evaluated_mesh = evaluated.to_mesh()
    evidence = {"seed": seed, "expected_leaf_surfaces": 8, "evaluated_vertices": len(evaluated_mesh.vertices),
                "evaluated_polygons": len(evaluated_mesh.polygons),
                "material_slots": [m.name for m in evaluated_mesh.materials],
                "polygon_material_indices": sorted({p.material_index for p in evaluated_mesh.polygons})}
    evaluated.to_mesh_clear()
    return evidence


def character():
    armature = bpy.data.armatures.new("FixtureRig")
    rig = bpy.data.objects.new("CharacterRig", armature)
    bpy.context.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    rig.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    root = armature.edit_bones.new("root")
    root.head, root.tail = (0, 0, 0), (0, 0, 1)
    tip = armature.edit_bones.new("tip")
    tip.head, tip.tail, tip.parent = (0, 0, 1), (0.2, 0, 2), root
    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, 1))
    obj = bpy.context.object
    obj.name = "CharacterMesh"
    obj.scale = (0.2, 0.3, 1)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.parent = rig
    for name in ("root", "tip"):
        group = obj.vertex_groups.new(name=name)
        indices = [v.index for v in obj.data.vertices if (v.co.z <= 0) == (name == "root")]
        group.add(indices, 1, "REPLACE")
    modifier = obj.modifiers.new("Armature", "ARMATURE")
    modifier.object = rig
    obj.data.materials.append(material("Character", (0.4, 0.4, 0.6)))
    tag(obj, "fixture.character.mesh")
    tag(rig, "fixture.character.rig")
    bone = rig.pose.bones["tip"]
    bone.rotation_mode = "XYZ"
    for frame, angle in ((1, 0), (12, 0.5), (24, 0)):
        bone.rotation_euler.y = angle
        bone.keyframe_insert(data_path="rotation_euler", frame=frame)
    rig.animation_data.action.name = "FixtureWave"
    return {"bones": 2, "clip": "FixtureWave", "morph_retarget_ik_physics": "queued subsequent fixtures"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", choices=("prop", "plant", "character"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    if bpy.app.version != (5, 1, 0):
        raise RuntimeError("Unqualified Blender version; expected exact 5.1.0 profile")
    args.output.mkdir(parents=True, exist_ok=False)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1
    scene.frame_start, scene.frame_end, scene.render.fps = 1, 24, 24
    scene.render.threads_mode, scene.render.threads = "FIXED", 2
    scene.view_settings.view_transform = "Standard"
    facts = {"prop": prop, "plant": lambda: plant(args.seed), "character": character}[args.suite]()
    scene.frame_set(1)
    bpy.ops.wm.save_as_mainfile(filepath=str(args.output / "fixture.blend"))
    settings = {"export_format": "GLB", "export_yup": True, "export_extras": True,
                "export_normals": True, "export_texcoords": True, "export_gn_mesh": False,
                "export_apply": args.suite != "character", "export_animations": args.suite == "character",
                "export_frame_range": True, "export_force_sampling": True}
    files = []
    for name in ("first", "repeat"):
        path = args.output / (name + ".glb")
        bpy.ops.export_scene.gltf(filepath=str(path), **settings)
        files.append({"path": path.name, "bytes": path.stat().st_size,
                      "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    receipt = {"mode": "BLENDER_EXPORT_PROBE", "engine_executed": False, "suite": args.suite,
               "blender": bpy.app.version_string, "settings": settings, "facts": facts, "files": files,
               "exact_repeat": files[0]["sha256"] == files[1]["sha256"],
               "color_management": {"view": scene.view_settings.view_transform, "exposure": scene.view_settings.exposure},
               "qualification": "Export only; compiled/runtime/viewport fidelity still required"}
    (args.output / "export-receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
