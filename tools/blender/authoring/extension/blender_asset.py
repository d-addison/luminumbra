"""Main-thread collection validation, persistent identities and library snapshots."""
from pathlib import Path
import re
import uuid

import bpy

IDENTIFIER = re.compile(r"[A-Za-z][A-Za-z0-9_.:-]{0,127}\Z")


def dependencies(collection):
    used = {}
    for data, users in bpy.data.user_map().items():
        for user in users:
            used.setdefault(user, set()).add(data)
    pending, result = [collection], set()
    while pending:
        data = pending.pop()
        if data not in result:
            result.add(data)
            pending.extend(used.get(data, ()))
    return result


def validate_collection(scene, collection, require_ids=True):
    if collection is None or collection == scene.collection:
        raise ValueError("Choose a regular asset collection in the Outliner")
    if scene.unit_settings.scale_length != 1:
        raise ValueError("This geometry profile requires scene unit scale 1 (meters)")
    objects = set(collection.all_objects)
    if not objects or not any(obj.type == "MESH" for obj in objects):
        raise ValueError("The asset collection needs mesh geometry")
    referenced = dependencies(collection)
    for data in referenced:
        if isinstance(data, (bpy.types.Sound, bpy.types.Speaker, bpy.types.Text, bpy.types.Scene,
                             bpy.types.MovieClip)):
            raise ValueError("Geometry snapshots cannot reference audio, scripts, movies or entire scenes")
        if data.library or data.override_library:
            raise ValueError("Linked libraries and overrides need a separately qualified export profile")
        animation = getattr(data, "animation_data", None)
        if animation and animation.drivers:
            raise ValueError("Bake drivers before building this geometry profile")
        if isinstance(data, bpy.types.Object) and data not in objects:
            raise ValueError("Include referenced parent, constraint and modifier objects in the asset collection")
    rigs = {obj for obj in objects if obj.type == "ARMATURE"}
    profile = "character" if rigs else "static"
    if len(rigs) > 1:
        raise ValueError("Export each character rig as a separate geometry asset")
    identities = set()
    for obj in objects:
        if obj.type not in ("MESH", "ARMATURE", "EMPTY") or obj.instance_type != "NONE":
            raise ValueError("This profile supports mesh, armature and empty objects; realize collection instances first")
        if obj.parent and obj.parent not in objects:
            raise ValueError("Include every parent inside the asset collection")
        if require_ids:
            identity = obj.get("luminumbra.object_id", "")
            if not isinstance(identity, str) or not IDENTIFIER.fullmatch(identity) or identity in identities:
                raise ValueError("Use Mark / Refresh Asset IDs to assign distinct persistent object IDs")
            identities.add(identity)
        if obj.type == "MESH":
            if obj.data.shape_keys and len(obj.data.shape_keys.key_blocks) > 1:
                raise ValueError("Morph assets are not supported by this build profile yet")
            if not obj.data.uv_layers:
                raise ValueError("Add a UV map before building geometry")
            if rigs:
                if obj.find_armature() not in rigs:
                    raise ValueError("Export unskinned attachments as separate geometry assets")
                if any(mod.type != "ARMATURE" for mod in obj.modifiers):
                    raise ValueError("Apply non-armature modifiers before exporting a character")
    if require_ids:
        identity = collection.get("luminumbra.asset_id", "")
        if not isinstance(identity, str) or not IDENTIFIER.fullmatch(identity):
            raise ValueError("Mark this collection as a geometry asset first")
    # The non-experimental static profile requires GN instances to be realized.
    if not rigs and any(mod.type == "NODES" for obj in objects for mod in obj.modifiers):
        graph = bpy.context.evaluated_depsgraph_get()
        for instance in graph.object_instances:
            if instance.is_instance and instance.parent and instance.parent.original in objects:
                raise ValueError("Realize Geometry Nodes instances before building this profile")
    return profile, referenced


def assign_ids(collection):
    def assign(items, universe, key, prefix):
        items = set(items)
        owners = {}
        for item in sorted(universe, key=lambda data: data.session_uid):
            identity = item.get(key)
            if isinstance(identity, str):
                owners.setdefault(identity, item)
        # Session UIDs survive renames and undo; a freshly duplicated block has
        # a newer UID. Names must not decide which block keeps an existing ID.
        for item in sorted(items, key=lambda data: data.session_uid):
            identity = item.get(key)
            if (not isinstance(identity, str) or not IDENTIFIER.fullmatch(identity)
                    or owners.get(identity) != item):
                identity = prefix + "." + uuid.uuid4().hex
                item[key] = identity
            owners[identity] = item
    assign([collection], bpy.data.collections, "luminumbra.asset_id", "asset")
    objects = set(collection.all_objects)
    assign(objects, bpy.data.objects, "luminumbra.object_id", "object")
    meshes = {obj.data for obj in objects if obj.type == "MESH"}
    assign(meshes, bpy.data.meshes, "luminumbra.asset_id", "mesh")
    materials = {slot.material for obj in objects for slot in obj.material_slots if slot.material}
    assign(materials, bpy.data.materials, "luminumbra.material_id", "material")


def capture(scene, collection, project, revision, guard, guard_hash):
    profile, _ = validate_collection(scene, collection)
    root = Path(project).resolve(strict=True)
    state = root / ".luminumbra-author-source"
    if state.is_symlink():
        raise ValueError("Authoring snapshot directory cannot be a symlink")
    state.mkdir(exist_ok=True)
    directory = state / uuid.uuid4().hex
    directory.mkdir()
    snapshot = directory / "source.blend"
    # Writes the selected collection and only its indirect dependencies. It
    # neither saves the open document nor changes Blender's current file path.
    bpy.data.libraries.write(str(snapshot), {collection}, path_remap="ABSOLUTE", fake_user=True, compress=True)
    return {"snapshot": snapshot.relative_to(root).as_posix(), "collection": collection.name,
            "asset_id": collection["luminumbra.asset_id"], "revision": revision,
            "profile": profile, "guard": guard, "guard_sha256": guard_hash,
            "frame_start": scene.frame_start, "frame_end": scene.frame_end,
            "fps": scene.render.fps, "fps_base": scene.render.fps_base}
