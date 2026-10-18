#!/usr/bin/env python3
"""Materialize only the reviewed Jolt edits, leaving supplied sources read-only."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path("cmake/patches/jolt-startup.json")
_PATH_SPEC = importlib.util.spec_from_file_location("_jolt_path_safety", Path(__file__).with_name("path_safety.py"))
_PATH_SAFETY = importlib.util.module_from_spec(_PATH_SPEC)
_PATH_SPEC.loader.exec_module(_PATH_SAFETY)
REVISION = "0373ec0dd762e4bc2f6acdb08371ee84fa23c6db"
# The complete reviewed edit payload is bounded, independently of vendor size.
MAX_BYTES = 8 * 1024 * 1024


class PatchError(ValueError):
    pass


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ordinary(path: Path, directory: bool = False) -> None:
    _PATH_SAFETY.reject_reparse_points(path)
    for component in (path, *path.parents):
        info = component.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise PatchError(f"redirected path: {component.name}")
        if component != path and not stat.S_ISDIR(info.st_mode):
            raise PatchError("non-directory path component")
    info = path.lstat()
    if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
        raise PatchError(f"non-regular input: {path.name}")


def read(path: Path) -> bytes:
    ordinary(path)
    # Nonblocking open then fstat prevents a substituted FIFO from blocking.
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(descriptor, "rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_BYTES:
            raise PatchError("input exceeds regular-file bound")
        data = handle.read(MAX_BYTES + 1)
        if len(data) > MAX_BYTES:
            raise PatchError("input grew beyond bound")
        return data


def inputs(root: Path = ROOT) -> tuple[dict, bytes]:
    manifest_bytes = read(root / MANIFEST)
    manifest = json.loads(manifest_bytes)
    if not isinstance(manifest, dict):
        raise PatchError("invalid manifest object")
    if (manifest.get("version") != 1 or manifest.get("revision") != REVISION
            or manifest.get("tag") != "v5.3.0"
            or manifest.get("repository") != "https://github.com/jrouwe/JoltPhysics.git"
            or manifest.get("patch") != "jolt-startup.patch"):
        raise PatchError("wrong Jolt patch revision")
    patch = read(root / MANIFEST.parent / manifest["patch"])
    if digest(patch) != manifest.get("sha256"):
        raise PatchError("Jolt patch digest mismatch")
    files = manifest.get("files")
    if not isinstance(files, list) or not files or len(files) > 64:
        raise PatchError("invalid edit roster")
    names = []
    for entry in files:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            raise PatchError("invalid edit entry")
        name = entry["path"]
        # ASCII source names have at most 512 bytes and 32 components. These
        # bounds also cap ancestor walks before any filesystem access.
        if len(name) > 512 or name.count("/") > 31:
            raise PatchError("edit path exceeds bound")
        path = PurePosixPath(name)
        if (path.as_posix() != name or not name.startswith("Jolt/") or ".." in path.parts
                or not re.fullmatch(r"[A-Za-z0-9_/]+\.(?:h|inl|cpp)", name)
                or entry.get("mode") != "100644"
                or any(not re.fullmatch(r"[0-9a-f]{64}", str(entry.get(key, "")))
                       for key in ("before", "after"))):
            raise PatchError("invalid edit identity")
        names.append(name)
    if names != sorted(set(names)):
        raise PatchError("duplicate or unsorted edits")
    return manifest, patch


def identity(root: Path = ROOT) -> dict:
    """Offline identity covers upstream revision plus every overlay input."""
    manifest, _ = inputs(root)
    paths = [MANIFEST, MANIFEST.parent / manifest["patch"],
             Path("tools/ci/jolt_startup.py"), Path("tools/ci/path_safety.py"),
             Path("cmake/jolt_startup.cmake"), Path("vendor/CMakeLists.txt")]
    return {"revision": manifest["revision"],
            "inputs": {path.as_posix(): digest(read(root / path)) for path in paths}}


def apply_patch(patch: bytes, originals: dict[str, bytes]) -> dict[str, bytes]:
    lines = patch.decode("utf-8").splitlines(keepends=True)
    result = {}
    index = 0
    while index < len(lines):
        if not lines[index].startswith("--- a/"):
            raise PatchError("missing patch file header")
        name = lines[index][6:].rstrip("\n")
        index += 1
        if index == len(lines) or lines[index] != f"+++ b/{name}\n" or name not in originals or name in result:
            raise PatchError("mismatched patch file")
        index += 1
        old = originals[name].decode("utf-8").splitlines(keepends=True)
        output = []
        position = 0
        while index < len(lines) and lines[index].startswith("@@"):
            match = re.fullmatch(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n", lines[index])
            if match is None:
                raise PatchError("invalid hunk")
            old_start, old_count, new_start, new_count = (int(match[1]), int(match[2] or 1),
                                                           int(match[3]), int(match[4] or 1))
            # Empty ranges name the boundary after that line, including zero
            # before the first line. Nonempty ranges start at a one-based line.
            start = old_start if old_count == 0 else old_start - 1
            new_offset = new_start if new_count == 0 else new_start - 1
            if start < position or start > len(old):
                raise PatchError("overlapping or missing hunk context")
            output.extend(old[position:start])
            if new_offset != len(output):
                raise PatchError("wrong output hunk offset")
            position = start
            consumed = produced = 0
            index += 1
            while index < len(lines) and lines[index][:1] in (" ", "-", "+"):
                if lines[index].startswith("--- a/"):
                    break
                prefix, content = lines[index][0], lines[index][1:]
                if prefix != "+":
                    if position >= len(old) or old[position] != content:
                        raise PatchError("patch context mismatch")
                    position += 1
                    consumed += 1
                if prefix != "-":
                    output.append(content)
                    produced += 1
                index += 1
            if consumed != old_count or produced != new_count:
                raise PatchError("hunk count mismatch")
        output.extend(old[position:])
        result[name] = "".join(output).encode("utf-8")
    if result.keys() != originals.keys():
        raise PatchError("patch and manifest rosters differ")
    return result


def prepare(source: Path, output: Path, root: Path = ROOT) -> None:
    ordinary(source, directory=True)
    source = source.resolve()
    if ".." in output.parts:
        raise PatchError("parent traversal in overlay path")
    output = output.absolute()
    for component in (output, *output.parents):
        if component.exists() or component.is_symlink():
            ordinary(component, directory=True)
    # Resolve existing ancestors before comparing custody, including Windows
    # short-name aliases; the original components were checked for redirects.
    output = output.resolve()
    if output == source or output.is_relative_to(source) or source.is_relative_to(output):
        raise PatchError("overlay and supplied source must be separate")
    manifest, patch = inputs(root)
    originals = {}
    total = 0
    for entry in manifest["files"]:
        content = read(source / entry["path"])
        total += len(content)
        if total > MAX_BYTES:
            raise PatchError("edit payload exceeds bound")
        originals[entry["path"]] = content
    for entry in manifest["files"]:
        content = originals[entry["path"]]
        # Upstream text attributes permit native checkout line endings. Accept
        # only uniform CRLF or LF, then verify the exact reviewed LF identity.
        # Raw file and aggregate bounds above apply before this bounded copy.
        if b"\r" in content:
            pairs = content.count(b"\r\n")
            if content.count(b"\r") != pairs or content.count(b"\n") != pairs:
                raise PatchError(f"nonuniform pinned line endings: {entry['path']}")
            content = content.replace(b"\r\n", b"\n")
        if digest(content) != entry["before"]:
            raise PatchError(f"wrong pinned preimage: {entry['path']}")
        originals[entry["path"]] = content
    patched = apply_patch(patch, originals)
    if sum(map(len, patched.values())) > MAX_BYTES:
        raise PatchError("patched payload exceeds bound")
    for entry in manifest["files"]:
        if digest(patched[entry["path"]]) != entry["after"]:
            raise PatchError(f"wrong patched output: {entry['path']}")
    # Only the manifest's files and their ancestor directories may exist here.
    # Bound the walk by that finite roster; extra headers must never shadow Jolt.
    allowed = set(patched)
    for name in patched:
        allowed.update(parent.as_posix() for parent in PurePosixPath(name).parents
                       if parent != PurePosixPath("."))
    if output.exists() or output.is_symlink():
        ordinary(output, directory=True)
        pending = [output]
        seen = 0
        while pending:
            directory = pending.pop()
            with os.scandir(directory) as children:
                for child in children:
                    seen += 1
                    if seen > len(allowed):
                        raise PatchError("unexpected overlay entries")
                    path = Path(child.path)
                    name = path.relative_to(output).as_posix()
                    if name not in allowed:
                        raise PatchError("unexpected overlay entry")
                    is_directory = name not in patched
                    ordinary(path, directory=is_directory)
                    if is_directory:
                        pending.append(path)
    # Validate every existing output before creating anything. Unknown content
    # is refused; repeat configuration never overwrites supplied files.
    for name, content in patched.items():
        target = output / name
        for parent in (target, *target.parents):
            if parent.exists() or parent.is_symlink():
                ordinary(parent, directory=parent != target)
        if target.exists() and read(target) != content:
            raise PatchError(f"overlay content mismatch: {name}")
    for name, content in patched.items():
        target = output / name
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as handle:
                if handle.write(content) != len(content):
                    raise PatchError("short overlay write")
                handle.flush()
                os.fsync(handle.fileno())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        prepare(args.source, args.output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Jolt startup patch refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
