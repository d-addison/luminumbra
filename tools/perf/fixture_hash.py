#!/usr/bin/env python3
"""Canonical fixture hash for the performance comparison gate.

The gate refuses to compare two runs whose `comparability_key` differs, and the
fixture hash is one of that key's inputs (see `canonical_identity` in perf.py).
The intent is sound: if the workload's input data changed, base and candidate did
not measure the same thing and a delta between them is not evidence of anything.

Hashing the raw `data/` tree implements that intent too bluntly. The tree hash
changes for edits that provably cannot reach the workload -- most obviously the
`_`-prefixed comment fields the JSON files under `data/` use as documentation.
Editing one of those made every comparison report `unevaluated`, which the gate
then failed, so a documentation-only change could not pass a required check.

This canonicalises before hashing:

  * JSON files are parsed, `_`-prefixed keys are dropped at every level, and the
    result is re-serialised with sorted keys and no incidental whitespace. So
    comment edits, key reordering and reindentation do not move the hash, while
    any change to a value the engine reads does.
  * Every other file is hashed byte for byte. Textures, fonts, meshes and markup
    have no comment convention to strip and are compared exactly.

A file whose suffix is `.json` but which does not parse is hashed byte for byte
rather than skipped: a malformed fixture must still be visible to the gate.

By default every file is included. The explicit `headless-server` scope accepts
`data/common` as its root and excludes only the client grass scatter and rain/snow emitter descriptors.
These are loaded by client render/preview paths, never by the server workloads.
Simulation species and other foliage data remain part of the hash. Both sides
of a comparison must use the same scope.

Authored biome table references use the portable containment resolver. Their
transitive dependencies outside the included tree extend the receipt with a
versioned, name-sorted JSON list of canonical digests (null for missing files).
Trees whose dependencies are already included retain their original digest.

Usage:  fixture_hash.py <data-root> [--scope all|headless-server]
Prints the hex digest on stdout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import stat
import sys
from pathlib import Path
from typing import Any, Iterator

from fixture_paths import contain_path, resolve_reference

# JSON object keys beginning with this prefix are documentation for humans, not
# input to the engine. `data/common/systems.json` uses the convention heavily
# (`_water_high_res_comment` and friends).
COMMENT_KEY_PREFIX = "_"
SCOPES = ("all", "headless-server")
SERVER_RENDER_ONLY_FILES = frozenset({
    "foliage/scatter_set.json",
    "particles/precip_rain.json",
    "particles/precip_snow.json",
})


def strip_comment_keys(value: Any) -> Any:
    """Recursively drop `_`-prefixed object keys."""
    if isinstance(value, dict):
        return {
            key: strip_comment_keys(item)
            for key, item in value.items()
            if not (isinstance(key, str) and key.startswith(COMMENT_KEY_PREFIX))
        }
    if isinstance(value, list):
        return [strip_comment_keys(item) for item in value]
    return value


def canonical_file_digest(path: Path, *, reference_root: Path | None = None,
                          dependencies: dict[str, Path] | None = None) -> str:
    """Digest one fixture file, canonicalising JSON and hashing anything else raw."""
    raw = path.read_bytes()
    if path.suffix.lower() == ".json":
        try:
            # utf-8-sig: several fixtures carry a byte-order mark.
            parsed = json.loads(raw.decode("utf-8-sig"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            # Malformed JSON still has to register as a change.
            return hashlib.sha256(raw).hexdigest()
        if reference_root is not None and isinstance(parsed, dict):
            generation = parsed.get("generation_params")
            biomes = generation.get("biomes") if isinstance(generation, dict) else None
            table = biomes.get("table") if isinstance(biomes, dict) else None
            # An empty table disables biomes. Malformed documents still hash;
            # only authored external path strings need containment validation.
            if isinstance(table, str) and table:
                dependency = resolve_reference(reference_root, table)
                if dependencies is not None:
                    name = dependency.relative_to(reference_root.resolve()).as_posix()
                    dependencies[name] = dependency
        canonical = json.dumps(
            strip_comment_keys(parsed), sort_keys=True, separators=(",", ":")
        )
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return hashlib.sha256(raw).hexdigest()


def fixture_files(root: Path) -> Iterator[tuple[Path, Path]]:
    """Check containment before reading files or descending into directories."""
    pending = [root]
    while pending:
        for path in pending.pop().iterdir():
            contained = contain_path(root, path)
            status = contained.lstat()
            if stat.S_ISREG(status.st_mode) or contained.is_file():
                yield path, contained
            elif (stat.S_ISDIR(status.st_mode)
                  and not (getattr(status, "st_file_attributes", 0) & 0x400)):
                # Match rglob's exclusion of linked directories. The reparse
                # bit covers Windows junctions too; use lstat, not DirEntry.stat.
                pending.append(path)


def fixture_hash(root: Path, *, scope: str = "all", reference_root: Path | None = None,
                 dependencies: dict[str, Path] | None = None) -> str:
    """Digest a tree plus referenced dependencies not already included in it.

    Optional dependencies receives the transitive, normalized root-relative
    dependency paths, including missing files, for fixture copying consumers.
    """
    if scope not in SCOPES:
        raise ValueError(f"unknown fixture scope: {scope}")
    digest = hashlib.sha256()
    reference_root = reference_root if reference_root is not None else root
    referenced: dict[str, Path] = {}
    hashed: set[Path] = set()
    if scope != "all":
        # Separate scoped identities from the existing full-tree hash format.
        digest.update(f"fixture-scope:{scope}\0".encode("ascii"))
    # Native Windows Path ordering folds case; the receipt order must not.
    for path, contained in sorted(fixture_files(root), key=lambda item: item[0].relative_to(root).parts):
        relative = path.relative_to(root).as_posix()
        if scope == "headless-server" and relative in SERVER_RENDER_ONLY_FILES:
            continue
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        file_digest = canonical_file_digest(contained, reference_root=reference_root,
                                            dependencies=referenced)
        hashed.add(contained)
        digest.update(file_digest.encode("ascii"))
        digest.update(b"\n")
    # Preserve the tree receipt byte-for-byte when every dependency is already
    # included. The extension uses JSON framing to bind names, contents and
    # missing-file state without delimiter ambiguity, including unusual names.
    extra: dict[str, str | None] = {}
    pending = list(referenced.items())
    visited: set[str] = set()
    while pending:
        name, path = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        if path not in hashed:
            if path.exists():
                discovered: dict[str, Path] = {}
                extra[name] = canonical_file_digest(path, reference_root=reference_root,
                                                     dependencies=discovered)
                referenced.update(discovered)
                pending.extend(discovered.items())
            else:
                extra[name] = None
    if dependencies is not None:
        dependencies.update(referenced)
    if extra:
        digest.update(b"\0fixture-dependencies:v1\0")
        digest.update(json.dumps(sorted(extra.items()), ensure_ascii=True,
                                 separators=(",", ":")).encode("ascii"))
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_root", type=Path)
    parser.add_argument("--reference-root", type=Path,
                        help="data root for preset references (defaults to the hashed root)")
    parser.add_argument(
        "--scope", choices=SCOPES, default="all",
        help="headless-server expects data/common and omits only client grass/rain/snow rendering data",
    )
    args = parser.parse_args(argv[1:])
    root = args.data_root
    if not root.is_dir():
        print(f"fixture root is not a directory: {root}", file=sys.stderr)
        return 2
    try:
        result = fixture_hash(root, scope=args.scope, reference_root=args.reference_root)
    except (OSError, ValueError) as exc:
        print(f"fixture refused: {exc}", file=sys.stderr)
        return 2
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
