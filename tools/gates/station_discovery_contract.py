#!/usr/bin/env python3
"""Versioned station discovery schema and strict stdlib contract validator."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

SCHEMA_ID = "luminumbra.station-discovery.v1"
HORIZONTAL_MM = 8388608000
CELL_BUDGET = 262144
SAMPLE_BUDGET = 16777216
POSE_KEYS = {"x", "y", "z", "yaw", "pitch"}
KINDS = ("mouth", "arch", "horizon", "coverage")


def object_schema(properties):
    return {"type": "object", "additionalProperties": False,
            "required": list(properties), "properties": properties}


def integer_schema(low=None, high=None):
    result = {"type": "integer"}
    if low is not None:
        result["minimum"] = low
    if high is not None:
        result["maximum"] = high
    return result


PAIR = {"type": "array", "items": {"type": "integer"}, "minItems": 2, "maxItems": 2}
POSE_SCHEMA = object_schema({
    "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
    "yaw": {"type": "number", "minimum": -180, "exclusiveMaximum": 180},
    "pitch": {"type": "number", "minimum": -89, "maximum": 89},
})
QUERY_SCHEMA = object_schema({
    "kind": {"enum": list(KINDS)}, "origin_mm": {"$ref": "#/$defs/intPair"},
    "radius_m": integer_schema(1, 16384), "y_range_m": {"$ref": "#/$defs/intPair"},
    "view_distance_m": integer_schema(0, 16384), "limit": integer_schema(1, 64),
})
SCHEMA = object_schema({
    "schema": {"const": SCHEMA_ID},
    "engine_revision": {"type": "string", "pattern": "^[0-9a-f]{40}$"},
    "inputs_sha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
    "algorithm": {"const": "sampled-portals.v1"},
    "numeric_profile": {"const": "canonical-worldgen.v1"},
    "certified": {"type": "boolean", "const": False},
    "seed": integer_schema(0, 2147483615), "query": QUERY_SCHEMA,
    "candidates": {"type": "array", "maxItems": 64, "items": object_schema({
        "id": {"type": "string", "pattern": "^[a-z0-9-]+$"},
        "anchor_mm": {"type": "array", "items": {"type": "integer"},
                      "minItems": 3, "maxItems": 3},
        "pose": POSE_SCHEMA,
    })},
})
SCHEMA.update({"$schema": "https://json-schema.org/draft/2020-12/schema",
               "$id": "urn:luminumbra:station-discovery:v1", "$defs": {"intPair": PAIR}})


class ContractError(ValueError):
    """Malformed, out of bounds or mismatched discovery evidence."""


def require(condition, message):
    if not condition:
        raise ContractError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"


def input_identity(files):
    """FILES maps logical relative names to exact bytes; preset is preset.json."""
    require(isinstance(files, dict) and "preset.json" in files, "missing preset snapshot")
    manifest = {}
    for name, raw in files.items():
        require(isinstance(name, str) and name and "\\" not in name and
                not name.startswith("/") and ":" not in name and
                all(part not in ("", ".", "..") for part in name.split("/")),
                "input names must be relative and normalized")
        require(isinstance(raw, bytes), "input snapshots must be bytes")
        manifest[name] = hashlib.sha256(raw).hexdigest()
    # The manifest has no trailing newline; the public document has one.
    return hashlib.sha256(canonical(manifest)[:-1]).hexdigest()


def loads(raw):
    def pairs(entries):
        result = {}
        for key, value in entries:
            require(key not in result, "duplicate field: " + key)
            result[key] = value
        return result

    def constant(value):
        raise ContractError("nonfinite JSON number: " + value)

    try:
        return json.loads(raw, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ContractError(str(exc)) from exc


def schema_validate(value, node=SCHEMA, path="$"):
    if "$ref" in node:
        node = SCHEMA["$defs"][node["$ref"].split("/")[-1]]
    kind = node.get("type")
    if kind == "object":
        require(isinstance(value, dict), path + " must be an object")
        require(set(value) == set(node["required"]), path + " fields mismatch")
        for key, child in value.items():
            schema_validate(child, node["properties"][key], path + "." + key)
    elif kind == "array":
        require(isinstance(value, list), path + " must be an array")
        require(node.get("minItems", 0) <= len(value) <= node.get("maxItems", len(value)),
                path + " array length outside bounds")
        for index, child in enumerate(value):
            schema_validate(child, node["items"], path + "[%d]" % index)
    elif kind == "integer":
        require(type(value) is int, path + " must be an integer")
    elif kind == "number":
        require(type(value) in (int, float), path + " must be a finite number")
        try:
            require(math.isfinite(value), path + " must be a finite number")
        except OverflowError as exc:
            raise ContractError(path + " number outside supported range") from exc
    elif kind == "boolean":
        require(type(value) is bool, path + " must be boolean")
    elif kind == "string":
        require(isinstance(value, str), path + " must be a string")
    if "const" in node:
        expected = node["const"]
        require(value == expected and type(value) is type(expected), path + " constant mismatch")
    if "enum" in node:
        require(value in node["enum"], path + " unknown value")
    if "pattern" in node:
        require(re.fullmatch(node["pattern"], value) is not None, path + " invalid token")
    for key, predicate in (("minimum", lambda a, b: a >= b),
                           ("maximum", lambda a, b: a <= b),
                           ("exclusiveMaximum", lambda a, b: a < b)):
        if key in node:
            require(predicate(value, node[key]), path + " outside bounds")


def validate_work(cells, samples):
    require(type(cells) is int and 0 <= cells <= CELL_BUDGET, "cell budget exhausted")
    require(type(samples) is int and 0 <= samples <= SAMPLE_BUDGET, "sample budget exhausted")


def validate(document, *, expected_engine_revision=None, expected_inputs_sha256=None,
             require_certified=False):
    schema_validate(document)
    for key, expected in (("engine_revision", expected_engine_revision),
                          ("inputs_sha256", expected_inputs_sha256)):
        if expected is not None:
            require(document[key] == expected, key + " identity mismatch")
    require(not require_certified or document["certified"], "numerical certification unavailable")
    query = document["query"]
    lo, hi = query["y_range_m"]
    require(-2147483648 <= lo <= hi <= 2147483647 and hi - lo <= 512,
            "vertical span outside bounds")
    feature = query["kind"] in ("mouth", "arch")
    reach = (query["radius_m"] + (query["view_distance_m"] + 8 if feature else 0)) * 1000 + 500
    require(all(-HORIZONTAL_MM + reach <= c <= HORIZONTAL_MM - reach
                for c in query["origin_mm"]), "horizontal search or pose footprint outside bounds")
    require(len(document["candidates"]) <= query["limit"], "candidate count exceeds limit")
    seen, previous = set(), None
    pattern = re.compile(r"(mouth|arch|horizon-control|coverage-control)-z-([np])(0|[1-9][0-9]*)"
                         r"-x-([np])(0|[1-9][0-9]*)-i-(0|[1-9][0-9]*)")
    for candidate in document["candidates"]:
        token = candidate["id"]
        require(token not in seen, "duplicate candidate id")
        seen.add(token)
        match = pattern.fullmatch(token)
        require(match is not None, "invalid candidate identity")
        prefix, sz, z, sx, x, index = match.groups()
        expected_prefix = query["kind"] + ("-control" if query["kind"] in ("horizon", "coverage") else "")
        require(prefix == expected_prefix, "control and discovery kinds must remain distinct")
        require(not (sz == "n" and z == "0") and not (sx == "n" and x == "0"),
                "negative zero cell identity refused")
        cell_z = int(z) * (-1 if sz == "n" else 1)
        cell_x = int(x) * (-1 if sx == "n" else 1)
        index = int(index)
        require(0 <= index <= 2 ** 64 - 1, "local index outside uint64 domain")
        require(-2147483648 <= cell_z <= 2147483647 and -2147483648 <= cell_x <= 2147483647,
                "cell identity outside integer domain")
        ax, ay, az = candidate["anchor_mm"]
        require(abs(ax) <= HORIZONTAL_MM and abs(az) <= HORIZONTAL_MM,
                "anchor outside horizontal domain")
        require(lo * 1000 <= ay <= hi * 1000, "anchor outside vertical span")
        distance = (ax - query["origin_mm"][0]) ** 2 + (az - query["origin_mm"][1]) ** 2
        require(distance <= (query["radius_m"] * 1000) ** 2, "anchor outside search radius")
        key = (distance, cell_z, cell_x, index)
        require(previous is None or previous < key, "candidates must be uniquely distance ordered")
        previous = key
        pose = candidate["pose"]
        require(abs(pose["x"]) * 1000 + 500 <= HORIZONTAL_MM and
                abs(pose["z"]) * 1000 + 500 <= HORIZONTAL_MM, "pose outside horizontal domain")
        require(lo + 0.5 <= pose["y"] <= hi - 0.5, "eye clearance outside vertical span")
        if query["kind"] in ("horizon", "coverage"):
            i = index
            require(cell_z == cell_x == 0 and i < (4 if query["kind"] == "horizon" else 5),
                    "invalid control identity")
            require([ax, az] == query["origin_mm"] and
                    pose["x"] == ax / 1000 and pose["z"] == az / 1000,
                    "control must stay at query origin")
            require(pose["yaw"] == (0, 90, -180, -90, 45)[i] and
                    pose["pitch"] == (-20 if i == 4 else 0), "control direction mismatch")
            require(pose["y"] == ay / 1000 + (80 if i == 4 else 1.8),
                    "control height offset mismatch")
        else:
            require(ax % 4000 == ay % 4000 == az % 4000 == 0, "feature anchor off 4 metre lattice")
            # The complete 12 m portal stencil above the anchor must fit the span.
            require(ay + 12000 <= hi * 1000, "portal stencil outside vertical span")
            # The eye stands at least 1.8 m above the anchor (higher over terrain).
            require(pose["y"] >= ay / 1000 + 1.8 - 1e-6, "feature eye below anchor clearance")
            dx, dz = ax / 1000 - pose["x"], az / 1000 - pose["z"]
            displacement = query["view_distance_m"]
            require((dx, dz) in ((displacement, 0), (0, displacement),
                                (-displacement, 0), (0, -displacement)),
                    "feature view displacement must be cardinal")
            horizontal = math.hypot(dx, dz)
            require(abs(horizontal - query["view_distance_m"]) <= 0.001,
                    "camera distance differs from view distance")
            yaw = (math.degrees(math.atan2(dz, dx)) + 180) % 360 - 180
            pitch = max(-89, min(89, math.degrees(math.atan2(ay / 1000 - pose["y"], horizontal))))
            # DeterministicMath's atan polynomial has <=1.2e-4 rad core error;
            # 0.01 degrees also covers binary32 division/sqrt/degree rounding.
            require(abs((pose["yaw"] - yaw + 180) % 360 - 180) <= 0.01 and
                    abs(pose["pitch"] - pitch) <= 0.01,
                    "feature orientation differs from displacement")
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", action="store_true")
    parser.add_argument("--input", type=Path)
    parser.add_argument("--expected-engine-revision")
    parser.add_argument("--expected-inputs-sha256")
    parser.add_argument("--require-certified", action="store_true")
    args = parser.parse_args()
    for value, size in ((args.expected_engine_revision, 40), (args.expected_inputs_sha256, 64)):
        if value is not None and re.fullmatch(r"[0-9a-f]{%d}" % size, value) is None:
            parser.error("expected identity must be lowercase hexadecimal")
    if args.schema:
        if args.input is not None:
            parser.error("--schema and --input are mutually exclusive")
        sys.stdout.buffer.write(canonical(SCHEMA))
        return 0
    if args.input is None:
        parser.error("--input is required")
    try:
        result = validate(loads(args.input.read_bytes()),
                          expected_engine_revision=args.expected_engine_revision,
                          expected_inputs_sha256=args.expected_inputs_sha256,
                          require_certified=args.require_certified)
        encoded = canonical(result)
    except (OSError, ValueError, RecursionError) as exc:
        print("station discovery refused: " + str(exc), file=sys.stderr)
        return 1
    sys.stdout.buffer.write(encoded)
    return 0


if __name__ == "__main__":
    sys.exit(main())
