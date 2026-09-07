"""Draft, deliberately narrow contract experiment. Not a runtime capability."""
from __future__ import annotations

from pathlib import Path, PurePosixPath
import re

from common import canonical, digest

ASSET = "luminumbra.authoring.asset.v1"
GRAPH = "luminumbra.behavior.graph.v1"
RECIPE = "luminumbra.authoring.recipe.v1"
RECEIPT = "luminumbra.authoring.receipt.v1"
PROFILE = "MOCK-contract-fixture-v1"
ID = re.compile(r"^[A-Za-z][A-Za-z0-9_.:-]{0,127}$")


class Refusal(ValueError):
    def __init__(self, rule, message, field="", object_id=None):
        super().__init__(message)
        self.finding = {"rule_id": rule, "severity": "error", "message": message,
                        "object_id": object_id, "component_id": None,
                        "node_id": None, "field": field}


def require(condition, rule, message, field="", object_id=None):
    if not condition:
        raise Refusal(rule, message, field, object_id)


def project_path(root, relative):
    require(isinstance(relative, str) and bool(relative), "path.invalid", "Use a project-relative path.")
    posix = PurePosixPath(relative)
    require(not posix.is_absolute() and ".." not in posix.parts and "\\" not in relative
            and ":" not in relative, "path.escape", "Path must stay inside the project.", relative)
    root = Path(root).resolve()
    path = root.joinpath(*posix.parts).resolve()
    require(path.is_relative_to(root), "path.escape", "Symlink escapes the project.", relative)
    require(path.is_file(), "path.missing", "Source or dependency does not exist.", relative)
    return path


def keys(value, required, optional=(), field=""):
    require(isinstance(value, dict), "schema.type", "Expected an object.", field)
    require(set(required) <= value.keys(), "schema.required", "Required field is missing.", field)
    unknown = value.keys() - set(required) - set(optional)
    require(not unknown, "schema.unknown_field", f"Unregistered fields: {sorted(unknown)}", field)


def identifier(value, field):
    require(isinstance(value, str) and ID.fullmatch(value), "identity.invalid", "Use a stable namespaced ID.", field)


def validate_asset(value):
    keys(value, ("schema", "asset_id", "revision", "profile", "objects", "dependencies"),
         ("annotations", "required_schemas"))
    require(value["schema"] == ASSET, "schema.unsupported", "Unknown required asset schema.", "schema")
    require(value["profile"] == PROFILE, "profile.unsupported", "This experiment only supports MOCK fixtures.", "profile")
    identifier(value["asset_id"], "asset_id")
    require(type(value["revision"]) is int and value["revision"] >= 0, "revision.invalid", "Revision must be a nonnegative integer.", "revision")
    required = value.get("required_schemas", [])
    require(isinstance(required, list) and all(x == ASSET for x in required),
            "schema.unsupported", "Unknown required schema; no execution occurred.", "required_schemas")
    require(isinstance(value["objects"], list) and len(value["objects"]) <= 1024,
            "objects.limit", "Expected at most 1024 objects.", "objects")
    ids = set()
    parents = {}
    for i, obj in enumerate(value["objects"]):
        field = f"objects/{i}"
        keys(obj, ("id", "parent", "asset_id", "transform", "components"), ("label",), field)
        identifier(obj["id"], field + "/id")
        require(obj["id"] not in ids, "identity.duplicate", "Duplicates need new object IDs.", field, obj["id"])
        ids.add(obj["id"])
        identifier(obj["asset_id"], field + "/asset_id")
        require("label" not in obj or isinstance(obj["label"], str),
                "schema.type", "Object label must be a string.", field + "/label")
        require(obj["parent"] is None or isinstance(obj["parent"], str), "hierarchy.parent", "Invalid parent ID.", field)
        parents[obj["id"]] = obj["parent"]
        transform = obj["transform"]
        require(isinstance(transform, list) and len(transform) == 16
                and all(type(x) in (int, float) for x in transform),
                "transform.invalid", "Expected a finite column-major 4x4 matrix.", field, obj["id"])
        require(obj["components"] == [], "component.unregistered",
                "MOCK has no executable registered components.", field + "/components", obj["id"])
    for obj_id in parents:
        visited = set()
        current = obj_id
        while current is not None:
            require(current in parents and current not in visited, "hierarchy.invalid", "Missing parent or cyclic hierarchy.", "objects", obj_id)
            visited.add(current)
            current = parents[current]
    deps = value["dependencies"]
    require(isinstance(deps, list) and len(deps) <= 128 and all(isinstance(x, str) for x in deps),
            "dependency.invalid", "Expected at most 128 relative dependency paths.", "dependencies")
    require(len(set(deps)) == len(deps), "dependency.duplicate", "Dependency paths must be unique.", "dependencies")
    require(isinstance(value.get("annotations", {}), dict), "annotation.invalid", "Annotations must be an inert JSON object.")
    try:
        canonical(value)
    except (ValueError, TypeError) as error:
        raise Refusal("schema.nonfinite", str(error)) from error
    return value


def graph_semantic_digest(graph):
    """Only root layout is non-executable; no recursive key stripping."""
    keys(graph, ("schema", "id", "definitions", "nodes", "connections"), ("layout",))
    require(graph["schema"] == GRAPH, "schema.unsupported", "Unknown graph schema.", "schema")
    # This hashes an interchange document; it does not validate or compile a language.
    return digest(canonical({k: v for k, v in graph.items() if k != "layout"}))
