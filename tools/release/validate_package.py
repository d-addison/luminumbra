#!/usr/bin/env python3
"""Reject audio, local state, and restricted paths in release archives."""

from __future__ import annotations

import argparse
import re
import tarfile
import zipfile
from pathlib import Path


FORBIDDEN = re.compile(
    r"(?:^|/)(?:\.banso|references|assets/audio|res/audio|vendor/steamworks)(?:/|$)|"
    r"\.(?:aac|flac|m4a|mp3|ogg|opus|wav|wma)$",
    re.IGNORECASE,
)


def names(path: Path) -> list[str]:
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            return archive.namelist()
    if tarfile.is_tarfile(path):
        with tarfile.open(path) as archive:
            return archive.getnames()
    raise ValueError(f"unsupported archive: {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archives", type=Path, nargs="+")
    args = parser.parse_args()
    failed = False
    for archive in args.archives:
        offenders = [name for name in names(archive) if FORBIDDEN.search(name.replace("\\", "/"))]
        if offenders:
            failed = True
            for name in offenders:
                print(f"{archive}: forbidden package entry: {name}")
    if not failed:
        print("release package validation: PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
