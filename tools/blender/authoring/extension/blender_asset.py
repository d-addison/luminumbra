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


def validate_prefab_materials(collection):
    """Accept a declared Principled subset; arbitrary shader graphs require baking."""
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def image_input(socket, normal=False):
        require(socket.is_linked and len(socket.links) == 1, "Use a direct image texture connection")
        link = socket.links[0]
        node = link.from_node
        require(node.type == "TEX_IMAGE" and node.image and link.from_socket.name == "Color",
                "Bake this shader input to a named image texture")
        require(not node.inputs["Vector"].is_linked and node.projection == "FLAT",
                "This Blender prefab profile uses the active render UV map without vector nodes")
        require(node.interpolation in ("Linear", "Closest") and node.extension in ("REPEAT", "EXTEND", "MIRROR"),
                "Use Linear/Closest texture sampling with repeat, extend or mirror addressing")
        require(node.image.colorspace_settings.name == ("Non-Color" if normal else "sRGB"),
                "Use Non-Color for normal maps and sRGB for color textures")
        return node

    materials = {slot.material for obj in collection.all_objects for slot in obj.material_slots if slot.material}
    for material in materials:
        require(material.use_nodes, "Use a Principled material before building a prefab")
        outputs = [node for node in material.node_tree.nodes if node.type == "OUTPUT_MATERIAL" and node.is_active_output]
        require(len(outputs) == 1 and len(outputs[0].inputs["Surface"].links) == 1,
                "Connect one Principled shader to the active material output")
        output = outputs[0]
        require(not output.inputs["Volume"].is_linked and not output.inputs["Displacement"].is_linked,
                "Bake material volume/displacement into supported geometry first")
        shader = output.inputs["Surface"].links[0].from_node
        require(shader.type == "BSDF_PRINCIPLED", "Bake non-Principled shader constructions before building a prefab")
        linked = {socket.name for socket in shader.inputs if socket.is_linked}
        require(linked <= {"Base Color", "Normal", "Alpha", "Emission Color"},
                "This Blender profile supports constant metal/roughness and direct color/normal textures")
        for name in ("Subsurface Weight", "Transmission Weight", "Coat Weight", "Sheen Weight",
                     "Anisotropic IOR Level", "Thin Film Thickness"):
            socket = shader.inputs.get(name)
            require(socket is None or socket.default_value == 0,
                    "Bake unsupported Principled layers before prefab export: " + name)
        for name, expected in (("IOR", 1.5), ("Specular IOR Level", .5), ("Weight", 1)):
            socket = shader.inputs.get(name)
            require(socket is None or socket.default_value == expected, "Restore the supported Principled value for " + name)
        tint = shader.inputs.get("Specular Tint")
        require(tint is None or tuple(tint.default_value) == (1,1,1,1), "Bake tinted specular into the supported material profile")
        color = image_input(shader.inputs["Base Color"]) if "Base Color" in linked else None
        if "Emission Color" in linked:
            image_input(shader.inputs["Emission Color"])
        if "Normal" in linked:
            normal = shader.inputs["Normal"].links[0].from_node
            require(normal.type == "NORMAL_MAP" and normal.space == "TANGENT" and not normal.uv_map,
                    "Use a tangent-space Normal Map node on the active render UV map")
            require(not normal.inputs["Strength"].is_linked, "Use a constant normal-map strength")
            image_input(normal.inputs["Color"], normal=True)
        if "Alpha" in linked:
            link = shader.inputs["Alpha"].links[0]
            alpha = link.from_node
            if alpha.type == "MATH":
                require(alpha.operation == "GREATER_THAN" and not alpha.inputs[1].is_linked
                        and 0 <= alpha.inputs[1].default_value <= 1 and len(alpha.inputs[0].links) == 1,
                        "Use an image alpha connection or a single Greater Than cutout threshold")
                link = alpha.inputs[0].links[0]
                alpha = link.from_node
            require(color is not None and alpha == color and link.from_socket.name == "Alpha",
                    "Use the base color image's alpha channel for transparency")


def capture(scene, collection, project, revision, guard, guard_hash, *, prefab=False):
    profile, _ = validate_collection(scene, collection)
    if prefab:
        if profile != "static":
            raise ValueError("Use the geometry profile for character assets")
        validate_prefab_materials(collection)
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
            "build_profile": "glb-static-prefab-v1" if prefab else "glb-geometry-v1",
            "frame_start": scene.frame_start, "frame_end": scene.frame_end,
            "fps": scene.render.fps, "fps_base": scene.render.fps_base}
