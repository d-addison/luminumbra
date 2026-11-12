#!/usr/bin/env python3
"""Package a native RegionFixtureCapture capture as a reproducible fixture ZIP.

Set LUMINUMBRA_REGION_FIXTURE_CAPTURE to an absolute output JSON filename outside
the source tree, then run common_tests --gtest_filter=RegionFixtureCapture.Emit.
Run this script with --capture that.json --output fixtures.zip. Review the ZIP's
explicit scenario/member inventory before extracting fixtures. Existing fixtures
are never read or modified. SHA-256 pins are computed from captured payloads;
production snapshot indexes are retained byte for byte, never reconstructed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import zipfile


SCHEMA = "luminumbra.region_fixture_capture.v1"
SCENARIOS = ("devel-off", "devel-off-plant-edit")
REGION = "chunks/region/r.0.0.lmr"
MANIFEST = "chunks/region/world-manifest.json"
PLANTS = "chunks/region/plant-entities.json"
INDEX = "chunks/region/snapshot.json"
SUBHASHES = {"chunk", "wind", "weather", "aether", "scents", "ecology", "plants"}
# Small, deliberately bounded golden scenarios, not a general saved-world importer.
MAX_MEMBER_BYTES = 1 << 20
MAX_CAPTURE_BYTES = 16 << 20
MAX_JSON_DEPTH = 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def unique_object(pairs: list) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def parse_json(data: bytes) -> dict:
    # Bound nesting before the recursive stdlib decoder, including malformed JSON.
    depth = 0
    quoted = escaped = False
    for byte in data:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):
            depth += 1
            require(depth <= MAX_JSON_DEPTH, "JSON nesting limit exceeded")
        elif byte in (93, 125):
            depth -= 1

    def finite_float(text: str) -> float:
        number = float(text)
        require(math.isfinite(number), "non-finite JSON number")
        return number

    value = json.loads(data.decode("utf-8"), object_pairs_hook=unique_object,
                       parse_float=finite_float,
                       parse_constant=lambda _: require(False, "non-finite JSON number"))
    require(isinstance(value, dict), "JSON object required")
    return value


def read_capture(path: Path) -> bytes:
    # Open nonblocking so hostile FIFO input cannot hang. Read and check size on
    # this descriptor, including on Windows (never DirEntry.stat identity fields).
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode), "capture must be a regular file")
        require(0 < info.st_size <= MAX_CAPTURE_BYTES, "capture size outside bound")
        data = stream.read(MAX_CAPTURE_BYTES + 1)
        require(len(data) == info.st_size, "capture changed or incomplete")
        return data


def hash_text(value: object, *, empty: bool = False) -> bool:
    return isinstance(value, str) and (bool(re.fullmatch(r"[0-9a-f]{16}", value))
                                      or (empty and value == ""))


def fnv64(data: bytes) -> str:
    value = 14695981039346656037
    for byte in data:  # At most MAX_MEMBER_BYTES, or seven bounded hash strings.
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


def world_hash(parts: dict) -> str:
    text = parts["chunk"] + "".join("|" + key + ":" + parts[key] for key in
                                  ("wind", "weather", "aether", "scents", "ecology", "plants"))
    return fnv64(text.encode("ascii"))


def validate_hashes(value: object) -> dict:
    require(isinstance(value, dict), "hashes object required")
    require(set(value) == {"live_world_hash", "loaded_world_hash", "live_subhashes",
                           "loaded_subhashes"}, "incomplete hash capture")
    for phase in ("live", "loaded"):
        require(hash_text(value[phase + "_world_hash"]), "invalid world hash")
        parts = value[phase + "_subhashes"]
        require(isinstance(parts, dict) and set(parts) == SUBHASHES,
                "incomplete diagnostic subhashes")
        require(all(hash_text(part, empty=True) for part in parts.values()),
                "invalid diagnostic subhash")
        require(value[phase + "_world_hash"] == world_hash(parts),
                "world hash does not match diagnostic subhashes")
    return value


def validate_index(document: dict, members: dict) -> None:
    # At most nine named payload/retirement entries in these fixed scenarios;
    # allow sixteen records to leave room for the production writer's metadata.
    records = document.get("members")
    require(document.get("schema") == "luminumbra.snapshot.v1"
            and isinstance(records, list) and len(records) <= 16, "invalid snapshot index")
    allowed = {REGION, MANIFEST, PLANTS, "world_info.json", "aether_state.efs",
               "chunks/region/plant-identities.json", "chunks/region/ground-objects.json",
               "chunks/region/field-pages.fpg", "chunks/region/active-regions.arl"}
    seen, present = set(), set()
    for record in records:
        require(isinstance(record, dict), "invalid snapshot member")
        path, state = record.get("path"), record.get("state")
        require(isinstance(path, str) and path in allowed and path not in seen,
                "unknown or duplicate snapshot member")
        require(state in ("present", "absent"), "unknown snapshot member state")
        seen.add(path)
        if state == "present":
            present.add(path)
    require(present == set(members) - {INDEX}, "incomplete snapshot member inventory")


def artifacts(data: bytes) -> dict[str, bytes]:
    capture = parse_json(data)
    require(set(capture) == {"schema", "scenarios"} and capture["schema"] == SCHEMA,
            "unsupported capture schema")
    scenarios = capture["scenarios"]
    require(isinstance(scenarios, dict) and set(scenarios) == set(SCENARIOS),
            "incomplete scenario capture")
    result = {}
    for scenario in SCENARIOS:
        entry = scenarios[scenario]
        require(isinstance(entry, dict) and set(entry) == {"members", "hashes"},
                "invalid scenario fields")
        members = entry["members"]
        require(isinstance(members, dict), "members object required")
        legacy = {REGION, MANIFEST}
        if scenario == "devel-off-plant-edit":
            legacy.add(PLANTS)
        # Admit the current writer and the later v2 writer without requiring any
        # intervening implementation. Indexed captures always include an envelope.
        require(set(members) in (legacy, legacy | {PLANTS, INDEX}),
                "unexpected or missing capture member")
        for name in sorted(members):
            member = members[name]
            require(isinstance(member, dict) and set(member) == {"bytes", "hex", "fnv64"},
                    "invalid member fields")
            size, encoded = member["bytes"], member["hex"]
            require(type(size) is int and 0 < size <= MAX_MEMBER_BYTES,
                    "member size outside bound")
            require(isinstance(encoded, str) and len(encoded) == size * 2
                    and re.fullmatch(r"[0-9a-f]+", encoded) is not None,
                    "incomplete or invalid member bytes")
            payload = bytes.fromhex(encoded)
            require(hash_text(member["fnv64"]) and member["fnv64"] == fnv64(payload),
                    "member checksum mismatch")
            if name.endswith(".json"):
                document = parse_json(payload)
                if name == MANIFEST:
                    require(document == {"container": "LMR1", "container_version": 2,
                                         "schema": "luminumbra.persistence.world_manifest.v1"},
                            "invalid world manifest")
                elif name == PLANTS:
                    require(document.get("schema") in ("luminumbra.ecs.entity_snapshot.v1",
                                                       "luminumbra.plants.v2")
                            and isinstance(document.get("entities"), list),
                            "invalid plant envelope")
                    require(len(document["entities"]) == int(scenario.endswith("plant-edit")),
                            "incomplete plant roster")
                    require((document["schema"] == "luminumbra.plants.v2") == (INDEX in members),
                            "plant envelope/index format mismatch")
                    if INDEX in members:
                        cursor = document.get("next_id")
                        require(type(cursor) is int and 1 <= cursor <= 1 << 63,
                                "invalid plant allocator cursor")
                else:
                    validate_index(document, members)
            else:
                require(len(payload) >= 24 and payload[:4] == b"LMR1",
                        "invalid region capture")
            result[scenario + "/" + name.rsplit("/", 1)[-1]] = payload
        hashes = validate_hashes(entry["hashes"])
        result[scenario + "/hashes.json"] = canonical_json(hashes)
    pins = {name: {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
            for name, payload in sorted(result.items())}
    result["capture-manifest.json"] = canonical_json({"schema": SCHEMA, "members": pins})
    return result


def canonical_json(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def regenerate(capture: Path, output: Path) -> None:
    payloads = artifacts(read_capture(capture))  # Validate everything before creating output.
    descriptor, temporary = tempfile.mkstemp(prefix=".region-fixtures-", dir=output.parent)
    published = False
    try:
        with os.fdopen(descriptor, "w+b") as stream:
            with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_STORED) as archive:
                for name, payload in sorted(payloads.items()):
                    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                    info.create_system = 3
                    info.external_attr = 0o100644 << 16
                    archive.writestr(info, payload)
            stream.flush()
            os.fsync(stream.fileno())
        # Windows also requires the ZIP handle closed before replacement.
        os.replace(temporary, output)
        published = True
    finally:
        if not published:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        regenerate(args.capture, args.output)
    except (OSError, ValueError, RecursionError) as error:
        print(f"capture refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
