"""Build the geometry-nodes vegetation fixture headlessly.

Run inside Blender::

    blender -b --python tools/blender/make_geonodes_fixture.py -- --output tools/blender/fixtures/vegetation.blend

The saved object ``Vegetation`` carries one geometry-nodes modifier whose
group exposes ``Seed`` (int) and ``Density`` (float) inputs. The graph scatters
cone "plants" over the object's own ground grid, stores a ``UVMap`` corner
attribute on both the ground and the instances, assigns one material, and
realizes everything into a single mesh — the shape the restricted-GLB export
profile and ``asset_processor`` expect.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy


def _args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, help="destination .blend path")
    return parser.parse_args(argv)


def _reset_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)


def _socket(node: bpy.types.Node, name: str, *, outputs: bool = False) -> bpy.types.NodeSocket:
    collection = node.outputs if outputs else node.inputs
    for socket in collection:
        if socket.name == name and socket.enabled:
            return socket
    raise KeyError(f"{node.name} has no enabled {'output' if outputs else 'input'} socket {name!r}")


def _build_group(material: bpy.types.Material) -> bpy.types.NodeTree:
    group = bpy.data.node_groups.new("VegetationScatter", "GeometryNodeTree")
    interface = group.interface
    interface.new_socket(name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    seed = interface.new_socket(name="Seed", in_out="INPUT", socket_type="NodeSocketInt")
    seed.default_value = 1
    density = interface.new_socket(name="Density", in_out="INPUT", socket_type="NodeSocketFloat")
    density.default_value = 2.0
    density.min_value = 0.0
    interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")

    nodes = group.nodes
    links = group.links
    group_in = nodes.new("NodeGroupInput")
    group_out = nodes.new("NodeGroupOutput")

    distribute = nodes.new("GeometryNodeDistributePointsOnFaces")
    distribute.distribute_method = "RANDOM"
    links.new(_socket(group_in, "Geometry", outputs=True), _socket(distribute, "Mesh"))
    links.new(_socket(group_in, "Density", outputs=True), _socket(distribute, "Density"))
    links.new(_socket(group_in, "Seed", outputs=True), _socket(distribute, "Seed"))

    cone = nodes.new("GeometryNodeMeshCone")
    _socket(cone, "Vertices").default_value = 8
    _socket(cone, "Radius Bottom").default_value = 0.15
    _socket(cone, "Radius Top").default_value = 0.0
    _socket(cone, "Depth").default_value = 0.6

    cone_uv = nodes.new("GeometryNodeStoreNamedAttribute")
    cone_uv.data_type = "FLOAT2"
    cone_uv.domain = "CORNER"
    _socket(cone_uv, "Name").default_value = "UVMap"
    links.new(_socket(cone, "Mesh", outputs=True), _socket(cone_uv, "Geometry"))
    links.new(_socket(cone, "UV Map", outputs=True), _socket(cone_uv, "Value"))

    instance = nodes.new("GeometryNodeInstanceOnPoints")
    links.new(_socket(distribute, "Points", outputs=True), _socket(instance, "Points"))
    links.new(_socket(cone_uv, "Geometry", outputs=True), _socket(instance, "Instance"))
    links.new(_socket(distribute, "Rotation", outputs=True), _socket(instance, "Rotation"))

    realize = nodes.new("GeometryNodeRealizeInstances")
    links.new(_socket(instance, "Instances", outputs=True), _socket(realize, "Geometry"))

    join = nodes.new("GeometryNodeJoinGeometry")
    links.new(_socket(group_in, "Geometry", outputs=True), _socket(join, "Geometry"))
    links.new(_socket(realize, "Geometry", outputs=True), _socket(join, "Geometry"))

    set_material = nodes.new("GeometryNodeSetMaterial")
    _socket(set_material, "Material").default_value = material
    links.new(_socket(join, "Geometry", outputs=True), _socket(set_material, "Geometry"))
    links.new(_socket(set_material, "Geometry", outputs=True), _socket(group_out, "Geometry"))
    return group


def build(output: Path) -> None:
    _reset_scene()
    material = bpy.data.materials.new("Vegetation")

    bpy.ops.mesh.primitive_grid_add(x_subdivisions=8, y_subdivisions=8, size=4.0, calc_uvs=True)
    ground = bpy.context.active_object
    ground.name = "Vegetation"
    ground.data.name = "VegetationGround"
    ground.data.materials.append(material)

    modifier = ground.modifiers.new("VegetationScatter", "NODES")
    modifier.node_group = _build_group(material)

    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(output), compress=True)
    print(f"fixture saved: {output}")


if __name__ == "__main__":
    build(Path(_args().output).resolve())
