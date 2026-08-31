#!/usr/bin/env python3
"""Verify cmake/runtime_data_manifest.cmake matches tracked data/ files."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "cmake" / "runtime_data_manifest.cmake"
ENTRY_RE = re.compile(r'^\s*"([^"]+)"\s*$')


def tracked_data_files() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "data"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return sorted(
        line.removeprefix("data/")
        for line in result.stdout.splitlines()
        if line.strip()
    )


def manifest_files() -> list[str]:
    entries: list[str] = []
    for line in MANIFEST.read_text(encoding="utf-8-sig").splitlines():
        match = ENTRY_RE.match(line)
        if match:
            entries.append(match.group(1))
    return entries


def main() -> int:
    tracked = tracked_data_files()
    manifest = manifest_files()
    manifest_set = set(manifest)
    missing = sorted(set(tracked) - manifest_set)
    stale = sorted(manifest_set - set(tracked))
    duplicates = sorted({entry for entry in manifest if manifest.count(entry) > 1})
    absent = sorted(entry for entry in manifest_set if not (ROOT / "data" / entry).is_file())

    if missing or stale or duplicates or absent:
        if missing:
            print("Tracked data files missing from the manifest:")
            print("\n".join(f"  data/{path}" for path in missing))
        if stale:
            print("Manifest entries no longer tracked:")
            print("\n".join(f"  data/{path}" for path in stale))
        if duplicates:
            print("Duplicate manifest entries:")
            print("\n".join(f"  {path}" for path in duplicates))
        if absent:
            print("Manifest entries absent from the working tree:")
            print("\n".join(f"  data/{path}" for path in absent))
        return 1

    print(f"OK: runtime-data manifest matches {len(tracked)} tracked data files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
