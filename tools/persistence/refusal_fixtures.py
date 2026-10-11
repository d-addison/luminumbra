#!/usr/bin/env python3
"""Deterministic structural refusal fixtures; native qualification is separate."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
from pathlib import Path

import save_receipt

SCHEMA = "luminumbra.refusal_fixtures.v1"
MANIFEST = "refusal-manifest.json"
HEADER = "src/luminumbra_common/persistence/WorldSaveService.h"
WORLD_MANIFEST = "chunks/region/world-manifest.json"
VARIANTS = (
    "obsolete-metadata", "future-metadata", "corrupt-metadata",
    "missing-metadata", "missing-preset", "truncated-region",
)
BLOCK = 1 << 20
JSON_LIMIT = 1 << 20
MANIFEST_LIMIT = 64 << 20
REGION = re.compile(r"r\.(-?(?:0|[1-9][0-9]*))\.(-?(?:0|[1-9][0-9]*))\.lmr\Z")
SAFE_NAME = re.compile(r"[A-Za-z0-9_.-]+\Z")


class FixtureError(ValueError):
    """Invalid fixture input or output."""


def require(condition, message):
    if not condition:
        raise FixtureError(message)


def _regular_status(status):
    require(not stat.S_ISLNK(status.st_mode), "symlink is forbidden")
    require(not getattr(status, "st_file_attributes", 0) & 0x400,
            "reparse point is forbidden")
    require(stat.S_ISDIR(status.st_mode) or stat.S_ISREG(status.st_mode),
            "special file is forbidden")


def safe_path(value, *, new=False):
    """Inspect raw ancestors BEFORE normalizing '..'; never resolve links."""
    raw = Path(value)
    if not raw.is_absolute():
        raw = Path.cwd() / raw
    cursor = Path(raw.anchor)
    parts = raw.parts[1:]
    _regular_status(cursor.lstat())
    for index, part in enumerate(parts):
        cursor = cursor / part
        try:
            status = cursor.lstat()
        except FileNotFoundError:
            require(new and index == len(parts) - 1, "path ancestor is missing")
            break
        _regular_status(status)
        require(index == len(parts) - 1 or stat.S_ISDIR(status.st_mode),
                "path ancestor is not a directory")
        require(not (new and index == len(parts) - 1), "output already exists")
    normalized = Path(os.path.abspath(raw))
    if new:
        require(not os.path.lexists(normalized), "output already exists")
    return normalized


def _scan(root):
    """Inventory directories as well as files, rejecting unsafe entries first."""
    require(stat.S_ISDIR(root.lstat().st_mode), "tree root is not a directory")
    files, directories = set(), set()

    def visit(directory, prefix):
        with os.scandir(directory) as iterator:
            entries = sorted(iterator, key=lambda entry: entry.name)
        for entry in entries:
            status = entry.stat(follow_symlinks=False)
            _regular_status(status)
            relative = prefix + entry.name
            require("\\" not in entry.name and ":" not in entry.name,
                    "unsupported tree entry name")
            if stat.S_ISDIR(status.st_mode):
                directories.add(relative)
                visit(Path(entry.path), relative + "/")
            else:
                files.add(relative)

    _regular_status(root.lstat())
    visit(root, "")
    return files, directories


def _directories(files):
    return {str(parent.as_posix()) for name in files
            for parent in Path(name).parents if parent != Path(".")}


def _exact_tree(root, files):
    actual_files, actual_dirs = _scan(root)
    require(actual_files == set(files) and actual_dirs == _directories(files),
            "unexpected or missing tree artifacts")


def _pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def _finite(value):
    if isinstance(value, float):
        require(math.isfinite(value), "nonfinite JSON number")
    elif isinstance(value, dict):
        for item in value.values():
            _finite(item)
    elif isinstance(value, list):
        for item in value:
            _finite(item)


def _bytes(path, limit=JSON_LIMIT):
    with path.open("rb") as handle:
        data = handle.read(limit + 1)
    require(len(data) <= limit, "file exceeds size limit")
    return data


def _json(path, limit=JSON_LIMIT):
    data = _bytes(path, limit)
    try:
        value = json.loads(data, object_pairs_hook=_pairs,
                           parse_constant=lambda _: require(False, "nonfinite JSON number"))
        _finite(value)
        return value
    except (ValueError, UnicodeError, RecursionError) as error:
        raise FixtureError("invalid JSON") from error


def _integer(value, lower, upper):
    return type(value) is int and lower <= value <= upper


def _name(value):
    return (isinstance(value, str) and value not in (".", "..") and
            SAFE_NAME.fullmatch(value) is not None)


def _has_world_identifier(value):
    if isinstance(value, dict):
        return ("id" in value or "worldId" in value or
                any(_has_world_identifier(item) for item in value.values()))
    if isinstance(value, list):
        return any(_has_world_identifier(item) for item in value)
    return False


def engine_version(engine_root):
    path = safe_path(engine_root / HEADER)
    require(stat.S_ISREG(path.lstat().st_mode), "engine header is not regular")
    with path.open("rb") as handle:
        data = handle.read(JSON_LIMIT + 1)
    require(len(data) <= JSON_LIMIT, "engine header exceeds size limit")
    text = data.decode("utf-8")
    # Strip comments and string literals so examples cannot count as declarations.
    text = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  " ", text, flags=re.S)
    declarations = re.findall(
        r"\b(?:static\s+)?constexpr\s+std::uint16_t\s+kContainerVersion\s*=\s*([0-9]+)\s*;",
        text)
    candidates = re.findall(
        r"\bconstexpr\s+[^;{}=]*\bkContainerVersion\s*=[^;{}]*;", text)
    require(len(declarations) == 1 and len(candidates) == 1,
            "missing or ambiguous engine container version declaration")
    value = int(declarations[0])
    require(2 <= value <= 65535, "unsupported engine container version")
    return value


def source_revision(engine_root):
    result = subprocess.run(
        ["git", "-C", str(engine_root), "rev-parse", "--verify", "HEAD^{commit}"],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
    )
    revision = result.stdout.decode("ascii").strip()
    require(re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", revision) is not None,
            "engine HEAD is not a full commit identifier")
    return revision


def _region_name(name):
    match = REGION.fullmatch(name)
    return (match is not None and all(str(int(part)) == part and
            -(1 << 31) <= int(part) < (1 << 31) for part in match.groups()))


def _frame(path, version):
    """Check framing and header fields without interpreting compressed payloads."""
    size = path.stat().st_size
    with path.open("rb") as handle:
        header = handle.read(8)
        require(len(header) == 8 and header[:4] == b"LMR1", "invalid region header")
        actual_version, count = struct.unpack("<HH", header[4:])
        require(actual_version == version and count > 0, "region version/count mismatch")
        payload_offset = 8 + count * 18
        require(size >= payload_offset, "truncated record headers")
        keys = set()
        for _ in range(count):
            record = handle.read(18)
            require(len(record) == 18, "truncated record header")
            identity, lod, flags, raw_size, compressed_size = struct.unpack("<QBBII", record)
            require(lod <= 2 and flags & ~3 == 0 and (lod, identity) not in keys,
                    "unknown or duplicate region record")
            require(raw_size > 0 and compressed_size > 0, "empty record payload")
            keys.add((lod, identity))
            payload_offset += compressed_size
        require(payload_offset == size, "region payload framing/count/length mismatch")
    return size


def _valid_save(save, version):
    files, _ = _scan(save)
    regions = sorted(name for name in files if name.startswith("chunks/region/")
                     and _region_name(name[len("chunks/region/"):]))
    require(regions, "no recognized immediate region files")
    _exact_tree(save, {"world_info.json", "preset.json", WORLD_MANIFEST, *regions})
    metadata = _json(save / "world_info.json")
    require(isinstance(metadata, dict), "metadata must be an object")
    require(not _has_world_identifier(metadata),
            "unexpected metadata world identifier")
    require(type(metadata.get("container_version")) is int and
            metadata["container_version"] == version, "metadata version mismatch")
    require(_name(metadata.get("worldType")), "invalid metadata worldType")
    require(isinstance(_json(save / "preset.json"), dict), "preset must be an object")
    native_manifest = _json(save / WORLD_MANIFEST)
    _same(native_manifest, {
        "schema": "luminumbra.persistence.world_manifest.v1",
        "container": "LMR1", "container_version": version,
    })
    for region in regions:
        _frame(save / region, version)
    return metadata, files, regions[0]


def _same(actual, expected):
    """Strict schema/value equality, including integers versus booleans/floats."""
    require(type(actual) is type(expected), "manifest value type mismatch")
    if isinstance(expected, dict):
        require(actual.keys() == expected.keys(), "manifest fields mismatch")
        for key in expected:
            _same(actual[key], expected[key])
    elif isinstance(expected, list):
        require(len(actual) == len(expected), "manifest list length mismatch")
        for left, right in zip(actual, expected):
            _same(left, right)
    else:
        require(actual == expected, "manifest value mismatch")


def _metadata_version_bytes(path, version):
    """Replace only the top-level version token, preserving all other bytes."""
    # Callers have already strictly validated this bounded UTF-8 JSON object.
    text = _bytes(path).decode("utf-8")
    decoder = json.JSONDecoder()
    offset = text.index("{") + 1
    while True:
        while text[offset].isspace() or text[offset] == ",":
            offset += 1
        key, offset = decoder.raw_decode(text, offset)
        while text[offset].isspace():
            offset += 1
        require(text[offset] == ":", "invalid metadata separator")
        offset += 1
        while text[offset].isspace():
            offset += 1
        start = offset
        _, offset = decoder.raw_decode(text, offset)
        if key == "container_version":
            return (text[:start] + str(version) + text[offset:]).encode("utf-8")
        require(text[offset:].lstrip()[0] != "}", "metadata version missing")


def _copy_file(source, target, length=None):
    remaining = source.stat().st_size if length is None else length
    with source.open("rb") as reader, target.open("xb") as writer:
        while remaining:
            block = reader.read(min(BLOCK, remaining))
            require(block, "source changed while copying")
            writer.write(block)
            remaining -= len(block)


def _equal_prefix(left, right, length):
    with left.open("rb") as before, right.open("rb") as after:
        while length:
            amount = min(length, BLOCK)
            block = before.read(amount)
            require(len(block) == amount and block == after.read(amount),
                    "incorrect truncated region bytes")
            length -= amount
        require(after.read(1) == b"", "truncated region has extra bytes")


def _mutation_path(identity, first_region):
    if identity == "missing-preset":
        return "preset.json"
    if identity == "truncated-region":
        return first_region
    return "world_info.json"


def _file_identity(inventory, path):
    for item in inventory["files"]:
        if item["path"] == path:
            return {"size": item["size"], "sha256": item["sha256"]}
    return None


def _expected_manifest(output, world_id, version, revision):
    baseline = output / "worlds/saves" / world_id
    metadata, files, first_region = _valid_save(baseline, version)
    inventory = save_receipt.snapshot(baseline)
    entries = []
    output_files = {f"worlds/saves/{world_id}/{path}" for path in files}
    for identity in VARIANTS:
        clone = output / "refusals" / identity
        path = _mutation_path(identity, first_region)
        missing = identity in ("missing-metadata", "missing-preset")
        variant_files = files - {path} if missing else files
        _exact_tree(clone, variant_files)
        output_files.update(f"refusals/{identity}/{name}" for name in variant_files)
        actual = save_receipt.snapshot(clone)
        differences = save_receipt.compare(inventory, actual)
        _same(differences, {"added": [], "removed": [path] if missing else [],
                            "changed": [] if missing else [path]})
        reason = {"kind": identity}
        if identity in ("obsolete-metadata", "future-metadata"):
            mutation_version = 1 if identity == "obsolete-metadata" else version + 1
            reason["version"] = mutation_version
            require(_bytes(clone / path, JSON_LIMIT + 8) ==
                    _metadata_version_bytes(baseline / path, mutation_version),
                    "incorrect metadata version mutation")
        elif identity == "corrupt-metadata":
            require(_bytes(clone / path, 1) == b"{", "incorrect corrupt metadata bytes")
        elif identity == "truncated-region":
            length = _frame(baseline / path, version) - 1
            require((clone / path).stat().st_size == length, "incorrect truncation length")
            _equal_prefix(baseline / path, clone / path, length)
        entries.append({
            "id": identity, "world_id": identity, "world_type": metadata["worldType"],
            "expected_exit": 2, "reason": reason, "inventory": actual,
            "mutation": {"path": path, "before": _file_identity(inventory, path),
                         "after": _file_identity(actual, path)},
            "runtime_requirements": {
                "atlas_preset_absent": metadata["worldType"] if identity == "missing-preset" else None,
            },
        })
    _exact_tree(output, output_files | ({MANIFEST} if (output / MANIFEST).exists() else set()))
    return {
        "schema": SCHEMA, "source_revision": revision, "container_version": version,
        "source_world_id": world_id, "source": inventory, "baseline": inventory,
        "variants": entries,
    }


def create(source_root, out, engine_root):
    source = safe_path(source_root)
    output = safe_path(out, new=True)
    engine = safe_path(engine_root)
    require(output != source and source not in output.parents and output not in source.parents,
            "source and output overlap")
    version = engine_version(engine)
    # Validate the whole source tree before reading any source JSON or payload.
    source_files, source_dirs = _scan(source)
    saves = source / "worlds/saves"
    require("worlds/saves" in source_dirs, "source save directory is missing")
    children = list(saves.iterdir())
    require(len(children) == 1 and children[0].is_dir() and _name(children[0].name),
            "source must contain exactly one save world")
    save = children[0]
    _, files, first_region = _valid_save(save, version)
    _exact_tree(source, {f"worlds/saves/{save.name}/{path}" for path in files})
    original = save_receipt.snapshot(save)
    revision = source_revision(engine)
    created_status = None
    try:
        output.mkdir()  # exclusive: an existing tree is never owned by this invocation
        created_status = output.lstat()
        targets = [(output / "worlds/saves" / save.name, None)]
        targets.extend((output / "refusals" / identity, identity) for identity in VARIANTS)
        for target, identity in targets:
            for directory in sorted(_directories(files)):
                (target / directory).mkdir(parents=True, exist_ok=True)
            path = _mutation_path(identity, first_region) if identity else None
            for relative in sorted(files):
                destination = target / relative
                if relative == path:
                    if identity in ("missing-metadata", "missing-preset"):
                        continue
                    if identity in ("obsolete-metadata", "future-metadata"):
                        value = 1 if identity == "obsolete-metadata" else version + 1
                        with destination.open("xb") as handle:
                            handle.write(_metadata_version_bytes(save / relative, value))
                        continue
                    if identity == "corrupt-metadata":
                        with destination.open("xb") as handle:
                            handle.write(b"{")
                        continue
                    _copy_file(save / relative, destination, (save / relative).stat().st_size - 1)
                else:
                    _copy_file(save / relative, destination)
        _exact_tree(source, source_files)
        require(save_receipt.snapshot(save) == original, "source changed during production")
        manifest = _expected_manifest(output, save.name, version, revision)
        require(manifest["baseline"] == original, "baseline changed during production")
        # Publishing this file is the final write, after all structural checks.
        with (output / MANIFEST).open("x", encoding="utf-8", newline="\n") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True, allow_nan=False)
            handle.write("\n")
        require((output / MANIFEST).stat().st_size <= MANIFEST_LIMIT,
                "fixture manifest exceeds size limit")
        return manifest
    except BaseException:
        if created_status is not None:
            try:
                current = output.lstat()
                _regular_status(current)
                if (current.st_dev, current.st_ino) == (created_status.st_dev, created_status.st_ino):
                    shutil.rmtree(output)
            except (OSError, FixtureError):
                pass  # Never delete a replacement root or traverse an unsafe root.
        raise


def verify(output):
    root = safe_path(output)
    _scan(root)  # Reject unsafe entries before opening the manifest.
    manifest = _json(root / MANIFEST, MANIFEST_LIMIT)
    require(isinstance(manifest, dict), "manifest must be an object")
    version = manifest.get("container_version")
    revision = manifest.get("source_revision")
    world_id = manifest.get("source_world_id")
    require(_integer(version, 2, 65535), "invalid manifest container version")
    require(isinstance(revision, str) and
            re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", revision) is not None,
            "invalid manifest source revision")
    require(_name(world_id), "invalid manifest source world identifier")
    expected = _expected_manifest(root, world_id, version, revision)
    _same(manifest, expected)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    producer = commands.add_parser("create")
    producer.add_argument("--source-root", required=True)
    producer.add_argument("--out", required=True)
    producer.add_argument("--engine-root", required=True)
    checker = commands.add_parser("verify")
    checker.add_argument("output")
    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            create(args.source_root, args.out, args.engine_root)
        else:
            verify(args.output)
    except (ValueError, save_receipt.ReceiptError, OSError,
            subprocess.SubprocessError, RecursionError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"{args.command} ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
