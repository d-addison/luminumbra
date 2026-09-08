"""Exercise the packaged geometry adapter in an isolated Blender process."""
import argparse
import importlib
import json
from pathlib import Path
import sys
import time
import traceback

import bpy


def main():
    parser = argparse.ArgumentParser()
    for name in ("archive", "output", "python", "service", "toolchain", "isolation-root"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    args.output.mkdir(parents=True, exist_ok=False)
    checks = []
    report = {"blender": bpy.app.version_string, "background": bpy.app.background,
              "mode": "NATIVE_GEOMETRY_ADAPTER", "engine_rendered": False, "checks": checks}
    def check(name, passed, **details):
        checks.append({"name": name, "passed": bool(passed), **details})
        (args.output / "receipt.json").write_text(json.dumps(report, indent=2))
        if not passed:
            raise AssertionError(name + ": " + str(details))
    resources = {"config": bpy.utils.user_resource("CONFIG"), "temp": bpy.app.tempdir,
                 "scripts": bpy.utils.user_resource("SCRIPTS"),
                 "datafiles": bpy.utils.user_resource("DATAFILES")}
    check("native resource paths are isolated", all(Path(value).resolve().is_relative_to(
          args.isolation_root.resolve()) for value in resources.values()), paths=resources)
    bpy.context.preferences.use_preferences_save = False
    bpy.context.preferences.filepaths.use_auto_save_temporary_files = False
    repos = bpy.context.preferences.extensions.repos
    while repos:
        repos.remove(repos[0])
    repository = args.output / "repository"
    repository.mkdir()
    repo = repos.new(name="Geometry adapter qualification", module="luminumbra_geometry_probe",
                     custom_directory=str(repository), remote_url="", source="USER")
    result = bpy.ops.extensions.package_install_files("EXEC_DEFAULT", filepath=str(args.archive),
                                                      repo=repo.module, enable_on_install=True)
    check("installed extension archive", result == {"FINISHED"})
    module_name = "bl_ext.luminumbra_geometry_probe.luminumbra_geometry_author"
    extension = importlib.import_module(module_name)
    check("registered panel and timer", hasattr(bpy.types, "LUMINUMBRA_PT_geometry")
          and bpy.app.timers.is_registered(extension.timer))
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    scene = bpy.context.scene
    collection = bpy.data.collections.new("AuthoredProp")
    scene.collection.children.link(collection)
    layer = next(child for child in bpy.context.view_layer.layer_collection.children if child.collection == collection)
    bpy.context.view_layer.active_layer_collection = layer
    bpy.ops.mesh.primitive_cube_add(size=1)
    obj = bpy.context.object
    obj.name = "AdapterProp"
    obj.location = (3, 2, 1)
    obj.scale = (1, 2, 0.5)
    project = args.output / "project"
    project.mkdir()
    scene.lum_author_project = str(project)
    scene.lum_author_python = str(args.python)
    scene.lum_author_service = str(args.service)
    scene.lum_author_toolchain = str(args.toolchain)
    scene.lum_author_auto = False
    bpy.context.preferences.edit.use_global_undo = True
    bpy.ops.ed.undo_push(message="Before marking geometry")
    check("mark collection", bpy.ops.luminumbra.mark_geometry("EXEC_DEFAULT", True) == {"FINISHED"})
    first_id = obj["luminumbra.object_id"]
    report["undo_evaluated"] = not bpy.app.background
    if not bpy.app.background:
        window = bpy.context.window_manager.windows[0]
        area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
        region = next(region for region in area.regions if region.type == "WINDOW")
        with bpy.context.temp_override(window=window, area=area, region=region):
            undo_result = bpy.ops.ed.undo()
        scene = bpy.context.scene
        collection = bpy.data.collections["AuthoredProp"]
        obj = bpy.data.objects["AdapterProp"]
        check("marking is undoable", undo_result == {"FINISHED"} and "luminumbra.object_id" not in obj
              and "luminumbra.asset_id" not in collection)
        with bpy.context.temp_override(window=window, area=area, region=region):
            redo_result = bpy.ops.ed.redo()
        scene = bpy.context.scene
        collection = bpy.data.collections["AuthoredProp"]
        obj = bpy.data.objects["AdapterProp"]
        check("redo restores the same persistent identities", redo_result == {"FINISHED"}
              and obj.get("luminumbra.object_id") == first_id)
    duplicate = obj.copy()
    collection.objects.link(duplicate)
    duplicate.name = "A_RenamedDuplicate"
    duplicate.location.x += 2
    check("refresh duplicated identity", bpy.ops.luminumbra.mark_geometry() == {"FINISHED"})
    check("duplicates have distinct object IDs and shared asset data",
          obj["luminumbra.object_id"] != duplicate["luminumbra.object_id"] and obj.data == duplicate.data)
    check("original identity survives duplicate repair", obj["luminumbra.object_id"] == first_id)
    outside = obj.copy()
    scene.collection.objects.link(outside)
    bpy.ops.luminumbra.mark_geometry()
    check("an outside duplicate cannot take the original identity", obj["luminumbra.object_id"] == first_id)
    bpy.data.objects.remove(outside, do_unlink=True)
    document = project / "authored.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(document))
    def wait_build():
        deadline = time.monotonic() + 100
        while extension._session.state.busy and time.monotonic() < deadline:
            extension._session.poll()
            yield 0.03
        check("build terminal", not extension._session.state.busy, status=extension._session.state.status)
    check("build operator", bpy.ops.luminumbra.geometry(operation="build") == {"FINISHED"})
    first_process = extension._session.process
    yield from wait_build()
    check("actual native generation published", bool(extension._session.state.generation),
          status=extension._session.state.status)
    pointer = json.loads((project / ".luminumbra-author/current.json").read_text())
    generation = project / ".luminumbra-author/generations" / pointer["job_id"]
    manifest = json.loads((generation / "manifest.json").read_text())
    check("both transformed mesh instances compiled", manifest["outputs"]["asset.lmesh"]["triangles"] == 24,
          outputs=manifest["outputs"])
    check("capture kept the user's active document", bpy.data.filepath == str(document))
    original_guard = (project / extension._session.guard).read_bytes()
    updates = []
    def observe(scene_arg, graph):
        updates.append({"same_scene": scene_arg == scene,
                        "original_scene": scene_arg.original == scene,
                        "ids": [{"name": update.id.name, "same_object": update.id == obj,
                                 "original_object": update.id.original == obj} for update in graph.updates]})
    bpy.app.handlers.depsgraph_update_post.append(observe)
    obj.location.x += 1
    bpy.context.view_layer.update()
    bpy.app.handlers.depsgraph_update_post.remove(observe)
    check("editing advances revision guard", original_guard != (project / extension._session.guard).read_bytes(),
          updates=updates)
    check("build after edit", bpy.ops.luminumbra.geometry(operation="build") == {"FINISHED"})
    extension._session.cancel()
    yield from wait_build()
    check("cancel keeps previous generation", pointer == json.loads((project / ".luminumbra-author/current.json").read_text()))
    obj.shape_key_add(name="Basis")
    obj.shape_key_add(name="UnsupportedMorph")
    refused = False
    try:
        bpy.ops.luminumbra.geometry(operation="build")
    except RuntimeError as error:
        refused = "Morph" in str(error)
    check("morph refusal preserves published generation", refused
          and pointer == json.loads((project / ".luminumbra-author/current.json").read_text()))
    obj.shape_key_clear()
    speaker = bpy.data.speakers.new("SyntheticAudioRefusal")
    obj["test_audio_reference"] = speaker
    refused = False
    try:
        bpy.ops.luminumbra.geometry(operation="build")
    except RuntimeError as error:
        refused = "audio" in str(error)
    check("indirect audio dependency refused before capture", refused)
    del obj["test_audio_reference"]
    bpy.data.speakers.remove(speaker)
    scene.lum_author_auto = True
    obj.location.z += 0.5
    bpy.context.view_layer.update()
    deadline = time.monotonic() + 100
    while extension._session.state.generation == pointer["job_id"] and time.monotonic() < deadline:
        extension.timer()
        yield 0.03
    check("debounced edit publishes a new generation", extension._session.state.generation != pointer["job_id"],
          status=extension._session.state.status)
    refreshed = json.loads((project / ".luminumbra-author/current.json").read_text())
    refreshed_manifest = json.loads((project / ".luminumbra-author/generations" / refreshed["job_id"] / "manifest.json").read_text())
    check("new transforms change compiled geometry", refreshed_manifest["outputs"]["asset.lmesh"]["sha256"]
          != manifest["outputs"]["asset.lmesh"]["sha256"])
    check("prior generation remains intact after refresh", json.loads((generation / "manifest.json").read_text()) == manifest)
    scene.lum_author_auto = False
    original_guard = (project / extension._session.guard).read_bytes()
    bpy.data.objects.remove(duplicate, do_unlink=True)
    extension.timer()
    check("deleting an object advances the revision guard", original_guard
          != (project / extension._session.guard).read_bytes())
    bpy.ops.wm.open_mainfile(filepath=str(document))
    # Loading replaces editor context; resume on the next event-loop turn.
    yield 0.1
    check("file switch stops owned process", extension._session is None and first_process.poll() is not None)
    check("restart build", bpy.ops.luminumbra.geometry(operation="build") == {"FINISHED"})
    second_process = extension._session.process
    bpy.ops.preferences.addon_disable(module=module_name)
    check("unload stops worker and removes timer", second_process.poll() is not None
          and not bpy.app.timers.is_registered(extension.timer))
    check("unload removes properties", not hasattr(bpy.types.Scene, "lum_author_project"))
    # Reuse the authored neutral foliage/character fixtures through the actual
    # extension path, including its separate static/character export profiles.
    bpy.ops.preferences.addon_enable(module=module_name)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "probes"))
    import blender_fixture
    for kind in ("plant", "character"):
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        asset_collection = bpy.data.collections.new("Adapter_" + kind)
        scene = bpy.context.scene
        scene.collection.children.link(asset_collection)
        layer = next(child for child in bpy.context.view_layer.layer_collection.children if child.collection == asset_collection)
        bpy.context.view_layer.active_layer_collection = layer
        scene.frame_start, scene.frame_end, scene.render.fps = 1, 24, 24
        (blender_fixture.plant(42) if kind == "plant" else blender_fixture.character())
        scene.lum_author_project = str(project)
        scene.lum_author_python = str(args.python)
        scene.lum_author_service = str(args.service)
        scene.lum_author_toolchain = str(args.toolchain)
        scene.lum_author_auto = False
        check(kind + " mark", bpy.ops.luminumbra.mark_geometry() == {"FINISHED"})
        check(kind + " build", bpy.ops.luminumbra.geometry(operation="build") == {"FINISHED"})
        yield from wait_build()
        check(kind + " published", extension._session.state.published_revision == extension._session.state.revision,
              status=extension._session.state.status)
        current = json.loads((project / ".luminumbra-author/current.json").read_text())
        manifest = json.loads((project / ".luminumbra-author/generations" / current["job_id"] / "manifest.json").read_text())
        if kind == "character":
            check("character mesh and clip compiled", manifest["outputs"]["asset.lmesh"]["format"] == "LMS2"
                  and any(name.endswith(".lanim") for name in manifest["outputs"]), outputs=manifest["outputs"])
        else:
            check("eight realized foliage surfaces compiled", manifest["outputs"]["asset.lmesh"]["triangles"] == 16,
                  outputs=manifest["outputs"])
        bpy.ops.luminumbra.geometry(operation="stop")
    bpy.ops.preferences.addon_disable(module=module_name)
    report["passed"] = all(item["passed"] for item in checks)
    (args.output / "receipt.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    if bpy.app.background:
        for delay in main():
            time.sleep(delay)
    else:
        steps = main()
        def quit_probe():
            window = bpy.context.window_manager.windows[0]
            with bpy.context.temp_override(window=window):
                bpy.ops.wm.quit_blender()
            return None
        def interactive():
            try:
                return next(steps)
            except StopIteration:
                pass
            except Exception:
                traceback.print_exc()
                sys.stdout.flush()
                sys.stderr.flush()
            bpy.app.timers.register(quit_probe, first_interval=0.1)
            return None
        bpy.app.timers.register(interactive, first_interval=1.0, persistent=True)
