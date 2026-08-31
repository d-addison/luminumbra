"""Bake a geometry-nodes object to the restricted engine GLB profile.

Headless invocation::

    blender -b scene.blend --python-exit-code 1 --python tools/blender/geonodes_bake.py -- \
        --object ProceduralTree --output builds/tree.glb --seed 42 \
        --params-json '{"Density": 1.5, "Height": 8.0}'

``--params-json`` accepts an inline JSON object, a JSON file path, or ``@file``;
repeatable ``--param NAME=JSON_VALUE`` arguments override it.  The integer seed
must be supplied explicitly either as ``--seed`` or as the JSON ``seed`` input.
Every parameter is resolved against the named geometry-nodes modifiers by input
identifier or display name and is recorded in the result manifest.

Determinism requires the same ``.blend`` file, complete parameter set, explicit
frame, seed, and Blender version.  Under those conditions the evaluated geometry
is expected to be identical.  Changing Blender versions, node groups, external
dependencies, or implicit time-dependent state can change the result.

The script evaluates the complete modifier stack through Blender's dependency
graph, adds a final instance-realization boundary, materializes and triangulates
the mesh, applies its world transform, and exports only that baked object.  The
output is published only after ``tools/blender/validate_glb.py`` accepts it; that
validator is the output gate, not the success status of Blender's exporter.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import os
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, Mapping

import bpy


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if os.fspath(SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, os.fspath(SCRIPT_DIRECTORY))

from geonodes_bake_core import (  # noqa: E402
    BakeConfigurationError,
    BakeRequest,
    ParameterValue,
    arguments_after_separator,
    build_export_settings,
    build_result_manifest,
    manifest_json,
    parse_cli_args,
)


class BakeRuntimeError(RuntimeError):
    """Raised when Blender cannot produce a conforming baked mesh."""


def _interface_inputs(node_group: Any) -> list[Any]:
    interface = getattr(node_group, "interface", None)
    if interface is not None:
        return [
            item
            for item in interface.items_tree
            if getattr(item, "item_type", None) == "SOCKET"
            and getattr(item, "in_out", None) == "INPUT"
        ]
    return list(node_group.inputs)


def _socket_kind(socket: Any) -> str:
    return str(
        getattr(socket, "socket_type", None)
        or getattr(socket, "bl_socket_idname", None)
        or getattr(socket, "type", "")
    )


def _input_bindings(obj: Any) -> list[tuple[Any, Any]]:
    bindings: list[tuple[Any, Any]] = []
    for modifier in obj.modifiers:
        if modifier.type != "NODES" or modifier.node_group is None:
            continue
        for socket in _interface_inputs(modifier.node_group):
            if "Geometry" in _socket_kind(socket):
                continue
            identifier = getattr(socket, "identifier", "")
            if identifier:
                bindings.append((modifier, socket))
    return bindings


def _matches_parameter(parameter: str, bindings: list[tuple[Any, Any]]) -> list[tuple[Any, Any]]:
    modifier_name: str | None = None
    socket_name = parameter
    if "." in parameter:
        possible_modifier, possible_socket = parameter.split(".", 1)
        if any(modifier.name.casefold() == possible_modifier.casefold() for modifier, _ in bindings):
            modifier_name = possible_modifier
            socket_name = possible_socket

    candidates = [
        binding
        for binding in bindings
        if modifier_name is None or binding[0].name.casefold() == modifier_name.casefold()
    ]
    exact = [
        binding
        for binding in candidates
        if socket_name
        in (str(getattr(binding[1], "identifier", "")), str(getattr(binding[1], "name", "")))
    ]
    if exact:
        return exact
    folded = socket_name.casefold()
    return [
        binding
        for binding in candidates
        if folded
        in {
            str(getattr(binding[1], "identifier", "")).casefold(),
            str(getattr(binding[1], "name", "")).casefold(),
        }
    ]


def _lookup_data_block(collection: Any, name: str, parameter: str) -> Any:
    value = collection.get(name)
    if value is None:
        raise BakeConfigurationError(f"parameter {parameter!r} references unknown data-block {name!r}")
    return value


def _coerce_socket_value(parameter: str, value: ParameterValue, socket: Any) -> Any:
    kind = _socket_kind(socket)
    if "Bool" in kind:
        if not isinstance(value, bool):
            raise BakeConfigurationError(f"parameter {parameter!r} requires a boolean")
        return value
    if "Int" in kind:
        if not isinstance(value, int) or isinstance(value, bool):
            raise BakeConfigurationError(f"parameter {parameter!r} requires an integer")
        return value
    if "Float" in kind and "Vector" not in kind:
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise BakeConfigurationError(f"parameter {parameter!r} requires a number")
        return float(value)
    if "Vector" in kind:
        if not isinstance(value, list) or len(value) != 3:
            raise BakeConfigurationError(f"parameter {parameter!r} requires a three-number array")
        return tuple(float(item) for item in value)
    if "Color" in kind:
        if not isinstance(value, list) or len(value) not in (3, 4):
            raise BakeConfigurationError(f"parameter {parameter!r} requires a three- or four-number array")
        result = [float(item) for item in value]
        if len(result) == 3:
            result.append(1.0)
        return tuple(result)
    if "Material" in kind:
        if not isinstance(value, str):
            raise BakeConfigurationError(f"parameter {parameter!r} requires a material name")
        return _lookup_data_block(bpy.data.materials, value, parameter)
    if "Collection" in kind:
        if not isinstance(value, str):
            raise BakeConfigurationError(f"parameter {parameter!r} requires a collection name")
        return _lookup_data_block(bpy.data.collections, value, parameter)
    if "Object" in kind:
        if not isinstance(value, str):
            raise BakeConfigurationError(f"parameter {parameter!r} requires an object name")
        return _lookup_data_block(bpy.data.objects, value, parameter)
    if "Image" in kind:
        if not isinstance(value, str):
            raise BakeConfigurationError(f"parameter {parameter!r} requires an image name")
        return _lookup_data_block(bpy.data.images, value, parameter)
    if "Texture" in kind:
        if not isinstance(value, str):
            raise BakeConfigurationError(f"parameter {parameter!r} requires a texture name")
        return _lookup_data_block(bpy.data.textures, value, parameter)
    if "String" in kind:
        if not isinstance(value, str):
            raise BakeConfigurationError(f"parameter {parameter!r} requires a string")
        return value
    return value


def _apply_parameters(obj: Any, parameters: Mapping[str, ParameterValue]) -> None:
    bindings = _input_bindings(obj)
    if not bindings:
        raise BakeRuntimeError(f"object {obj.name!r} has no geometry-nodes modifier inputs")

    available = sorted(
        {
            f"{modifier.name}.{getattr(socket, 'name', getattr(socket, 'identifier', ''))}"
            for modifier, socket in bindings
        }
    )
    for parameter, value in parameters.items():
        matches = _matches_parameter(parameter, bindings)
        if not matches:
            choices = ", ".join(available)
            raise BakeConfigurationError(
                f"parameter {parameter!r} does not match a geometry-nodes input; available: {choices}"
            )
        for modifier, socket in matches:
            identifier = socket.identifier
            try:
                modifier[identifier] = _coerce_socket_value(parameter, value, socket)
            except (TypeError, ValueError) as error:
                raise BakeConfigurationError(
                    f"cannot assign parameter {parameter!r} to {modifier.name}.{socket.name}: {error}"
                ) from error
    obj.update_tag()


def _new_geometry_socket(node_group: Any, *, in_out: str) -> None:
    interface = getattr(node_group, "interface", None)
    if interface is not None:
        interface.new_socket(name="Geometry", in_out=in_out, socket_type="NodeSocketGeometry")
    elif in_out == "INPUT":
        node_group.inputs.new("NodeSocketGeometry", "Geometry")
    else:
        node_group.outputs.new("NodeSocketGeometry", "Geometry")


def _add_realize_instances_modifier(obj: Any) -> Any:
    node_group = bpy.data.node_groups.new("Geometry Bake Realize Instances", "GeometryNodeTree")
    _new_geometry_socket(node_group, in_out="INPUT")
    _new_geometry_socket(node_group, in_out="OUTPUT")
    input_node = node_group.nodes.new("NodeGroupInput")
    realize_node = node_group.nodes.new("GeometryNodeRealizeInstances")
    output_node = node_group.nodes.new("NodeGroupOutput")
    node_group.links.new(input_node.outputs["Geometry"], realize_node.inputs["Geometry"])
    node_group.links.new(realize_node.outputs["Geometry"], output_node.inputs["Geometry"])
    modifier = obj.modifiers.new(name="Realize Instances", type="NODES")
    modifier.node_group = node_group
    return node_group


def _replace_with_single_material(mesh: Any) -> Any:
    diffuse = (0.8, 0.8, 0.8, 1.0)
    for material in mesh.materials:
        if material is not None:
            diffuse = tuple(material.diffuse_color)
            break
    material = bpy.data.materials.new("Geometry Bake Material")
    material.use_nodes = False
    material.diffuse_color = diffuse
    material.metallic = 0.0
    material.roughness = 0.5
    mesh.materials.clear()
    mesh.materials.append(material)
    for polygon in mesh.polygons:
        polygon.material_index = 0
    return material


def _validate_baked_mesh(mesh: Any) -> None:
    if not mesh.vertices or not mesh.polygons:
        raise BakeRuntimeError("evaluated geometry produced an empty mesh")
    if any(len(polygon.vertices) != 3 for polygon in mesh.polygons):
        raise BakeRuntimeError("evaluated geometry was not fully triangulated")
    if not mesh.uv_layers or mesh.uv_layers.active is None:
        raise BakeRuntimeError("evaluated mesh has no UV map")
    for vertex in mesh.vertices:
        if any(not math.isfinite(component) for component in vertex.co):
            raise BakeRuntimeError("evaluated mesh contains a non-finite position")
    for loop in mesh.loops:
        if any(not math.isfinite(component) for component in loop.normal):
            raise BakeRuntimeError("evaluated mesh contains a non-finite normal")
    for loop in mesh.uv_layers.active.data:
        if any(not math.isfinite(component) for component in loop.uv):
            raise BakeRuntimeError("evaluated mesh contains a non-finite UV coordinate")


def _materialize_object(source: Any, parameters: Mapping[str, ParameterValue], frame: int) -> dict[str, Any]:
    if source.type != "MESH":
        raise BakeRuntimeError(f"object {source.name!r} must be a mesh")
    collection = bpy.data.collections.new("Geometry Bake Working Collection")
    bpy.context.scene.collection.children.link(collection)
    working = source.copy()
    working.data = source.data.copy()
    collection.objects.link(working)
    working.matrix_world = source.matrix_world.copy()

    resources: dict[str, Any] = {
        "collection": collection,
        "working": working,
        "working_mesh": working.data,
        "realize_group": None,
        "baked": None,
        "mesh": None,
        "material": None,
    }
    try:
        _apply_parameters(working, parameters)
        resources["realize_group"] = _add_realize_instances_modifier(working)
        working.modifiers.new(name="Triangulate", type="TRIANGULATE")

        bpy.context.scene.frame_set(frame)
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        depsgraph.update()
        evaluated = working.evaluated_get(depsgraph)
        mesh = bpy.data.meshes.new_from_object(
            evaluated,
            preserve_all_data_layers=True,
            depsgraph=depsgraph,
        )
        resources["mesh"] = mesh
        mesh.transform(evaluated.matrix_world)
        mesh.update()
        _validate_baked_mesh(mesh)
        resources["material"] = _replace_with_single_material(mesh)

        baked = bpy.data.objects.new(f"{source.name} Baked", mesh)
        resources["baked"] = baked
        collection.objects.link(baked)
        baked.matrix_world.identity()
        return resources
    except Exception:
        _cleanup_resources(resources)
        raise


def _cleanup_resources(resources: Mapping[str, Any]) -> None:
    baked = resources.get("baked")
    working = resources.get("working")
    if baked is not None and baked.name in bpy.data.objects:
        bpy.data.objects.remove(baked, do_unlink=True)
    if working is not None and working.name in bpy.data.objects:
        bpy.data.objects.remove(working, do_unlink=True)
    for key, collection in (
        ("mesh", bpy.data.meshes),
        ("working_mesh", bpy.data.meshes),
        ("material", bpy.data.materials),
        ("realize_group", bpy.data.node_groups),
    ):
        data_block = resources.get(key)
        if data_block is not None and data_block.name in collection and data_block.users == 0:
            collection.remove(data_block)
    working_collection = resources.get("collection")
    if working_collection is not None and working_collection.name in bpy.data.collections:
        bpy.data.collections.remove(working_collection)


def _select_only(obj: Any) -> None:
    for candidate in bpy.context.view_layer.objects:
        candidate.select_set(False)
    obj.hide_set(False)
    obj.hide_viewport = False
    obj.hide_render = False
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def _load_validator() -> Any:
    validator_path = SCRIPT_DIRECTORY / "validate_glb.py"
    spec = importlib.util.spec_from_file_location("_geonodes_bake_validator", validator_path)
    if spec is None or spec.loader is None:
        raise BakeRuntimeError(f"cannot load GLB validator from {validator_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _validate_export(path: Path, published_path: Path) -> dict[str, Any]:
    validator = _load_validator()
    report, exit_code = validator.report_for_path(path)
    report = dict(report)
    report["path"] = os.fspath(published_path)
    if exit_code != 0 or not report.get("valid"):
        findings = json.dumps(report.get("findings", []), sort_keys=True)
        raise BakeRuntimeError(f"restricted-GLB validation failed: {findings}")
    return report


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)



def _supported_export_settings(settings: dict[str, Any]) -> tuple[dict[str, Any], set[str]]:
    """Drop exporter keywords this Blender's glTF operator does not declare.

    The pinned profile names every setting the restricted export relies on;
    exporter releases add and retire keywords, and an unknown keyword makes the
    operator raise before exporting anything. Filtering against the operator's
    own property table keeps the pinned intent and records what was ignored.
    """

    try:
        supported = set(bpy.ops.export_scene.gltf.get_rna_type().properties.keys())
    except Exception:  # noqa: BLE001 - any introspection failure keeps the full set
        return dict(settings), set()
    kept = {key: value for key, value in settings.items() if key in supported}
    return kept, set(settings) - set(kept)

def run_bake(request: BakeRequest) -> dict[str, Any]:
    """Evaluate, export, validate, and publish one geometry-nodes bake."""

    source = bpy.data.objects.get(request.object_name)
    if source is None:
        raise BakeRuntimeError(f"object {request.object_name!r} does not exist")

    output_path = request.output_path.expanduser().resolve()
    manifest_path = request.manifest_path.expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output_path.stem}.", suffix=".glb", dir=output_path.parent
    )
    os.close(descriptor)
    temporary_path = Path(temporary_name)
    temporary_path.unlink(missing_ok=True)

    resources: dict[str, Any] | None = None
    try:
        resources = _materialize_object(source, request.parameters, request.frame)
        baked = resources["baked"]
        mesh = resources["mesh"]
        _select_only(baked)
        export_settings, dropped_settings = _supported_export_settings(
            build_export_settings(temporary_path)
        )
        if dropped_settings:
            print(
                "geonodes_bake: exporter on this Blender ignores settings: "
                + ", ".join(sorted(dropped_settings))
            )
        result = bpy.ops.export_scene.gltf(**export_settings)
        if "FINISHED" not in result or not temporary_path.is_file():
            raise BakeRuntimeError(f"Blender GLB exporter did not finish successfully: {result}")

        validation = _validate_export(temporary_path, output_path)
        content_sha256 = _sha256(temporary_path)
        vertex_count = len(mesh.vertices)
        triangle_count = len(mesh.polygons)
        material_count = len(mesh.materials)
        os.replace(temporary_path, output_path)

        manifest = build_result_manifest(
            source_blend=os.fspath(Path(bpy.data.filepath).resolve()) if bpy.data.filepath else "",
            object_name=request.object_name,
            output_path=os.fspath(output_path),
            frame=request.frame,
            seed=request.seed,
            parameters=request.parameters,
            blender_version=bpy.app.version_string,
            vertex_count=vertex_count,
            triangle_count=triangle_count,
            material_count=material_count,
            content_sha256=content_sha256,
            validation=validation,
        )
        _atomic_write(manifest_path, manifest_json(manifest))
        return manifest
    finally:
        temporary_path.unlink(missing_ok=True)
        if resources is not None:
            _cleanup_resources(resources)


def main(argv: Iterable[str] | None = None) -> int:
    script_args = arguments_after_separator(sys.argv) if argv is None else list(argv)
    request = parse_cli_args(script_args)
    manifest = run_bake(request)
    sys.stdout.write(manifest_json(manifest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
