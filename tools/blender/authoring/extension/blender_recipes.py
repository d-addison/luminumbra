"""Main-thread adapter for the reviewed asset ID recipe."""
from pathlib import Path
import uuid

import bpy

from .blender_asset import validate_collection
from .logic import atomic_json
from .recipes import DEFINITION, RecipeError, RecipeSession


state = RecipeSession()
before_apply = None


def reset_document():
    global state
    state = RecipeSession()


def records(scene, collection):
    if sum(len(items) for items in (bpy.data.collections, bpy.data.objects,
                                   bpy.data.meshes, bpy.data.materials)) > DEFINITION["max_records"]:
        raise RecipeError("recipe.limit", "Too many datablocks for the bounded ID recipe")
    validate_collection(scene, collection, require_ids=False)
    objects = set(collection.all_objects)
    meshes = {obj.data for obj in objects if obj.type == "MESH"}
    materials = {slot.material for obj in objects for slot in obj.material_slots if slot.material}
    users = bpy.data.user_map()
    result, blocks = [], {}
    for kind, universe, selected, key, prefix in (
        ("collection", bpy.data.collections, {collection}, "luminumbra.asset_id", "asset"),
        ("object", bpy.data.objects, objects, "luminumbra.object_id", "object"),
        ("mesh", bpy.data.meshes, meshes, "luminumbra.asset_id", "mesh"),
        ("material", bpy.data.materials, materials, "luminumbra.material_id", "material"),
    ):
        for block in universe:
            handle = f"{kind}:{block.session_uid}"
            identity = block.get(key)
            if identity is not None and not isinstance(identity, str):
                raise RecipeError("recipe.identity", f"Repair non-text identity on {block.name} manually first")
            shared = sorted({f"{user.bl_rna.identifier}:{user.name_full}"
                             for user in users.get(block, ()) if user not in objects and user != collection})
            row = {"handle": handle, "uid": block.session_uid, "kind": kind, "prefix": prefix,
                   "label": block.name_full, "key": key, "identity": identity,
                   "target": block in selected,
                   "editable": block.library is None and block.override_library is None,
                   "shared_users": shared}
            if kind == "object":
                row["parent"] = block.parent.session_uid if block.parent else None
                row["data"] = block.data.session_uid if block.data else None
                row["collections"] = sorted(item.session_uid for item in block.users_collection)
            if kind == "collection":
                row["objects"] = sorted(item.session_uid for item in block.all_objects)
            result.append(row)
            blocks[handle] = (block, key)
    return result, blocks


def receipt_directory(scene):
    root = Path(bpy.path.abspath(scene.lum_author_project))
    if not scene.lum_author_project or not root.is_dir():
        raise RecipeError("recipe.project", "Choose an existing project directory to retain recipe receipts")
    root = root.resolve(strict=True)
    directory = root / ".luminumbra-author-recipes"
    if directory.is_symlink():
        raise RecipeError("recipe.project", "Recipe receipt directory cannot be a symlink")
    directory.mkdir(exist_ok=True)
    directory = directory / state.document
    if directory.is_symlink():
        raise RecipeError("recipe.project", "Recipe session directory cannot be a symlink")
    directory.mkdir(exist_ok=True)
    return directory


def inspect(scene, collection):
    """Bounded local automation inspection; names are labels, handles are session-local."""
    return state.inspect(records(scene, collection)[0])


def plan(scene, collection):
    directory = receipt_directory(scene)
    value = state.plan(records(scene, collection)[0], f"collection:{collection.session_uid}", str(directory))
    atomic_json(directory / ("plan-" + value["plan_sha256"] + ".json"), value)
    return value


def run(scene, collection, plan_sha256, request_id):
    directory = receipt_directory(scene)
    current, blocks = records(scene, collection)

    def write(handle, identity):
        block, key = blocks[handle]
        if identity is None:
            if key in block:
                del block[key]
        else:
            block[key] = identity

    return state.apply(plan_sha256, request_id, current, write,
                       lambda value: atomic_json(directory / (request_id + ".json"), value),
                       scope=str(directory), before_apply=before_apply)


class LUMINUMBRA_OT_review_id_repairs(bpy.types.Operator):
    bl_idname = "luminumbra.review_id_repairs"
    bl_label = "Review ID Repairs"
    bl_description = "Review each planned identity repair before applying it as one undoable edit"
    bl_options = {"REGISTER", "UNDO"}

    plan_sha256: bpy.props.StringProperty(options={"HIDDEN", "SKIP_SAVE"})
    request_id: bpy.props.StringProperty(options={"HIDDEN", "SKIP_SAVE"})

    def invoke(self, context, _event):
        try:
            self._plan = plan(context.scene, context.scene.lum_author_collection)
            self.plan_sha256 = self._plan["plan_sha256"]
            self.request_id = uuid.uuid4().hex
            if not self._plan["changes"]:
                self.report({"INFO"}, "All asset identities are already distinct")
                return {"CANCELLED"}
            return context.window_manager.invoke_props_dialog(self, width=700, confirm_text="Apply Reviewed Repairs")
        except (OSError, ValueError, RuntimeError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}

    def draw(self, _context):
        layout = self.layout
        value = getattr(self, "_plan", None)
        if value is None:
            layout.label(text="Review a fresh plan before applying", icon="ERROR")
            return
        layout.label(text=f"{len(value['changes'])} ID repairs — one undoable edit")
        for change in value["changes"]:
            box = layout.box()
            box.label(text=f"{change['kind']}: {change['label']}")
            box.label(text="Before: " + (change["before"] or "(unassigned)"))
            box.label(text="After: " + change["after"])
            for user in change["shared_users"]:
                box.label(text="Also used by: " + user, icon="INFO")

    def cancel(self, _context):
        state.plans.pop(self.plan_sha256, None)

    def execute(self, context):
        try:
            result = run(context.scene, context.scene.lum_author_collection,
                         self.plan_sha256, self.request_id)
            if result["status"] != "applied":
                raise RecipeError("recipe.failed", result.get("message", "Request previously failed"))
            self.report({"INFO"}, f"Applied {len(result['changes'])} reviewed ID repairs")
            return {"FINISHED"}
        except (OSError, ValueError, RuntimeError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
