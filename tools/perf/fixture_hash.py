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

Usage:  fixture_hash.py <data-root>
Prints the hex digest on stdout.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
from typing import Any

# JSON object keys beginning with this prefix are documentation for humans, not
# input to the engine. `data/common/systems.json` uses the convention heavily
# (`_water_high_res_comment` and friends).
COMMENT_KEY_PREFIX = "_"


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


def canonical_file_digest(path: Path) -> str:
    """Digest one fixture file, canonicalising JSON and hashing anything else raw."""
    raw = path.read_bytes()
    if path.suffix.lower() == ".json":
        try:
            # utf-8-sig: several fixtures carry a byte-order mark.
            parsed = json.loads(raw.decode("utf-8-sig"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            # Malformed JSON still has to register as a change.
            return hashlib.sha256(raw).hexdigest()
        canonical = json.dumps(
            strip_comment_keys(parsed), sort_keys=True, separators=(",", ":")
        )
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return hashlib.sha256(raw).hexdigest()


def fixture_hash(root: Path) -> str:
    """Digest a fixture tree. Path-ordered, so the result is reproducible."""
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        relative = path.relative_to(root).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(canonical_file_digest(path).encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {Path(argv[0]).name} <data-root>", file=sys.stderr)
        return 2
    root = Path(argv[1])
    if not root.is_dir():
        print(f"fixture root is not a directory: {root}", file=sys.stderr)
        return 2
    print(fixture_hash(root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
