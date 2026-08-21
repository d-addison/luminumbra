#!/usr/bin/env python3
"""Validate a GLB against luminumbra's asset-processor input profile."""

from __future__ import annotations

import json
import math
import os
import struct
import sys
from pathlib import Path
from typing import Any, Iterable


GLB_MAGIC = b"glTF"
GLB_VERSION = 2
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
MAX_JOINTS_PER_SKELETON = 128
WEIGHT_SUM_TOLERANCE = 1.0e-4

COMPONENTS = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
TYPE_WIDTHS = {
    "SCALAR": 1,
    "VEC2": 2,
    "VEC3": 3,
    "VEC4": 4,
    "MAT2": 4,
    "MAT3": 9,
    "MAT4": 16,
}


class GlbError(ValueError):
    """Raised when a file cannot be decoded as a GLB."""


def empty_location() -> dict[str, Any]:
    return {
        "mesh": None,
        "primitive": None,
        "joint": None,
        "accessor": None,
        "vertex": None,
        "attribute": None,
        "extension": None,
    }


class Validator:
    def __init__(self, source: Path, document: dict[str, Any], binary: bytes):
        self.source = source
        self.document = document
        self.binary = binary
        self.findings: list[dict[str, Any]] = []
        self._decoded_accessors: dict[int, list[tuple[float | int, ...]]] = {}
        self._accessor_failures: set[int] = set()
        self.mesh_primitives: list[tuple[int, int, dict[str, Any]]] = []

    def add(
        self,
        rule_id: str,
        message: str,
        *,
        mesh: int | None = None,
        primitive: int | None = None,
        joint: int | None = None,
        accessor: int | None = None,
        vertex: int | None = None,
        attribute: str | None = None,
        extension: str | None = None,
    ) -> None:
        location = empty_location()
        location.update(
            mesh=mesh,
            primitive=primitive,
            joint=joint,
            accessor=accessor,
            vertex=vertex,
            attribute=attribute,
            extension=extension,
        )
        self.findings.append(
            {
                "rule_id": rule_id,
                "severity": "error",
                "message": message,
                "location": location,
            }
        )

    def validate(self) -> str:
        self._validate_extensions()
        self._validate_accessors()
        self._collect_primitives()

        has_skin_attributes = any(
            any(name.startswith(("JOINTS_", "WEIGHTS_")) for name in primitive.get("attributes", {}))
            for _, _, primitive in self.mesh_primitives
            if isinstance(primitive.get("attributes"), dict)
        )
        skins = self.document.get("skins", [])
        skinned = bool(skins) or has_skin_attributes

        for mesh_index, primitive_index, primitive in self.mesh_primitives:
            self._validate_primitive(mesh_index, primitive_index, primitive, skinned)

        if skinned:
            self._validate_skin_profile(skins)
        self._validate_animations(skinned, skins)
        return "skinned" if skinned else "static"

    def _validate_extensions(self) -> None:
        names: set[str] = set()
        for field in ("extensionsUsed", "extensionsRequired"):
            value = self.document.get(field, [])
            if isinstance(value, list):
                names.update(item for item in value if isinstance(item, str))

        def visit(value: Any) -> None:
            if isinstance(value, dict):
                extensions = value.get("extensions")
                if isinstance(extensions, dict):
                    names.update(str(name) for name in extensions)
                for child in value.values():
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)

        visit(self.document)
        for name in sorted(names):
            self.add(
                "extension.unsupported",
                f"extension {name!r} is not supported by the asset processor",
                extension=name,
            )

    def _validate_accessors(self) -> None:
        accessors = self.document.get("accessors", [])
        if not isinstance(accessors, list):
            self.add("accessor.invalid", "accessors must be an array")
            return
        for index, accessor in enumerate(accessors):
            if not isinstance(accessor, dict):
                self.add("accessor.invalid", "accessor must be an object", accessor=index)
                continue
            if "sparse" in accessor:
                self.add(
                    "accessor.sparse",
                    "sparse accessors are outside the supported profile",
                    accessor=index,
                )

    def _collect_primitives(self) -> None:
        meshes = self.document.get("meshes", [])
        if not isinstance(meshes, list) or not meshes:
            self.add("mesh.missing", "the GLB must contain at least one mesh")
            return
        for mesh_index, mesh in enumerate(meshes):
            if not isinstance(mesh, dict):
                self.add("mesh.invalid", "mesh must be an object", mesh=mesh_index)
                continue
            primitives = mesh.get("primitives", [])
            if not isinstance(primitives, list) or not primitives:
                self.add("primitive.missing", "mesh has no primitives", mesh=mesh_index)
                continue
            for primitive_index, primitive in enumerate(primitives):
                if not isinstance(primitive, dict):
                    self.add(
                        "primitive.invalid",
                        "primitive must be an object",
                        mesh=mesh_index,
                        primitive=primitive_index,
                    )
                    continue
                self.mesh_primitives.append((mesh_index, primitive_index, primitive))

    def _validate_primitive(
        self, mesh_index: int, primitive_index: int, primitive: dict[str, Any], skinned: bool
    ) -> None:
        context = {"mesh": mesh_index, "primitive": primitive_index}
        if primitive.get("mode", 4) != 4:
            self.add(
                "primitive.triangles",
                "only triangle-list primitives are supported",
                **context,
            )
        if "targets" in primitive:
            self.add(
                "primitive.morph_targets",
                "morph targets are not converted by the asset processor",
                **context,
            )

        attributes = primitive.get("attributes")
        if not isinstance(attributes, dict):
            self.add("primitive.attributes", "primitive attributes must be an object", **context)
            return

        position = self._required_attribute(attributes, "POSITION", mesh_index, primitive_index)
        normal = self._required_attribute(attributes, "NORMAL", mesh_index, primitive_index)
        texcoord_name = self._selected_texcoord(primitive, attributes)
        texcoord = None
        if texcoord_name is None:
            self.add(
                "attribute.texcoord",
                "primitive is missing a TEXCOORD attribute",
                attribute="TEXCOORD",
                **context,
            )
        else:
            texcoord = self._accessor_for_attribute(
                attributes, texcoord_name, mesh_index, primitive_index
            )

        indices = primitive.get("indices")
        index_accessor = None
        if not isinstance(indices, int):
            self.add("primitive.indexed", "primitive must have an index accessor", **context)
        else:
            index_accessor = self._accessor(indices, mesh_index, primitive_index, "INDICES")

        self._check_accessor_shape(position, "VEC3", {5126}, "POSITION", **context)
        self._check_accessor_shape(normal, "VEC3", {5126}, "NORMAL", **context)
        self._check_accessor_shape(texcoord, "VEC2", {5126}, texcoord_name, **context)
        self._check_accessor_shape(index_accessor, "SCALAR", {5121, 5123, 5125}, "INDICES", **context)

        position_count = self._count(position)
        for name, accessor in (("NORMAL", normal), (texcoord_name, texcoord)):
            if position_count is not None and accessor is not None and self._count(accessor) != position_count:
                self.add(
                    "attribute.count",
                    f"{name} count must match POSITION count",
                    accessor=accessor[0],
                    attribute=name,
                    **context,
                )

        self._check_finite(position, "POSITION", **context)
        self._check_finite(normal, "NORMAL", **context)
        self._check_finite(texcoord, texcoord_name, **context)
        if position_count is not None:
            self._check_indices(index_accessor, position_count, **context)

        joint_names = sorted(name for name in attributes if name.startswith("JOINTS_"))
        weight_names = sorted(name for name in attributes if name.startswith("WEIGHTS_"))
        has_base_skinning = "JOINTS_0" in attributes and "WEIGHTS_0" in attributes
        if skinned and not has_base_skinning:
            self.add(
                "geometry.mixed_static_skinned",
                "every primitive in a skinned GLB must provide JOINTS_0 and WEIGHTS_0",
                **context,
            )
        if not skinned and (joint_names or weight_names):
            self.add(
                "geometry.mixed_static_skinned",
                "skinning attributes require exactly one skin",
                **context,
            )
        for name in joint_names + weight_names:
            if name not in ("JOINTS_0", "WEIGHTS_0"):
                self.add(
                    "skin.extra_influence_set",
                    f"{name} would be ignored; only four influence lanes are supported",
                    attribute=name,
                    **context,
                )

        if skinned:
            joints = self._accessor_for_attribute(
                attributes, "JOINTS_0", mesh_index, primitive_index, required=False
            )
            weights = self._accessor_for_attribute(
                attributes, "WEIGHTS_0", mesh_index, primitive_index, required=False
            )
            self._validate_influences(joints, weights, position_count, **context)

    def _required_attribute(
        self, attributes: dict[str, Any], name: str, mesh: int, primitive: int
    ) -> tuple[int, dict[str, Any]] | None:
        if name not in attributes:
            self.add(
                f"attribute.{name.lower()}",
                f"primitive is missing {name}",
                mesh=mesh,
                primitive=primitive,
                attribute=name,
            )
            return None
        return self._accessor_for_attribute(attributes, name, mesh, primitive)

    def _accessor_for_attribute(
        self,
        attributes: dict[str, Any],
        name: str,
        mesh: int,
        primitive: int,
        *,
        required: bool = True,
    ) -> tuple[int, dict[str, Any]] | None:
        value = attributes.get(name)
        if not isinstance(value, int):
            if required:
                self.add(
                    "attribute.accessor",
                    f"{name} must reference an accessor",
                    mesh=mesh,
                    primitive=primitive,
                    attribute=name,
                )
            return None
        return self._accessor(value, mesh, primitive, name)

    def _accessor(
        self, index: int, mesh: int | None, primitive: int | None, attribute: str
    ) -> tuple[int, dict[str, Any]] | None:
        accessors = self.document.get("accessors", [])
        if not isinstance(accessors, list) or index < 0 or index >= len(accessors):
            self.add(
                "accessor.reference",
                f"{attribute} references invalid accessor {index}",
                mesh=mesh,
                primitive=primitive,
                accessor=index,
                attribute=attribute,
            )
            return None
        accessor = accessors[index]
        if not isinstance(accessor, dict):
            return None
        return index, accessor

    def _selected_texcoord(
        self, primitive: dict[str, Any], attributes: dict[str, Any]
    ) -> str | None:
        available = sorted(name for name in attributes if name.startswith("TEXCOORD_"))
        if not available:
            return None
        material_index = primitive.get("material")
        materials = self.document.get("materials", [])
        if isinstance(material_index, int) and isinstance(materials, list) and 0 <= material_index < len(materials):
            material = materials[material_index]
            if isinstance(material, dict):
                pbr = material.get("pbrMetallicRoughness", {})
                if isinstance(pbr, dict):
                    texture = pbr.get("baseColorTexture", {})
                    if isinstance(texture, dict):
                        preferred = f"TEXCOORD_{texture.get('texCoord', 0)}"
                        if preferred in attributes:
                            return preferred
        return available[0]

    def _check_accessor_shape(
        self,
        accessor_ref: tuple[int, dict[str, Any]] | None,
        expected_type: str,
        component_types: set[int],
        attribute: str | None,
        *,
        mesh: int,
        primitive: int,
    ) -> None:
        if accessor_ref is None:
            return
        index, accessor = accessor_ref
        if accessor.get("type") != expected_type or accessor.get("componentType") not in component_types:
            allowed = ", ".join(str(value) for value in sorted(component_types))
            self.add(
                "accessor.format",
                f"{attribute} must be {expected_type} with component type in [{allowed}]",
                mesh=mesh,
                primitive=primitive,
                accessor=index,
                attribute=attribute,
            )

    @staticmethod
    def _count(accessor_ref: tuple[int, dict[str, Any]] | None) -> int | None:
        if accessor_ref is None:
            return None
        count = accessor_ref[1].get("count")
        return count if isinstance(count, int) and count >= 0 else None

    def _check_finite(
        self,
        accessor_ref: tuple[int, dict[str, Any]] | None,
        attribute: str | None,
        *,
        mesh: int,
        primitive: int,
    ) -> None:
        if accessor_ref is None:
            return
        values = self._decode_accessor(accessor_ref[0], mesh, primitive, attribute or "")
        if values is None:
            return
        for vertex, lanes in enumerate(values):
            if any(not math.isfinite(float(value)) for value in lanes):
                self.add(
                    "accessor.non_finite",
                    f"{attribute} contains a non-finite value",
                    mesh=mesh,
                    primitive=primitive,
                    accessor=accessor_ref[0],
                    vertex=vertex,
                    attribute=attribute,
                )
                return

    def _check_indices(
        self,
        accessor_ref: tuple[int, dict[str, Any]] | None,
        vertex_count: int,
        *,
        mesh: int,
        primitive: int,
    ) -> None:
        if accessor_ref is None:
            return
        values = self._decode_accessor(accessor_ref[0], mesh, primitive, "INDICES")
        if values is None:
            return
        for position, lanes in enumerate(values):
            if len(lanes) != 1 or int(lanes[0]) < 0 or int(lanes[0]) >= vertex_count:
                self.add(
                    "index.range",
                    f"index {position} is outside the primitive vertex range",
                    mesh=mesh,
                    primitive=primitive,
                    accessor=accessor_ref[0],
                    vertex=position,
                    attribute="INDICES",
                )
                return

    def _validate_influences(
        self,
        joints_ref: tuple[int, dict[str, Any]] | None,
        weights_ref: tuple[int, dict[str, Any]] | None,
        vertex_count: int | None,
        *,
        mesh: int,
        primitive: int,
    ) -> None:
        self._check_accessor_shape(joints_ref, "VEC4", {5121, 5123}, "JOINTS_0", mesh=mesh, primitive=primitive)
        self._check_accessor_shape(weights_ref, "VEC4", {5121, 5123, 5126}, "WEIGHTS_0", mesh=mesh, primitive=primitive)
        for name, ref in (("JOINTS_0", joints_ref), ("WEIGHTS_0", weights_ref)):
            if vertex_count is not None and ref is not None and self._count(ref) != vertex_count:
                self.add(
                    "attribute.count",
                    f"{name} count must match POSITION count",
                    mesh=mesh,
                    primitive=primitive,
                    accessor=ref[0],
                    attribute=name,
                )
        if joints_ref is not None and joints_ref[1].get("normalized") is True:
            self.add(
                "skin.joint_format",
                "JOINTS_0 must not be normalized",
                mesh=mesh,
                primitive=primitive,
                accessor=joints_ref[0],
                attribute="JOINTS_0",
            )
        if weights_ref is not None and weights_ref[1].get("componentType") != 5126:
            if weights_ref[1].get("normalized") is not True:
                self.add(
                    "skin.weight_format",
                    "integer WEIGHTS_0 accessors must be normalized",
                    mesh=mesh,
                    primitive=primitive,
                    accessor=weights_ref[0],
                    attribute="WEIGHTS_0",
                )
        if weights_ref is None:
            return
        weights = self._decode_accessor(weights_ref[0], mesh, primitive, "WEIGHTS_0")
        if weights is None:
            return
        for vertex, lanes in enumerate(weights):
            if len(lanes) != 4:
                continue
            floats = [float(value) for value in lanes]
            if any(not math.isfinite(value) or value < 0.0 for value in floats):
                self.add(
                    "skin.weights_invalid",
                    "weights must be finite and non-negative",
                    mesh=mesh,
                    primitive=primitive,
                    accessor=weights_ref[0],
                    vertex=vertex,
                    attribute="WEIGHTS_0",
                )
            elif abs(sum(floats) - 1.0) > WEIGHT_SUM_TOLERANCE:
                self.add(
                    "skin.weights_normalized",
                    "the four weights for each vertex must sum to one",
                    mesh=mesh,
                    primitive=primitive,
                    accessor=weights_ref[0],
                    vertex=vertex,
                    attribute="WEIGHTS_0",
                )

    def _validate_skin_profile(self, skins: Any) -> None:
        if not isinstance(skins, list) or len(skins) != 1:
            count = len(skins) if isinstance(skins, list) else 0
            self.add("skin.count", f"skinned content must contain exactly one skin; found {count}")
        if not isinstance(skins, list) or not skins or not isinstance(skins[0], dict):
            return
        skin = skins[0]
        joint_nodes = skin.get("joints", [])
        if not isinstance(joint_nodes, list):
            self.add("skin.joints", "skin joints must be an array")
            return
        joint_count = len(joint_nodes)
        if joint_count == 0:
            self.add("skin.joints", "skin must contain at least one joint")
        if joint_count > MAX_JOINTS_PER_SKELETON:
            self.add(
                "skin.joint_limit",
                f"skin has {joint_count} joints; the engine limit is {MAX_JOINTS_PER_SKELETON}",
            )

        nodes = self.document.get("nodes", [])
        names: dict[str, int] = {}
        for joint_index, node_index in enumerate(joint_nodes):
            if not isinstance(node_index, int) or not isinstance(nodes, list) or not 0 <= node_index < len(nodes):
                self.add(
                    "skin.joint_node",
                    "joint references an invalid node",
                    joint=joint_index,
                )
                continue
            node = nodes[node_index]
            name = node.get("name") if isinstance(node, dict) else None
            if not isinstance(name, str) or not name.strip():
                self.add("skin.joint_name", "joint node must have a non-empty name", joint=joint_index)
            elif name in names:
                self.add(
                    "skin.joint_name",
                    f"joint name {name!r} is duplicated",
                    joint=joint_index,
                )
            else:
                names[name] = joint_index

        inverse_bind = skin.get("inverseBindMatrices")
        if inverse_bind is not None:
            if not isinstance(inverse_bind, int):
                self.add("skin.inverse_bind", "inverseBindMatrices must reference an accessor")
            else:
                ref = self._accessor(inverse_bind, None, None, "inverseBindMatrices")
                if ref is not None:
                    self._check_accessor_shape(
                        ref, "MAT4", {5126}, "inverseBindMatrices", mesh=-1, primitive=-1
                    )
                    if self._count(ref) != joint_count:
                        self.add(
                            "skin.inverse_bind",
                            "inverse-bind matrix count must match skin joint count",
                            accessor=ref[0],
                        )
                    self._check_finite(ref, "inverseBindMatrices", mesh=-1, primitive=-1)

        self._validate_mesh_bindings()
        self._validate_joint_ranges(joint_count)

    def _validate_mesh_bindings(self) -> None:
        nodes = self.document.get("nodes", [])
        bindings: dict[int, list[tuple[int, Any]]] = {}
        if isinstance(nodes, list):
            for node_index, node in enumerate(nodes):
                if isinstance(node, dict) and isinstance(node.get("mesh"), int):
                    bindings.setdefault(node["mesh"], []).append((node_index, node.get("skin")))
        meshes = self.document.get("meshes", [])
        if not isinstance(meshes, list):
            return
        for mesh_index in range(len(meshes)):
            mesh_bindings = bindings.get(mesh_index, [])
            if not mesh_bindings:
                self.add(
                    "skin.mesh_binding",
                    "every mesh in a skinned GLB must be instantiated by a node using skin 0",
                    mesh=mesh_index,
                )
                continue
            for node_index, skin_index in mesh_bindings:
                if skin_index != 0:
                    self.add(
                        "skin.mesh_binding",
                        f"mesh node {node_index} must use skin 0",
                        mesh=mesh_index,
                    )

    def _validate_joint_ranges(self, joint_count: int) -> None:
        for mesh, primitive_index, primitive in self.mesh_primitives:
            attributes = primitive.get("attributes", {})
            if not isinstance(attributes, dict) or not isinstance(attributes.get("JOINTS_0"), int):
                continue
            accessor_index = attributes["JOINTS_0"]
            values = self._decode_accessor(accessor_index, mesh, primitive_index, "JOINTS_0")
            if values is None:
                continue
            for vertex, lanes in enumerate(values):
                for value in lanes:
                    joint = int(value)
                    if joint < 0 or joint >= joint_count:
                        self.add(
                            "skin.joint_range",
                            f"joint index {joint} is outside skin 0",
                            mesh=mesh,
                            primitive=primitive_index,
                            joint=joint,
                            accessor=accessor_index,
                            vertex=vertex,
                            attribute="JOINTS_0",
                        )
                        break

    def _validate_animations(self, skinned: bool, skins: Any) -> None:
        animations = self.document.get("animations", [])
        if not isinstance(animations, list) or not animations:
            return
        joint_nodes: set[int] = set()
        if skinned and isinstance(skins, list) and skins and isinstance(skins[0], dict):
            raw_joints = skins[0].get("joints", [])
            if isinstance(raw_joints, list):
                joint_nodes = {value for value in raw_joints if isinstance(value, int)}
        seen_names: set[str] = set()
        for animation_index, animation in enumerate(animations):
            if not isinstance(animation, dict):
                self.add("animation.invalid", f"animation {animation_index} must be an object")
                continue
            name = animation.get("name")
            if not isinstance(name, str) or not name or any(char not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for char in name):
                self.add("animation.name", f"animation {animation_index} has an unsafe or empty name")
            elif name in seen_names:
                self.add("animation.name", f"animation name {name!r} is duplicated")
            else:
                seen_names.add(name)
            channels = animation.get("channels", [])
            if not isinstance(channels, list) or not channels:
                self.add("animation.empty", f"animation {animation_index} has no channels")
                continue
            for channel_index, channel in enumerate(channels):
                target = channel.get("target", {}) if isinstance(channel, dict) else {}
                path = target.get("path") if isinstance(target, dict) else None
                node = target.get("node") if isinstance(target, dict) else None
                if path not in ("translation", "rotation", "scale"):
                    self.add(
                        "animation.path",
                        f"animation {animation_index} channel {channel_index} uses unsupported path {path!r}",
                    )
                if skinned and node not in joint_nodes:
                    self.add(
                        "animation.target",
                        f"animation {animation_index} channel {channel_index} targets a node outside skin 0",
                        joint=node if isinstance(node, int) else None,
                    )

    def _decode_accessor(
        self, index: int, mesh: int | None, primitive: int | None, attribute: str
    ) -> list[tuple[float | int, ...]] | None:
        if index in self._decoded_accessors:
            return self._decoded_accessors[index]
        if index in self._accessor_failures:
            return None
        try:
            values = self._decode_accessor_unchecked(index)
        except (KeyError, IndexError, TypeError, ValueError, struct.error) as error:
            self._accessor_failures.add(index)
            self.add(
                "accessor.data",
                f"cannot read accessor: {error}",
                mesh=mesh,
                primitive=primitive,
                accessor=index,
                attribute=attribute,
            )
            return None
        self._decoded_accessors[index] = values
        return values

    def _decode_accessor_unchecked(self, index: int) -> list[tuple[float | int, ...]]:
        accessors = self.document["accessors"]
        accessor = accessors[index]
        if "sparse" in accessor:
            raise ValueError("sparse accessor data is unsupported")
        view_index = accessor.get("bufferView")
        if not isinstance(view_index, int):
            raise ValueError("accessor has no bufferView")
        views = self.document.get("bufferViews", [])
        if not isinstance(views, list) or not 0 <= view_index < len(views):
            raise ValueError("invalid bufferView reference")
        view = views[view_index]
        if not isinstance(view, dict) or view.get("buffer", 0) != 0:
            raise ValueError("only the GLB binary buffer is supported")
        component_type = accessor.get("componentType")
        accessor_type = accessor.get("type")
        if component_type not in COMPONENTS or accessor_type not in TYPE_WIDTHS:
            raise ValueError("unsupported accessor format")
        count = accessor.get("count")
        if not isinstance(count, int) or count < 0:
            raise ValueError("invalid accessor count")
        format_char, component_size = COMPONENTS[component_type]
        width = TYPE_WIDTHS[accessor_type]
        element_size = component_size * width
        stride = view.get("byteStride", element_size)
        if not isinstance(stride, int) or stride < element_size:
            raise ValueError("invalid byte stride")
        offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        view_end = view.get("byteOffset", 0) + view.get("byteLength", 0)
        if not isinstance(offset, int) or not isinstance(view_end, int):
            raise ValueError("invalid byte offset")
        if count and offset + (count - 1) * stride + element_size > view_end:
            raise ValueError("accessor exceeds its bufferView")
        if view_end > len(self.binary):
            raise ValueError("bufferView exceeds the GLB binary chunk")
        unpack_format = "<" + format_char * width
        normalized = accessor.get("normalized") is True
        result: list[tuple[float | int, ...]] = []
        for element in range(count):
            lanes = struct.unpack_from(unpack_format, self.binary, offset + element * stride)
            if normalized and component_type != 5126:
                lanes = tuple(self._normalize_component(value, component_type) for value in lanes)
            result.append(lanes)
        return result

    @staticmethod
    def _normalize_component(value: int, component_type: int) -> float:
        if component_type == 5120:
            return max(float(value) / 127.0, -1.0)
        if component_type == 5121:
            return float(value) / 255.0
        if component_type == 5122:
            return max(float(value) / 32767.0, -1.0)
        if component_type == 5123:
            return float(value) / 65535.0
        return float(value)


def parse_glb(path: Path) -> tuple[dict[str, Any], bytes]:
    data = path.read_bytes()
    if len(data) < 12:
        raise GlbError("file is shorter than the GLB header")
    magic, version, declared_length = struct.unpack_from("<4sII", data, 0)
    if magic != GLB_MAGIC:
        raise GlbError("file does not have the GLB magic")
    if version != GLB_VERSION:
        raise GlbError(f"unsupported GLB version {version}")
    if declared_length != len(data):
        raise GlbError("GLB header length does not match file length")

    offset = 12
    chunks: list[tuple[int, bytes]] = []
    while offset < len(data):
        if offset + 8 > len(data):
            raise GlbError("truncated GLB chunk header")
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + chunk_length
        if end > len(data):
            raise GlbError("truncated GLB chunk")
        chunks.append((chunk_type, data[offset:end]))
        offset = end
    if not chunks or chunks[0][0] != JSON_CHUNK:
        raise GlbError("the first GLB chunk must be JSON")
    if sum(1 for chunk_type, _ in chunks if chunk_type == JSON_CHUNK) != 1:
        raise GlbError("GLB must contain exactly one JSON chunk")
    try:
        document = json.loads(chunks[0][1].decode("utf-8").rstrip(" \t\r\n\0"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GlbError(f"invalid GLB JSON chunk: {error}") from error
    if not isinstance(document, dict):
        raise GlbError("GLB JSON root must be an object")
    asset = document.get("asset")
    if not isinstance(asset, dict) or asset.get("version") != "2.0":
        raise GlbError("GLB asset.version must be 2.0")
    binary_chunks = [payload for chunk_type, payload in chunks if chunk_type == BIN_CHUNK]
    if len(binary_chunks) != 1:
        raise GlbError("GLB must contain exactly one binary chunk")
    buffers = document.get("buffers", [])
    if not isinstance(buffers, list) or len(buffers) != 1 or not isinstance(buffers[0], dict):
        raise GlbError("GLB must declare exactly one buffer")
    if "uri" in buffers[0]:
        raise GlbError("GLB buffer must use the embedded binary chunk")
    byte_length = buffers[0].get("byteLength")
    if not isinstance(byte_length, int) or byte_length < 0 or byte_length > len(binary_chunks[0]):
        raise GlbError("declared buffer length exceeds the binary chunk")
    return document, binary_chunks[0][:byte_length]


def report_for_path(path: Path) -> tuple[dict[str, Any], int]:
    try:
        document, binary = parse_glb(path)
    except (OSError, GlbError) as error:
        finding = {
            "rule_id": "glb.invalid",
            "severity": "error",
            "message": str(error),
            "location": empty_location(),
        }
        return {
            "path": os.fspath(path),
            "valid": False,
            "profile": None,
            "findings": [finding],
        }, 1
    validator = Validator(path, document, binary)
    profile = validator.validate()
    report = {
        "path": os.fspath(path),
        "valid": not validator.findings,
        "profile": profile,
        "findings": validator.findings,
    }
    return report, 0 if report["valid"] else 1


def _usage_error(message: str) -> int:
    json.dump(
        {
            "error": "usage",
            "message": message,
            "usage": "validate_glb.py [--json] [--pretty] FILE.glb",
        },
        sys.stderr,
        sort_keys=True,
    )
    sys.stderr.write("\n")
    return 2


def main(argv: Iterable[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if args in (["-h"], ["--help"]):
        print("usage: validate_glb.py [--json] [--pretty] FILE.glb")
        return 0
    pretty = "--pretty" in args
    args = [arg for arg in args if arg not in ("--json", "--pretty")]
    if len(args) != 1:
        return _usage_error("exactly one GLB path is required")
    path = Path(args[0])
    if path.suffix.lower() != ".glb":
        return _usage_error("input path must end in .glb")
    if not path.is_file():
        return _usage_error(f"input file does not exist: {path}")
    report, exit_code = report_for_path(path)
    json.dump(report, sys.stdout, indent=2 if pretty else None, sort_keys=True)
    sys.stdout.write("\n")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
