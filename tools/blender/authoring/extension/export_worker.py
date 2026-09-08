"""Run only in a private background Blender process with automatic scripts disabled."""
import hashlib
import json
from pathlib import Path
import sys

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from logic import atomic_json, canonical, digest, read_json


def file_hash(path):
    checksum = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def main():
    request = read_json(Path(sys.argv[sys.argv.index("--") + 1]))
    if bpy.app.version != (5, 1, 0):
        raise RuntimeError("This adapter is qualified for Blender 5.1.0 only")
    project = Path(request["project"]).resolve(strict=True)
    snapshot = Path(request["snapshot"]).resolve(strict=True)
    if not snapshot.is_relative_to(project):
        raise RuntimeError("Snapshot is outside the project")
    resources = {"config": bpy.utils.user_resource("CONFIG"), "temp": bpy.app.tempdir}
    if not (Path(resources["config"]).resolve().is_relative_to(snapshot.parent / "user")
            and Path(resources["temp"]).resolve().is_relative_to(snapshot.parent / "temp")):
        raise RuntimeError("Background Blender resource paths are not isolated")
    bpy.context.preferences.use_preferences_save = False
    guard = (project / request["guard"]).resolve(strict=True)
    if not guard.is_relative_to(project) or file_hash(guard) != request["guard_sha256"]:
        raise RuntimeError("Scene revision changed before export")
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    with bpy.data.libraries.load(str(snapshot), link=False) as (available, loaded):
        if request["collection"] not in available.collections:
            raise RuntimeError("Marked collection is missing from the snapshot")
        if available.sounds or available.speakers or available.texts:
            raise RuntimeError("Geometry snapshots cannot contain sounds, speakers or scripts")
        loaded.collections = [request["collection"]]
    collection = loaded.collections[0]
    if collection.get("luminumbra.asset_id") != request["asset_id"]:
        raise RuntimeError("Snapshot asset identity changed")
    scene = bpy.context.scene
    scene.collection.children.link(collection)
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1
    scene.frame_start = request["frame_start"]
    scene.frame_end = request["frame_end"]
    scene.render.fps = request["fps"]
    scene.render.fps_base = request["fps_base"]
    scene.render.threads_mode = "FIXED"
    scene.render.threads = 2
    scene.frame_set(scene.frame_start)
    layer = next(child for child in bpy.context.view_layer.layer_collection.children
                 if child.collection == collection)
    bpy.context.view_layer.active_layer_collection = layer
    dependencies = {request["guard"]: request["guard_sha256"],
                    snapshot.relative_to(project).as_posix(): file_hash(snapshot)}
    for image in bpy.data.images:
        if image.source in ("VIEWER", "GENERATED") or image.packed_file:
            continue
        if image.source != "FILE":
            raise RuntimeError("Only static image textures are supported")
        path = Path(bpy.path.abspath(image.filepath)).resolve(strict=True)
        if not path.is_relative_to(project):
            raise RuntimeError("Unpacked image dependencies must stay inside the project")
        dependencies[path.relative_to(project).as_posix()] = file_hash(path)
    profile = request["profile"]
    if profile not in ("static", "character"):
        raise RuntimeError("Unknown export profile")
    output = snapshot.parent / "asset.glb"
    settings = {"export_format": "GLB", "export_yup": True, "export_extras": True,
                "export_normals": True, "export_texcoords": True, "export_gn_mesh": False,
                "export_apply": profile == "static", "export_animations": profile == "character",
                "export_frame_range": True, "export_force_sampling": True,
                "export_morph": False, "export_skins": profile == "character",
                "use_active_collection": True, "use_active_collection_with_nested": True}
    result = bpy.ops.export_scene.gltf(filepath=str(output), **settings)
    if result != {"FINISHED"} or not output.is_file():
        raise RuntimeError("glTF export did not produce a snapshot")
    for name, expected in dependencies.items():
        if file_hash(project / name) != expected:
            raise RuntimeError("Source dependency changed during export")
    import io_scene_gltf2
    exporter_root = Path(io_scene_gltf2.__file__).resolve().parent
    files = sorted(
        ({"path": path.relative_to(exporter_root).as_posix(), "sha256": file_hash(path)}
         for path in exporter_root.rglob("*.py")), key=lambda entry: entry["path"])
    exporter = {"id": "blender.gltf.5.1.18", "sha256": digest(canonical(files))}
    # The initial planning receipt used a different aggregate encoding. This
    # canonical record-array digest is pinned by the subsequent file-level audit.
    if exporter["sha256"] != "5fe9f5ede7e5264b0a1045dc3784e243e645a90cb6073fc73ae56bc7a10de447":
        raise RuntimeError("Exporter source differs from the qualified Blender profile")
    asset = {"schema": "luminumbra.authoring.asset.v1", "profile": "glb-geometry-v1",
             "asset_id": request["asset_id"], "revision": request["revision"],
             "source": output.relative_to(project).as_posix(), "dependencies": sorted(dependencies),
             "expected_dependency_hashes": dependencies, "exporter": exporter,
             "annotations": {"blender": bpy.app.version_string, "settings": settings,
                             "source_collection": collection.name}}
    atomic_json(snapshot.parent / "asset.json", asset)
    atomic_json(snapshot.parent / "export-receipt.json", {
        "blender": bpy.app.version_string, "blender_sha256": file_hash(Path(bpy.app.binary_path)),
        "exporter": exporter, "asset_id": request["asset_id"], "revision": request["revision"],
        "exporter_hash_algorithm": "SHA-256 of canonical JSON sorted {path,sha256} records",
        "resource_paths": resources,
        "glb_sha256": file_hash(output), "settings": settings, "engine_rendered": False})


if __name__ == "__main__":
    main()
