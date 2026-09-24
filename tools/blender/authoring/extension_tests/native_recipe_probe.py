"""Qualify reviewed recipes in an isolated interactive Blender editor."""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import sys
import traceback

import bpy


def main():
    parser = argparse.ArgumentParser()
    for name in ("archive", "output", "isolation-root"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"mode": "NATIVE_REVIEWED_ID_RECIPE", "blender": bpy.app.version_string,
              "background": bpy.app.background, "engine_rendered": False, "checks": [], "passed": False}

    def check(name, passed, **details):
        report["checks"].append({"name": name, "passed": bool(passed), **details})
        (args.output / "receipt.json").write_text(json.dumps(report, indent=2))
        if not passed:
            raise AssertionError(name + ": " + str(details))

    check("interactive editor", not bpy.app.background)
    resources = {kind: bpy.utils.user_resource(kind) for kind in ("CONFIG", "SCRIPTS", "DATAFILES")}
    resources["TEMP"] = bpy.app.tempdir
    check("isolated resource directories", all(Path(value).resolve().is_relative_to(
          args.isolation_root.resolve()) for value in resources.values()), paths=resources)
    bpy.context.preferences.use_preferences_save = False
    bpy.context.preferences.filepaths.use_auto_save_temporary_files = False
    bpy.context.preferences.view.show_splash = False
    initial_window = bpy.context.window_manager.windows[0]
    initial_window.event_simulate(type="ESC", value="PRESS")
    initial_window.event_simulate(type="ESC", value="RELEASE")
    yield 0.2
    repos = bpy.context.preferences.extensions.repos
    while repos:
        repos.remove(repos[0])
    repository = args.output / "repository"
    repository.mkdir()
    repo = repos.new(name="Reviewed recipe qualification", module="luminumbra_recipe_probe",
                     custom_directory=str(repository), remote_url="", source="USER")
    check("install actual archive", bpy.ops.extensions.package_install_files(
        "EXEC_DEFAULT", filepath=str(args.archive), repo=repo.module, enable_on_install=True) == {"FINISHED"})
    module_name = "bl_ext.luminumbra_recipe_probe.luminumbra_geometry_author"
    extension = importlib.import_module(module_name)
    recipes = extension.blender_recipes
    check("recipe operator and callback registered", hasattr(bpy.types, "LUMINUMBRA_OT_review_id_repairs")
          and recipes.before_apply == extension.recipe_will_apply)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    scene = bpy.context.scene
    collection = bpy.data.collections.new("RecipeProp")
    scene.collection.children.link(collection)
    bpy.context.view_layer.active_layer_collection = next(
        child for child in bpy.context.view_layer.layer_collection.children if child.collection == collection)
    bpy.ops.mesh.primitive_cube_add(size=1)
    obj = bpy.context.object
    obj.name = "RecipeOriginal"
    obj.data.name = "RecipeMesh"
    project = args.output / "project"
    project.mkdir()
    scene.lum_author_project = str(project)
    scene.lum_author_auto = False
    bpy.context.preferences.edit.use_global_undo = True
    check("manual marking bootstrap", bpy.ops.luminumbra.mark_geometry() == {"FINISHED"})
    original_id = obj["luminumbra.object_id"]
    duplicate = obj.copy()
    duplicate.name = "RecipeDuplicate"
    duplicate.location.x += 2
    collection.objects.link(duplicate)
    outside = obj.copy()
    outside.name = "OutsideSharedUser"
    outside.location.x -= 2
    scene.collection.objects.link(outside)
    del obj.data["luminumbra.asset_id"]
    bpy.context.view_layer.update()
    yield 0.1
    check("revision exists before any build session", extension._session is None and recipes.state.revision > 0)
    window = bpy.context.window_manager.windows[0]
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    ui_before = recipes.records(scene, collection)[0]
    previous_plans = set(recipes.state.plans)
    window.event_simulate(type="MOUSEMOVE", value="NOTHING", x=850, y=650)
    yield 0.2
    with bpy.context.temp_override(window=window, area=area, region=region):
        dialog = bpy.ops.luminumbra.review_id_repairs("INVOKE_DEFAULT")
    check("review dialog opens without editing", dialog == {"RUNNING_MODAL"}
          and recipes.records(scene, collection)[0] == ui_before)
    dialog_plans = set(recipes.state.plans) - previous_plans
    yield 0.3
    screenshot = args.output / "review-dialog.png"
    with bpy.context.temp_override(window=window):
        captured = bpy.ops.screen.screenshot(filepath=str(screenshot))
    check("actual review dialog screenshot", captured == {"FINISHED"} and screenshot.is_file(),
          sha256=hashlib.sha256(screenshot.read_bytes()).hexdigest())
    window.event_simulate(type="ESC", value="PRESS")
    window.event_simulate(type="ESC", value="RELEASE")
    yield 0.3
    check("cancel invalidates the dialog plan without editing", bool(dialog_plans)
          and not dialog_plans.intersection(recipes.state.plans)
          and recipes.records(scene, collection)[0] == ui_before)
    bpy.ops.ed.undo_push(message="Before reviewed identity repairs")
    before = recipes.records(scene, collection)[0]
    planned = recipes.plan(scene, collection)
    check("planning is nonmutating", before == recipes.records(scene, collection)[0])
    check("exact duplicate and shared mesh changes", len(planned["changes"]) == 2
          and any("Object:OutsideSharedUser" in row["shared_users"] for row in planned["changes"]), plan=planned)
    request = "native_apply"
    check("apply reviewed operator", bpy.ops.luminumbra.review_id_repairs(
        "EXEC_DEFAULT", True, plan_sha256=planned["plan_sha256"], request_id=request) == {"FINISHED"})
    assigned = (duplicate["luminumbra.object_id"], obj.data["luminumbra.asset_id"])
    check("original stable and duplicate repaired", obj["luminumbra.object_id"] == original_id
          and assigned[0] != original_id)
    receipt = recipes.state.receipts[request]
    receipt_path = recipes.receipt_directory(scene) / (request + ".json")
    check("durable receipt matches live result", json.loads(receipt_path.read_text()) == receipt)
    document_id, revision = recipes.state.document, recipes.state.revision
    window = bpy.context.window_manager.windows[0]
    area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
    region = next(region for region in area.regions if region.type == "WINDOW")
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.ed.undo()
    scene, collection = bpy.context.scene, bpy.data.collections["RecipeProp"]
    obj, duplicate = bpy.data.objects["RecipeOriginal"], bpy.data.objects["RecipeDuplicate"]
    check("one undo restores complete ID preimage", result == {"FINISHED"}
          and obj.get("luminumbra.object_id") == original_id
          and duplicate.get("luminumbra.object_id") == original_id
          and "luminumbra.asset_id" not in obj.data)
    check("undo advances revision and retains receipt", recipes.state.document == document_id
          and recipes.state.revision > revision and json.loads(receipt_path.read_text()) == receipt)
    # Duplicate delivery after undo must not reapply the changes.
    replay = recipes.run(scene, collection, planned["plan_sha256"], request)
    check("request replay does not reapply undone data", replay == receipt and "luminumbra.asset_id" not in obj.data)
    with bpy.context.temp_override(window=window, area=area, region=region):
        result = bpy.ops.ed.redo()
    scene, collection = bpy.context.scene, bpy.data.collections["RecipeProp"]
    obj, duplicate = bpy.data.objects["RecipeOriginal"], bpy.data.objects["RecipeDuplicate"]
    check("redo restores exact reviewed IDs", result == {"FINISHED"}
          and (duplicate.get("luminumbra.object_id"), obj.data.get("luminumbra.asset_id")) == assigned)
    del duplicate["luminumbra.object_id"]
    bpy.context.view_layer.update()
    yield 0.1
    stale = recipes.plan(scene, collection)
    obj.location.x += 1
    bpy.context.view_layer.update()
    yield 0.1
    refused = False
    try:
        recipes.run(scene, collection, stale["plan_sha256"], "native_stale")
    except ValueError:
        refused = True
    check("ordinary scene edit invalidates review without build session", refused
          and "luminumbra.object_id" not in duplicate)
    planned = recipes.plan(scene, collection)
    original_atomic = recipes.atomic_json

    def failure(_path, _value):
        raise OSError("injected receipt failure")

    recipes.atomic_json = failure
    refused = False
    try:
        recipes.run(scene, collection, planned["plan_sha256"], "native_failed")
    except ValueError:
        refused = True
    finally:
        recipes.atomic_json = original_atomic
    check("native receipt failure rolls back scene mutation", refused
          and "luminumbra.object_id" not in duplicate
          and recipes.state.receipts["native_failed"]["status"] == "failed")
    planned = recipes.plan(scene, collection)
    document = project / "recipe.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(document))
    old_document = recipes.state.document
    bpy.ops.wm.open_mainfile(filepath=str(document))
    yield 0.1
    check("file load invalidates document and pending plans", recipes.state.document != old_document
          and planned["plan_sha256"] not in recipes.state.plans)
    bpy.ops.preferences.addon_disable(module=module_name)
    check("extension unload removes recipe and timer", not hasattr(bpy.types, "LUMINUMBRA_OT_review_id_repairs")
          and not bpy.app.timers.is_registered(extension.timer) and recipes.before_apply is None)
    report["passed"] = all(row["passed"] for row in report["checks"])
    (args.output / "receipt.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    steps = main()

    def quit_probe():
        window = bpy.context.window_manager.windows[0]
        with bpy.context.temp_override(window=window):
            bpy.ops.wm.quit_blender()
        return None

    def advance():
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

    bpy.app.timers.register(advance, first_interval=1.0, persistent=True)
