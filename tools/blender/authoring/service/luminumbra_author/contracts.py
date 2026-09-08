"""Strict contracts and bounded file access for the geometry build profile."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import uuid

ASSET = "luminumbra.authoring.asset.v1"
RECEIPT = "luminumbra.authoring.receipt.v1"
GENERATION = "luminumbra.authoring.generation.v1"
TOOLCHAIN = "luminumbra.authoring.toolchain.v1"
PROFILE = "glb-geometry-v1"
PREFAB_PROFILE = "glb-static-prefab-v1"
MAX_SNAPSHOT = 256 * 1024 * 1024
MAX_DOCUMENT = 1024 * 1024
MAX_OUTPUT = 256 * 1024 * 1024
ID = re.compile(r"[A-Za-z][A-Za-z0-9_.:-]{0,127}\Z")
SHA = re.compile(r"[0-9a-f]{64}\Z")


class Refusal(ValueError):
    def __init__(self, rule, message, field="", object_id=None):
        super().__init__(message)
        self.finding = {"rule_id": rule, "severity": "error", "message": message,
                        "field": field, "object_id": object_id,
                        "component_id": None, "node_id": None}


def require(condition, rule, message, field=""):
    if not condition:
        raise Refusal(rule, message, field)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse_json(data):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, "json.duplicate", "Duplicate JSON field.", key)
            value[key] = item
        return value

    def invalid(value):
        raise Refusal("json.nonfinite", f"Nonfinite JSON number: {value}")

    result = json.loads(data, object_pairs_hook=unique, parse_constant=invalid)
    canonical(result)  # Reject numeric overflow such as 1e999, including annotations.
    return result


def read_bounded(path, limit):
    with Path(path).open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, "file.limit", "File exceeds this profile's size limit.")
    return data


def read_json(path):
    return parse_json(read_bounded(path, MAX_DOCUMENT))


def file_digest(path):
    checksum = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    temp = path.with_name(".tmp-" + uuid.uuid4().hex)
    try:
        with temp.open("xb") as stream:
            stream.write(canonical(value) + b"\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def keys(value, required, optional=()):
    require(isinstance(value, dict), "schema.type", "Expected an object.")
    require(set(required) <= value.keys(), "schema.required", "Required field is missing.")
    unknown = value.keys() - set(required) - set(optional)
    require(not unknown, "schema.unknown_field", f"Unregistered fields: {sorted(unknown)}")


def relative_path(root, name):
    require(isinstance(name, str) and bool(name), "path.invalid", "Use a relative file path.")
    parts = PurePosixPath(name).parts
    require(not name.startswith("/") and ".." not in parts and "\\" not in name
            and ":" not in name and "\x00" not in name
            and str(PurePosixPath(name)) == name,
            "path.escape", "Use a canonical project-relative path.", name)
    root = Path(root).resolve()
    path = root
    for part in parts:
        path /= part
        require(not path.is_symlink(), "path.symlink", "Source/state symlinks are unsupported.", name)
    require(path.is_file() and path.resolve().is_relative_to(root),
            "path.missing", "File does not exist inside its declared root.", name)
    return path


def validate_asset(value):
    keys(value, ("schema", "asset_id", "revision", "profile", "source", "dependencies", "exporter"),
         ("settings", "required_schemas", "annotations", "expected_dependency_hashes"))
    require(value["schema"] == ASSET, "schema.unsupported", "Unknown required asset schema.")
    require(value["profile"] in (PROFILE, PREFAB_PROFILE), "profile.unsupported", "Unsupported compilation profile.")
    require(isinstance(value["asset_id"], str) and ID.fullmatch(value["asset_id"]),
            "identity.invalid", "Use a stable namespaced asset ID.", "asset_id")
    require(type(value["revision"]) is int and value["revision"] >= 0,
            "revision.invalid", "Revision must be a nonnegative integer.", "revision")
    require(isinstance(value["source"], str) and value["source"].endswith(".glb"),
            "source.profile", "This profile requires a self-contained GLB snapshot.")
    deps = value["dependencies"]
    require(isinstance(deps, list) and len(deps) <= 128 and all(isinstance(x, str) for x in deps)
            and len(set(deps)) == len(deps) and value["source"] not in deps,
            "dependency.invalid", "Supply at most 128 distinct source dependency paths.")
    expected = value.get("expected_dependency_hashes", {})
    require(isinstance(expected, dict) and expected.keys() <= set(deps)
            and all(isinstance(x, str) and SHA.fullmatch(x) for x in expected.values()),
            "dependency.expected_hash", "Expected hashes must name declared dependencies and contain SHA-256 values.")
    required = value.get("required_schemas", [])
    require(isinstance(required, list) and all(x == ASSET for x in required),
            "schema.unsupported", "Unknown required schema; no execution occurred.")
    exporter = value["exporter"]
    keys(exporter, ("id", "sha256"))
    require(isinstance(exporter["id"], str) and ID.fullmatch(exporter["id"])
            and isinstance(exporter["sha256"], str) and SHA.fullmatch(exporter["sha256"]),
            "exporter.identity", "Record the exporter identity and SHA-256.")
    settings = value.get("settings", {})
    keys(settings, (), ("emit_lods",))
    require(type(settings.get("emit_lods", False)) is bool,
            "settings.invalid", "emit_lods must be a boolean.")
    require(isinstance(value.get("annotations", {}), dict),
            "annotation.invalid", "Annotations must be an inert object.")
    canonical(value)
    return value


class Toolchain:
    """Locally configured trust: never accept executables from build requests."""
    def __init__(self, manifest):
        self.path = Path(manifest).resolve(strict=True)
        self.document = read_json(self.path)
        keys(self.document, ("schema", "id", "processor", "files"))
        require(self.document["schema"] == TOOLCHAIN, "toolchain.schema", "Unknown toolchain schema.")
        require(isinstance(self.document["id"], str) and ID.fullmatch(self.document["id"]),
                "toolchain.identity", "Supply an installed module/toolchain ID.")
        files = self.document["files"]
        require(isinstance(files, dict) and 1 <= len(files) <= 256,
                "toolchain.files", "Record the compiler and its installed libraries.")
        require(isinstance(self.document["processor"], str) and self.document["processor"] in files,
                "toolchain.processor", "The compiler must be a hashed toolchain member.")
        for name, expected in files.items():
            require(isinstance(expected, str) and SHA.fullmatch(expected),
                    "toolchain.hash", "Toolchain members need SHA-256 digests.", name)
        self.processor = relative_path(self.path.parent, self.document["processor"])
        self.verify()

    def verify(self):
        require(read_json(self.path) == self.document, "toolchain.changed", "Toolchain manifest changed.")
        for name, expected in self.document["files"].items():
            path = relative_path(self.path.parent, name)
            require(file_digest(path) == expected, "toolchain.changed", "Installed toolchain changed.", name)
        return self.document
