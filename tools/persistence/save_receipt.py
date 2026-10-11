#!/usr/bin/env python3
"""Snapshot, verify and diff saved-world directory trees by SHA-256.

Commands:
  snapshot <world_dir> --out receipt.json   write a receipt for the tree
  verify <world_dir> <receipt.json>         exit 0 if the tree matches the receipt
  diff <a.json> <b.json>                    list added/removed/changed paths

Receipt schema "luminumbra.save_receipt.v1":
  {schema, file_count, total_bytes, tree_digest,
   files: [{path, size, sha256}]}  (files sorted by posix path)
tree_digest is the sha256 hex of, for each file in sorted order, the UTF-8 bytes of
f"{path}\\0{size}\\0{sha256}\\n".

Exit codes: 0 ok/equal, 1 mismatch or differences, 2 usage or invalid input.
Symlinks and non-regular files anywhere in the tree are invalid input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path

SCHEMA = "luminumbra.save_receipt.v1"
EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_INVALID = 2
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class ReceiptError(Exception):
    """Invalid input: missing tree, unsupported entry, or malformed receipt."""


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _walk(base: Path, prefix: str, out: list) -> None:
    with os.scandir(base) as iterator:
        entries = sorted(iterator, key=lambda entry: entry.name)
    for entry in entries:
        rel = f"{prefix}{entry.name}"
        if entry.is_symlink():
            raise ReceiptError(f"symlink not allowed in save tree: {rel}")
        if entry.is_dir(follow_symlinks=False):
            _walk(Path(entry.path), rel + "/", out)
        elif entry.is_file(follow_symlinks=False):
            out.append((rel, Path(entry.path)))
        else:
            raise ReceiptError(f"non-regular file not allowed in save tree: {rel}")


def _tree_digest(files: list) -> str:
    digest = hashlib.sha256()
    for item in files:
        line = f"{item['path']}\0{item['size']}\0{item['sha256']}\n"
        digest.update(line.encode("utf-8"))
    return digest.hexdigest()


def snapshot(world_dir) -> dict:
    """Return the receipt dict for the regular-file tree beneath world_dir."""
    root = Path(world_dir)
    if root.is_symlink() or not root.is_dir():
        raise ReceiptError(f"not a directory: {world_dir}")
    found: list = []
    _walk(root, "", found)
    found.sort(key=lambda item: item[0])
    files = []
    for rel, path in found:
        files.append(
            {"path": rel, "size": path.stat().st_size, "sha256": _sha256_file(path)}
        )
    return {
        "schema": SCHEMA,
        "file_count": len(files),
        "total_bytes": sum(item["size"] for item in files),
        "tree_digest": _tree_digest(files),
        "files": files,
    }


def _valid_relative_path(path) -> bool:
    if not isinstance(path, str) or not path or path.startswith("/") or "\\" in path:
        return False
    return all(part not in ("", ".", "..") for part in path.split("/"))


def load_receipt(receipt_path) -> dict:
    """Parse and check a receipt, including its internal count, total and digest."""
    try:
        with open(receipt_path, "r", encoding="utf-8") as handle:
            receipt = json.load(handle)
    except OSError as error:
        raise ReceiptError(f"cannot read receipt {receipt_path}: {error}") from error
    except ValueError as error:
        raise ReceiptError(f"malformed receipt {receipt_path}: {error}") from error
    if not isinstance(receipt, dict):
        raise ReceiptError("malformed receipt: top level is not an object")
    if receipt.get("schema") != SCHEMA:
        raise ReceiptError(f"unsupported receipt schema: {receipt.get('schema')!r}")
    files = receipt.get("files")
    if not isinstance(files, list):
        raise ReceiptError("malformed receipt: files is not a list")
    previous = None
    for item in files:
        if not isinstance(item, dict):
            raise ReceiptError("malformed receipt: file entry is not an object")
        path = item.get("path")
        size = item.get("size")
        sha = item.get("sha256")
        if not _valid_relative_path(path):
            raise ReceiptError(f"malformed receipt: invalid path {path!r}")
        if previous is not None and path <= previous:
            raise ReceiptError(f"malformed receipt: files not strictly sorted at {path!r}")
        previous = path
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise ReceiptError(f"malformed receipt: invalid size for {path!r}")
        if not isinstance(sha, str) or not _SHA256_RE.match(sha):
            raise ReceiptError(f"malformed receipt: invalid sha256 for {path!r}")
    if receipt.get("file_count") != len(files):
        raise ReceiptError("malformed receipt: file_count does not match files")
    if receipt.get("total_bytes") != sum(item["size"] for item in files):
        raise ReceiptError("malformed receipt: total_bytes does not match files")
    if receipt.get("tree_digest") != _tree_digest(files):
        raise ReceiptError("malformed receipt: tree_digest does not match files")
    return receipt


def compare(expected: dict, actual: dict) -> dict:
    """Return {added, removed, changed} path lists going from expected to actual."""
    before = {item["path"]: item for item in expected["files"]}
    after = {item["path"]: item for item in actual["files"]}
    added = sorted(path for path in after if path not in before)
    removed = sorted(path for path in before if path not in after)
    changed = sorted(
        path
        for path in before
        if path in after
        and (before[path]["size"] != after[path]["size"]
             or before[path]["sha256"] != after[path]["sha256"])
    )
    return {"added": added, "removed": removed, "changed": changed}


def verify(world_dir, receipt_path) -> dict:
    """Compare the tree at world_dir with the receipt; empty lists mean a match."""
    return compare(load_receipt(receipt_path), snapshot(world_dir))


def diff(a_path, b_path) -> dict:
    """Compare two receipts; added is in b only, removed is in a only."""
    return compare(load_receipt(a_path), load_receipt(b_path))


def _print_differences(result: dict) -> None:
    for key in ("added", "removed", "changed"):
        for path in result[key]:
            print(f"{key}: {path}")


def _has_differences(result: dict) -> bool:
    return any(result[key] for key in ("added", "removed", "changed"))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Save tree receipts (sha256).")
    commands = parser.add_subparsers(dest="command", required=True)
    snap = commands.add_parser("snapshot", help="write a receipt for a tree")
    snap.add_argument("world_dir")
    snap.add_argument("--out", required=True)
    check = commands.add_parser("verify", help="check a tree against a receipt")
    check.add_argument("world_dir")
    check.add_argument("receipt")
    compare_parser = commands.add_parser("diff", help="compare two receipts")
    compare_parser.add_argument("a")
    compare_parser.add_argument("b")
    args = parser.parse_args(argv)
    try:
        if args.command == "snapshot":
            receipt = snapshot(args.world_dir)
            with open(args.out, "w", encoding="utf-8") as handle:
                json.dump(receipt, handle, indent=2)
                handle.write("\n")
            print(f"snapshot ok: {receipt['file_count']} files, {receipt['tree_digest']}")
            return EXIT_OK
        if args.command == "verify":
            result = verify(args.world_dir, args.receipt)
        else:
            result = diff(args.a, args.b)
    except ReceiptError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_INVALID
    if _has_differences(result):
        _print_differences(result)
        return EXIT_MISMATCH
    print(f"{args.command} ok: equal")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
