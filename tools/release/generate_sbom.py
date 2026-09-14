#!/usr/bin/env python3
"""Generate a compact SPDX 2.3 file inventory, not a dependency/license audit."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import BinaryIO
from urllib.parse import quote


def valid_path(name: str) -> str:
    """Reject traversal, ambiguous separators, and control characters."""
    if (not name or any(part in ("", ".", "..") for part in name.split("/"))
            or "\\" in name or ":" in name
            or any(ord(char) < 32 or ord(char) == 127 for char in name)):
        raise ValueError(f"unsupported package path: {name!r}")
    name.encode("utf-8", "strict")
    return name


def checksums(stream: BinaryIO) -> list[dict]:
    sha1 = hashlib.sha1(usedforsecurity=False)
    sha256 = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        sha1.update(chunk)
        sha256.update(chunk)
    return [{"algorithm": "SHA1", "checksumValue": sha1.hexdigest()},
            {"algorithm": "SHA256", "checksumValue": sha256.hexdigest()}]


def is_link(info: os.stat_result) -> bool:
    # Windows junctions are directories with reparse attributes, not symlinks.
    return stat.S_ISLNK(info.st_mode) or bool(
        (getattr(info, "st_file_attributes", 0) or 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def archive_inventory(path: Path) -> dict[str, list[dict]]:
    """Read regular tar members without extracting or stripping the prefix."""
    inventory = {}
    seen = set()
    directories = set()
    with tarfile.open(path, "r:*") as archive:
        for member in archive:
            name = valid_path(member.name.removesuffix("/") if member.isdir() else member.name)
            if name in seen:
                raise ValueError(f"duplicate archive path: {name}")
            seen.add(name)
            parts = name.split("/")
            parents = {"/".join(parts[:index]) for index in range(1, len(parts))}
            if parents.intersection(inventory) or (not member.isdir() and name in directories):
                raise ValueError(f"file/directory archive path collision: {name}")
            directories.update(parents)
            if member.isdir():
                directories.add(name)
                continue
            if not member.isfile() or member.sparse is not None:
                raise ValueError(f"unsupported archive entry: {name} (regular files only)")
            with archive.extractfile(member) as stream:
                inventory[name] = checksums(stream)
    return dict(sorted(inventory.items()))


def files_for(root: Path, tracked: bool) -> list[Path]:
    if not root.is_dir():
        raise ValueError(f"not a package directory: {root}")
    paths = []
    if tracked:
        top = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--show-toplevel"], text=True).strip()
        if Path(top).resolve() != root:
            raise ValueError("--tracked requires the repository root")
        output = subprocess.check_output(["git", "-C", str(root), "ls-files", "--stage", "-z"])
        for entry in output.split(b"\0"):
            if not entry:
                continue
            metadata, name = entry.split(b"\t", 1)
            mode, _, stage = metadata.split()
            if mode not in (b"100644", b"100755") or stage != b"0":
                raise ValueError(f"unsupported tracked entry (link or conflict): {name!r}")
            paths.append(root / valid_path(name.decode("utf-8")))
    else:
        def fail(error: OSError) -> None:
            raise error

        for directory, dirs, files in os.walk(root, onerror=fail, followlinks=False):
            for name in dirs + files:
                path = Path(directory) / name
                valid_path(path.relative_to(root).as_posix())
                info = path.lstat()
                if is_link(info):
                    raise ValueError(f"unsupported package entry: {path}")
                if stat.S_ISDIR(info.st_mode):
                    continue
                if not stat.S_ISREG(info.st_mode):
                    raise ValueError(f"unsupported package entry: {path}")
                paths.append(path)
    for path in paths:
        # A tracked regular file can have been replaced by a symlink locally.
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or is_link(info) or any(
                is_link((root / parent).lstat()) for parent in path.relative_to(root).parents):
            raise ValueError(f"unsupported package entry: {path}")
    return sorted(paths)


def root_inventory(root: Path, tracked: bool, output: Path) -> dict[str, list[dict]]:
    root, output = root.resolve(), output.resolve()
    if not tracked and output.is_relative_to(root):
        raise ValueError("SBOM output must be outside the package directory")
    inventory = {}
    for path in files_for(root, tracked):
        if path.resolve() == output or (output.exists() and path.samefile(output)):
            raise ValueError("SBOM output must not replace an inventoried file")
        with path.open("rb") as stream:
            inventory[path.relative_to(root).as_posix()] = checksums(stream)
    return inventory


def validate_created(value: str) -> str:
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", value):
        raise ValueError("created must have UTC format YYYY-MM-DDThh:mm:ssZ")
    dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    return value


def creation_time(explicit: str | None) -> tuple[str, str]:
    if explicit is not None:
        return validate_created(explicit), "Creation timestamp explicitly supplied with --created."
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is not None:
        if not re.fullmatch(r"[0-9]+", epoch):
            raise ValueError("SOURCE_DATE_EPOCH must be a nonnegative integer")
        value = dt.datetime.fromtimestamp(int(epoch), dt.timezone.utc)
        return value.isoformat(timespec="seconds").replace("+00:00", "Z"), (
            "Reproducible creation timestamp from SOURCE_DATE_EPOCH; the release workflow "
            "uses the tagged commit's committer timestamp, not the build wall clock.")
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), (
        "Creation timestamp is the generation wall clock in UTC.")


def document_namespace(document: dict) -> str:
    payload = {key: value for key, value in document.items() if key != "documentNamespace"}
    digest = hashlib.sha256(json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")).hexdigest()
    return f"https://github.com/d-addison/luminumbra/sbom/{quote(document['name'], safe='')}/{digest}"


def make_document(inventory: dict[str, list[dict]], name: str, version: str,
                  created: str, time_comment: str) -> dict:
    if any(not value.strip() or any(ord(char) < 32 for char in value) for value in (name, version)):
        raise ValueError("name and version must be nonempty single-line values")
    validate_created(created)
    spdx_files = [{
        "SPDXID": f"SPDXRef-File-{index}",
        "fileName": f"./{valid_path(path)}",
        "checksums": inventory[path],
        "licenseConcluded": "NOASSERTION",
        "licenseInfoInFiles": ["NOASSERTION"],
        "copyrightText": "NOASSERTION",
    } for index, path in enumerate(sorted(inventory), 1)]
    file_sha1s = [checksum["checksumValue"] for item in spdx_files for checksum in item["checksums"]
                  if checksum["algorithm"] == "SHA1"]
    verification = hashlib.sha1("".join(sorted(file_sha1s)).encode("ascii"),
                                usedforsecurity=False).hexdigest()
    document = {
        "spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{name}-{version}",
        "creationInfo": {"creators": ["Tool: luminumbra-generate-sbom-2"], "created": created,
                         "comment": time_comment},
        "comment": "File inventory of the supplied package bytes. No dependency resolution or "
                   "per-file license audit was performed. MIT is the project's declared license; "
                   "bundled third-party files retain their own licenses.",
        "packages": [{
            "name": name, "SPDXID": "SPDXRef-Package", "versionInfo": version,
            "downloadLocation": "NOASSERTION", "filesAnalyzed": True,
            "packageVerificationCode": {"packageVerificationCodeValue": verification},
            "licenseConcluded": "NOASSERTION", "licenseDeclared": "MIT",
            "licenseInfoFromFiles": ["NOASSERTION"], "copyrightText": "NOASSERTION",
        }],
        "files": spdx_files,
        "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                           "relatedSpdxElement": "SPDXRef-Package"}] + [
            {"spdxElementId": "SPDXRef-Package", "relationshipType": "CONTAINS",
             "relatedSpdxElement": item["SPDXID"]} for item in spdx_files],
    }
    document["documentNamespace"] = document_namespace(document)
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--root", type=Path, help="inventory a staged directory's bytes")
    source.add_argument("--archive", type=Path, help="inventory exact tar/tar.gz members")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--tracked", action="store_true", help="restrict --root to tracked checkout files")
    parser.add_argument("--created", help="UTC timestamp; otherwise SOURCE_DATE_EPOCH or current UTC")
    args = parser.parse_args()
    try:
        if args.tracked and args.root is None:
            raise ValueError("--tracked requires --root")
        if args.output.is_symlink():
            raise ValueError("SBOM output must not be a symlink")
        if args.archive and (args.archive.resolve() == args.output.resolve()
                             or (args.output.exists() and args.archive.samefile(args.output))):
            raise ValueError("SBOM output must not replace the archive")
        created, time_comment = creation_time(args.created)
        inventory = (archive_inventory(args.archive) if args.archive
                     else root_inventory(args.root, args.tracked, args.output))
        document = make_document(inventory, args.name, args.version, created, time_comment)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8", newline="\n")
    except (OSError, ValueError, OverflowError, subprocess.SubprocessError, tarfile.TarError) as exc:
        print(f"SBOM: FAIL - {exc}", file=sys.stderr)
        return 1
    print(f"SBOM: {len(inventory)} files -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
