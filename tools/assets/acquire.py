#!/usr/bin/env python3
"""Acquire a pinned game asset pack, verify every byte, then install it atomically."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tarfile
import tempfile
import time
import urllib.request
import uuid


MAX_PACK_BYTES = 512 * 1024 * 1024
MAX_FILES = 256


def is_redirect(path: Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return (stat.S_ISLNK(info.st_mode) or
            bool(getattr(info, "st_file_attributes", 0) &
                 getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)))


def relative_path(value: str) -> Path:
    path = PurePosixPath(value)
    if (not value or path.is_absolute() or str(path) != value or
            any(part in {"", ".", ".."} for part in value.split("/")) or
            "\\" in value or ":" in value or "\x00" in value):
        raise ValueError(f"Unsafe asset path: {value!r}")
    reserved = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(10)),
                *(f"LPT{i}" for i in range(10))}
    if any(not re.fullmatch(r"[A-Za-z0-9_.-]{1,255}", part) or part.endswith(".") or
           part.split(".")[0].upper() in reserved for part in path.parts):
        raise ValueError(f"Non-portable asset path: {value!r}")
    return Path(*path.parts)


def checked_path(root: Path, relative: str) -> Path:
    path = root
    for part in relative_path(relative).parts:
        path /= part
        if is_redirect(path):
            raise ValueError(f"Asset paths cannot traverse a symbolic link or reparse point: {path}")
    return path


def validate_identity(item: dict) -> None:
    if (type(item.get("size")) is not int or not 0 < item["size"] <= MAX_PACK_BYTES or
            not re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", ""))):
        raise ValueError("Invalid asset size or SHA-256 in manifest")


def validate_pack(pack: dict) -> dict[str, dict]:
    relative_path(pack["install_dir"])
    archive = pack["archive"]
    validate_identity(archive)
    if not archive["url"].startswith("https://"):
        raise ValueError("Asset archive URL must use HTTPS")
    expanded = archive["uncompressed_size"]
    if type(expanded) is not int or not 0 < expanded <= MAX_PACK_BYTES:
        raise ValueError("Invalid uncompressed archive limit")
    entries = pack["files"]
    if not isinstance(entries, list) or not 0 < len(entries) <= MAX_FILES:
        raise ValueError("Invalid required-file count")
    files = {}
    for item in entries:
        relative_path(item["path"])
        validate_identity(item)
        if item["path"].casefold() in {name.casefold() for name in files}:
            raise ValueError("Duplicate asset path")
        files[item["path"]] = item
    if sum(item["size"] for item in files.values()) > expanded:
        raise ValueError("Required files exceed the archive limit")
    return files


def matches(path: Path, identity: dict) -> bool:
    if is_redirect(path) or not path.is_file() or path.stat().st_size != identity["size"]:
        return False
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(128 * 1024), b""):
            digest.update(block)
    return digest.hexdigest() == identity["sha256"]


def verify_install(root: Path, files: dict[str, dict]) -> None:
    for name, identity in files.items():
        path = checked_path(root, name)
        if not matches(path, identity):
            raise ValueError(f"Missing or corrupt required asset: {path}")
    for path in root.rglob("*"):
        if is_redirect(path) or (not path.is_dir() and path.relative_to(root).as_posix() not in files):
            raise ValueError(f"Unexpected installed asset: {path}")


def copy_verified(stream, target: Path, identity: dict) -> None:
    remaining = identity["size"]
    digest = hashlib.sha256()
    started = time.monotonic()
    with target.open("xb") as output:
        while remaining:
            if time.monotonic() - started > 120:
                raise ValueError("Asset transfer exceeded its 120-second deadline")
            block = stream.read(min(remaining, 128 * 1024))
            if not block:
                raise ValueError("Asset transfer ended early")
            output.write(block)
            digest.update(block)
            remaining -= len(block)
        if stream.read(1):
            raise ValueError("Asset transfer exceeds its declared size")
        if digest.hexdigest() != identity["sha256"]:
            raise ValueError("Asset SHA-256 mismatch")
        output.flush()
        os.fsync(output.fileno())


def acquire_archive(pack: dict, cache: Path, offline: bool, local: Path | None) -> Path:
    archive = pack["archive"]
    cache.mkdir(parents=True, exist_ok=True)
    destination = checked_path(cache, archive["sha256"] + ".tar.gz")
    if matches(destination, archive):
        return destination
    if local is None and offline:
        raise ValueError(f"Verified offline archive unavailable: {destination}")
    temporary = cache / (".download-" + uuid.uuid4().hex)
    try:
        if local is not None:
            with local.open("rb") as stream:
                copy_verified(stream, temporary, archive)
        else:
            request = urllib.request.Request(archive["url"], headers={"User-Agent": "Luminumbra-assets/1"})
            with urllib.request.urlopen(request, timeout=30) as stream:
                if not stream.geturl().startswith("https://"):
                    raise ValueError("Asset download redirected away from HTTPS")
                copy_verified(stream, temporary, archive)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


class BoundedReader(io.RawIOBase):
    def __init__(self, source, limit: int):
        self.source = source
        self.remaining = limit

    def read(self, size=-1):
        count = self.remaining + 1 if size < 0 else min(size, self.remaining + 1)
        data = self.source.read(count)
        self.remaining -= len(data)
        if self.remaining < 0:
            raise ValueError("Decompressed archive exceeds the manifest limit")
        return data


def extract_verified(archive: Path, staging: Path, pack: dict, files: dict[str, dict]) -> None:
    seen = set()
    with gzip.open(archive, "rb") as compressed:
        bounded = BoundedReader(compressed, pack["archive"]["uncompressed_size"])
        with tarfile.open(fileobj=bounded, mode="r|") as source:
            for member in source:
                if (not member.isfile() or member.name not in files or member.name in seen or
                        member.size != files[member.name]["size"] or member.pax_headers):
                    raise ValueError(f"Unexpected archive member: {member.name}")
                target = checked_path(staging, member.name)
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as stream:
                    copy_verified(stream, target, files[member.name])
                seen.add(member.name)
        # Consume the gzip trailer and any padding under the same expansion bound.
        while bounded.read(128 * 1024):
            pass
        if bounded.remaining != 0:
            raise ValueError("Decompressed archive size differs from the manifest")
    if seen != files.keys():
        raise ValueError("Archive is missing required files")
    verify_install(staging, files)


def install(pack: dict, root: Path, cache: Path, *, offline=False, local=None, repair=False) -> Path:
    files = validate_pack(pack)
    destination = checked_path(root.resolve(), pack["install_dir"])
    if destination.exists():
        try:
            verify_install(destination, files)
            return destination
        except ValueError:
            if not repair:
                raise ValueError(f"Corrupt installation at {destination}; rerun with --repair to preserve it and install verified content") from None
    archive = acquire_archive(pack, cache.resolve(), offline, local)
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".install-", dir=destination.parent))
    previous = None
    try:
        extract_verified(archive, staging, pack, files)
        if destination.exists():
            previous = destination.with_name(destination.name + ".corrupt-" + uuid.uuid4().hex)
            os.replace(destination, previous)
        try:
            os.replace(staging, destination)
        except BaseException:
            if previous is not None:
                os.replace(previous, destination)
            raise
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path("config/game-asset-packs.json"))
    parser.add_argument("--pack", default="tree-small-02-runtime")
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--cache", type=Path, default=Path(".asset-cache"))
    parser.add_argument("--archive", type=Path, help="Verify and reuse a local archive")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--repair", action="store_true")
    args = parser.parse_args()
    try:
        if args.manifest.stat().st_size > 1024 * 1024:
            raise ValueError("Asset manifest exceeds 1 MiB")
        manifest = json.loads(args.manifest.read_text())
        if manifest["schema"] != "luminumbra.game.asset-packs.v1":
            raise ValueError("Unsupported asset manifest schema")
        pack = manifest["packs"][args.pack]
        destination = install(pack, args.root, args.cache, offline=args.offline,
                              local=args.archive, repair=args.repair)
        print(f"Verified {args.pack} {pack['version']}: {destination}")
        return 0
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError) as error:
        print(f"Asset setup failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
