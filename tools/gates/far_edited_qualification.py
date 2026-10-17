#!/usr/bin/env python3
"""Saved far-world station oracle and native qualification gate (stdlib only)."""
import argparse
import json
import re
from pathlib import Path

from far_ladder_oracle import (ArtifactError, STATIONS, STATION_ORIENTATIONS,
                               ELEVATED_DELTA_METRES, POSE_TOLERANCE,
                               integer, number, read_artifact, require, wanted_tiles)

STATION_SCHEMA = "luminumbra.farlod_horizon.edited.v1"
NATIVE_SCHEMA = "luminumbra.far_edited.native.v1"


def production_tier_budget():
    # Read the engine's production profile; never trust a captured limit or
    # silently fall back if the source-of-truth expression changes.
    source = Path(__file__).resolve().parents[2] / "src/luminumbra_client/rendering/FarWorldRenderer.cpp"
    match = re.search(r"FarWorldDevelopmentLimits\(\) noexcept\s*\{\s*"
                      r"FarWorldExperimentalLimits limits;\s*"
                      r"limits\.budgets\.bytes\.fill\((\d+)ull \* (\d+) \* (\d+)\);",
                      source.read_text(encoding="utf-8"))
    require(match is not None, "cannot read production tier budget")
    result = 1
    for factor in match.groups():
        result *= int(factor)
    return result


def fields(value, names, label):
    require(type(value) is dict, f"{label} must be an object")
    missing = sorted(set(names) - value.keys())
    require(not missing, f"{label} missing fields: {missing}")
    return value


def span(value, label, requested=False):
    require(type(value) is list and len(value) == 2, f"{label} must be a span")
    for coordinate in value:
        if requested:
            number(coordinate, label)
        else:
            require(type(coordinate) is int, f"{label} must use integer coordinates")
    require(value[0] <= value[1] if requested else value[0] < value[1], f"{label} is empty or reversed")
    return value


def identity(value, snapshot, key):
    fields(value, ("world_epoch", "content_hash", "cave_policy", "tier", "tx", "tz",
                   "bake_generation", "span_generation", "requested_span"), "target identity")
    for name in ("world_epoch", "content_hash", "cave_policy", "bake_generation", "span_generation"):
        integer(value[name], name)
    require(value["world_epoch"] == snapshot["world_epoch"], "foreign target epoch")
    require(value["bake_generation"] <= snapshot["committed_generation"], "future target generation")
    require(value["cave_policy"] in (0, 1), "invalid cave policy")
    require((value["tier"], value["tx"], value["tz"]) == key, "target key mismatch")
    return (None if value["requested_span"] is None else
            span(value["requested_span"], "target requested span", requested=True))


def validate_target(snapshot, expected):
    fields(snapshot, ("schema", "captured", "complete", "world_epoch", "committed_generation",
                      "frame", "target_serial", "target_ready", "tiles", "memory",
                      "submission_refusals", "submitted_draws", "submitted_indices",
                      "submitted_water_indices", "draws"), "snapshot")
    require(snapshot["schema"] == "luminumbra.far_handoff.v1", "wrong handoff schema")
    require(snapshot["captured"] is True and snapshot["complete"] is True, "incomplete capture")
    for name in ("world_epoch", "target_serial"):
        integer(snapshot[name], name, 1)
    for name in ("committed_generation", "frame", "submission_refusals"):
        integer(snapshot[name], name)
    require(snapshot["submission_refusals"] == 0, "submission refusal")
    require(type(snapshot["tiles"]) is list, "tiles must be an array")
    targets, bindings = [], {}
    for tile in snapshot["tiles"]:
        fields(tile, ("tier", "tx", "tz", "target"), "tile")
        key = tuple(tile[name] for name in ("tier", "tx", "tz"))
        require(all(type(v) is int for v in key) and 1 <= key[0] <= 5, "invalid tile key")
        if tile["target"] is None:
            continue  # An old serving-only key cannot cover a target key.
        fields(tile, ("target_covering", "target_water_complete", "target_state", "target_built_span",
                      "target_build_failure", "target_upload_failure", "target_terrain_indices",
                      "target_water_indices", "target_water_known_dry"), "target tile")
        require(key not in targets, "duplicate target key")
        targets.append(key)
        requested = identity(tile["target"], snapshot, key)
        built = span(tile["target_built_span"], "target built span")
        if requested is not None:
            require(built[0] <= requested[0] and built[1] >= requested[1], "target span does not cover")
        require(tile["target_covering"] is True, "stale serving coverage cannot qualify target")
        require(tile["target_water_complete"] is True, "incomplete target water")
        # FarTileState: Wanted, Building, Ready, Uploaded, Empty, Rejected.
        require(type(tile["target_state"]) is int and tile["target_state"] in (3, 4), "target not uploaded or Empty")
        require(tile["target_build_failure"] is None and tile["target_upload_failure"] is None,
                "failed target")
        terrain = integer(tile["target_terrain_indices"], "terrain indices")
        water = integer(tile["target_water_indices"], "water indices")
        require(type(tile["target_water_known_dry"]) is bool, "dry observation must be boolean")
        if tile["target_state"] == 4:
            require(terrain == water == 0 and tile["target_water_known_dry"], "wet or drawable Empty")
        else:
            require(terrain + water > 0, "uploaded target has no geometry")
            require((water == 0) == tile["target_water_known_dry"], "wet/dry indices disagree")
        # The sweep pre-captures common support. This is render currency, not a
        # support-independent authority token: bind the support as well as hash.
        bindings[key] = (tile["target"]["content_hash"], tile["target"]["cave_policy"],
                         tile["target"]["bake_generation"], tile["target"]["span_generation"],
                         None if requested is None else tuple(requested), tuple(built))
    require(len({binding[1] for binding in bindings.values()}) == 1, "inconsistent cave policy")
    require(sorted(targets) == sorted(expected), "target wanted-set mismatch")
    require(snapshot["target_ready"] is True, "target readiness refused")
    require(type(snapshot["memory"]) is list and len(snapshot["memory"]) == 5, "omitted memory tier")
    budget = production_tier_budget()
    for memory in snapshot["memory"]:
        fields(memory, ("limit", "total_bytes", "peak_bytes"), "memory")
        limit = integer(memory["limit"], "memory limit", 1)
        total = integer(memory["total_bytes"], "memory total")
        peak = integer(memory["peak_bytes"], "memory peak")
        require(limit == budget and total <= peak <= budget, "production memory budget exceeded")
    validate_submission(snapshot)
    return (snapshot["world_epoch"], snapshot["committed_generation"], bindings)


def validate_submission(snapshot):
    draws = snapshot["draws"]
    require(type(draws) is list, "draws must be an array")
    count = integer(snapshot["submitted_draws"], "submitted draws")
    indices = integer(snapshot["submitted_indices"], "submitted indices")
    water = integer(snapshot["submitted_water_indices"], "submitted water indices")
    require(count == len(draws), "draw submission count mismatch")
    target_tiles = {(t["tier"], t["tx"], t["tz"]): t for t in snapshot["tiles"] if t["target"] is not None}
    drawable_keys = {key for key, tile in target_tiles.items()
                     if tile["target_terrain_indices"] + tile["target_water_indices"] > 0}
    seen, expected_indices, expected_water = set(), 0, 0
    for draw in draws:
        fields(draw, ("tier", "tx", "tz", "submitted", "world_epoch", "content_hash",
                      "cave_policy", "bake_generation", "span_generation"), "draw")
        key = tuple(draw[name] for name in ("tier", "tx", "tz"))
        require(all(type(v) is int for v in key), "invalid draw key")
        require(draw["submitted"] is True and key not in seen and key in target_tiles,
                "missing, duplicate or foreign submitted draw")
        seen.add(key)
        tile = target_tiles[key]
        require(all(type(draw[name]) is int and draw[name] == tile["target"][name] for name in
                    ("world_epoch", "content_hash", "cave_policy", "bake_generation", "span_generation")),
                "submitted draw is not current target")
        size = tile["target_terrain_indices"] + tile["target_water_indices"]
        require(size > 0, "Empty target cannot submit a draw")
        expected_indices += size
        expected_water += tile["target_water_indices"]
    require(seen == drawable_keys, "drawable targets were not submitted exactly once")
    require(indices == expected_indices and water == expected_water, "submitted index totals mismatch")


def validate_stations(artifact):
    fields(artifact, ("schema", "world_preset", "complete", "expected_stations", "stations"), "stations artifact")
    require(artifact["schema"] == STATION_SCHEMA, "wrong station schema")
    require(artifact["complete"] is True, "incomplete station artifact")
    require(type(artifact["expected_stations"]) is int and artifact["expected_stations"] == 5,
            "expected all five stations")
    require(type(artifact["world_preset"]) is str and artifact["world_preset"], "missing world preset binding")
    rows = artifact["stations"]
    require(type(rows) is list and len(rows) == 5, "missing station")
    names, poses, binding, last_frame, last_serial = [], {}, None, -1, 0
    for row in rows:
        fields(row, ("name", "camera", "requested_mode", "effective_mode", "selection_refused",
                     "selection_advance", "selection_submit", "settle_seconds", "ready_within_deadline",
                     "paired_capture_preserved", "snapshot"), "station")
        name = row["name"]
        require(type(name) is str and name in STATIONS and name not in names, "unknown or duplicate station")
        require(name == STATIONS[len(names)], "station order mismatch")
        names.append(name)
        require(row["requested_mode"] == row["effective_mode"] == "ladder" and row["selection_refused"] is False and
                row["selection_advance"] is True and row["selection_submit"] is True,
                "production ladder selection refused")
        require(0 <= number(row["settle_seconds"], "settle seconds") <= 90, "station exceeds inclusive 90 seconds")
        require(row["ready_within_deadline"] is True and row["paired_capture_preserved"] is True,
                "station not ready or paired capture not preserved")
        camera = fields(row["camera"], ("world_x", "world_y", "world_z", "yaw_degrees", "pitch_degrees"), "camera")
        for value in camera.values():
            number(value, "camera coordinate")
        yaw, pitch = STATION_ORIENTATIONS[name]
        yaw_error = abs((camera["yaw_degrees"] - yaw + 180) % 360 - 180)
        require(yaw_error <= POSE_TOLERANCE and abs(camera["pitch_degrees"] - pitch) <= POSE_TOLERANCE,
                "station orientation mismatch")
        poses[name] = camera
        snapshot = row["snapshot"]
        current = validate_target(snapshot, wanted_tiles(camera["world_x"], camera["world_z"]))
        require(all(tile["target_built_span"][0] <= camera["world_y"] <= tile["target_built_span"][1]
                    for tile in snapshot["tiles"] if tile["target"] is not None),
                "common capture support does not cover station height")
        if binding is None:
            binding = current
        require(current == binding, "qualification binding changed across stations")
        require(snapshot["frame"] > last_frame, "station frames must be strictly ordered")
        require(snapshot["target_serial"] >= last_serial, "target publication serial regressed")
        last_frame, last_serial = snapshot["frame"], snapshot["target_serial"]
    origin = poses[STATIONS[0]]
    for name in STATIONS:
        pose = poses[name]
        for axis in ("world_x", "world_z"):
            require(abs(pose[axis] - origin[axis]) <= POSE_TOLERANCE, "station moved horizontally")
        delta = ELEVATED_DELTA_METRES if name == "elevated" else 0
        require(abs(pose["world_y"] - origin["world_y"] - delta) <= POSE_TOLERANCE, "station height mismatch")
    return {"status": "PASS", "stations": len(rows)}


def validate_native(artifact):
    fields(artifact, ("schema", "execution", "complete"), "native artifact")
    require(artifact["schema"] == NATIVE_SCHEMA, "wrong native schema")
    if artifact["execution"] != "native" or artifact["complete"] is not True:
        return {"status": "PENDING", "reason": "native qualification has not completed"}
    fields(artifact, ("chunk_count", "edited_chunk_count", "edit_ms", "authoritative_commit_ms",
                      "all_tier_bake_ms", "peak_memory_bytes", "authoritative_bytes", "derived_bytes",
                      "tiers", "hash_runs", "stations"), "native measurements")
    require(integer(artifact["chunk_count"], "chunk count") == 5433, "requires 5,433 chunks")
    require(1 <= integer(artifact["edited_chunk_count"], "edited chunk count") <= 5433, "requires edited chunks")
    for name in ("edit_ms", "authoritative_commit_ms", "all_tier_bake_ms"):
        require(number(artifact[name], name) > 0, f"{name} must be measured")
    for name in ("peak_memory_bytes", "authoritative_bytes", "derived_bytes"):
        integer(artifact[name], name, 1)
    tiers = artifact["tiers"]
    require(type(tiers) is list and len(tiers) == 5, "all-tier measurements required")
    seen = []
    for tier in tiers:
        fields(tier, ("tier", "bake_ms", "bytes", "complete"), "tier measurement")
        index = integer(tier["tier"], "tier", 1)
        require(index <= 5 and index not in seen, "duplicate or invalid tier")
        seen.append(index)
        require(tier["complete"] is True and number(tier["bake_ms"], "tier bake ms") > 0, "unmeasured tier")
        integer(tier["bytes"], "tier bytes", 1)
    runs = artifact["hash_runs"]
    require(type(runs) is list and len(runs) == 12, "render/schedule/cache hash matrix required")
    combinations, baseline = [], None
    for run in runs:
        fields(run, ("rendering", "schedule", "cache", "seed", "tick_hashes"), "hash run")
        require(type(run["rendering"]) is bool, "rendering must be boolean")
        require(run["schedule"] in ("serial", "permuted") and run["cache"] in ("absent", "present", "removed"),
                "unknown hash matrix control")
        require(type(run["seed"]) is int and run["seed"] == 12345, "hash matrix requires fixed seed")
        combination = (run["rendering"], run["schedule"], run["cache"])
        require(combination not in combinations, "duplicate hash matrix control")
        combinations.append(combination)
        hashes = run["tick_hashes"]
        require(type(hashes) is list and len(hashes) >= 2 and
                all(type(h) is str and len(h) == 16 and all(c in "0123456789abcdef" for c in h)
                    for h in hashes), "missing or malformed tick hashes")
        if baseline is None:
            baseline = hashes
        require(hashes == baseline, "simulation hashes differ across rendering, schedules or cache")
    validate_stations(artifact["stations"])
    return {"status": "PASS", "chunk_count": 5433, "stations": 5}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("stations", "native"))
    parser.add_argument("artifact", nargs="?")
    args = parser.parse_args()
    try:
        if args.command == "native" and (args.artifact is None or not Path(args.artifact).exists()):
            result = {"status": "PENDING", "reason": "native 5,433-chunk measurements have not been supplied"}
        else:
            require(args.artifact is not None, "station artifact required")
            artifact = read_artifact(args.artifact)
            result = validate_stations(artifact) if args.command == "stations" else validate_native(artifact)
    except (ArtifactError, TypeError, ValueError) as error:
        result = {"status": "FAIL", "reason": str(error)}
    print(json.dumps(result, sort_keys=True))
    return {"PASS": 0, "FAIL": 1, "PENDING": 3}[result["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
