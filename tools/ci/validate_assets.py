#!/usr/bin/env python3
"""Classify every tracked runtime/documentation/test asset and verify its hash inventory."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


ROOTS = ("data/", "docs/assets/", "test/fixtures/", "worlds/")
RULES = (
    ("data/fonts/Lora/", "OFL-1.1", "Lora"),
    ("data/textures/terrain/soil/", "CC0-1.0", "ambientCG Ground048"),
    ("data/textures/terrain/grass/", "CC0-1.0", "ambientCG Grass003"),
    ("data/textures/terrain/rock/", "CC0-1.0", "ambientCG Rock028"),
    ("data/textures/terrain/sand/", "CC0-1.0", "ambientCG Ground087"),
    ("data/textures/terrain/deepslate/", "CC0-1.0", "ambientCG Gravel040"),
    ("test/fixtures/dem/", "LicenseRef-Public-Domain", "documented DEM fixtures"),
    ("data/", "MIT", "Luminumbra first-party runtime data"),
    ("docs/assets/", "MIT", "Luminumbra documentation assets"),
    ("test/fixtures/", "MIT", "Luminumbra test fixtures"),
    ("worlds/", "MIT", "Luminumbra authored worlds"),
)


def tracked(root: Path) -> list[str]:
    output = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
    return sorted(
        item.decode("utf-8", "surrogateescape")
        for item in output.split(b"\0")
        if item and item.decode("utf-8", "surrogateescape").startswith(ROOTS)
    )


def classify(path: str) -> tuple[str, str] | None:
    for prefix, license_id, source in RULES:
        if path.startswith(prefix):
            return license_id, source
    return None


def build_inventory(root: Path) -> dict:
    files = []
    for relative in tracked(root):
        classification = classify(relative)
        if classification is None:
            raise ValueError(f"unclassified asset: {relative}")
        license_id, source = classification
        data = (root / relative).read_bytes()
        files.append(
            {
                "path": relative,
                "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": len(data),
                "license": license_id,
                "source": source,
            }
        )
    return {"schema": "luminumbra.public_asset_inventory.v1", "files": files}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--write", type=Path)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        inventory = build_inventory(root)
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"asset inventory: FAIL - {exc}", file=sys.stderr)
        return 1
    rendered = json.dumps(inventory, indent=2, sort_keys=True) + "\n"
    if args.write:
        args.write.parent.mkdir(parents=True, exist_ok=True)
        args.write.write_text(rendered, encoding="utf-8")
    if args.check:
        try:
            expected = args.check.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"asset inventory: FAIL - {exc}", file=sys.stderr)
            return 1
        if expected != rendered:
            print("asset inventory: FAIL - committed inventory is stale", file=sys.stderr)
            return 1
    print(f"asset inventory: PASS - {len(inventory['files'])} classified files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
