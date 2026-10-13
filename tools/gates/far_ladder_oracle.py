#!/usr/bin/env python3
"""Independent #199 oracle and strict horizon-v2 station validator (stdlib only).

The wanted/legacy formulas formerly in far-ladder-oracle.ps1 live here so both
PowerShell and file-only CI use one implementation independent of engine code.
V1 and the historical aggregate-only v2 remain legacy evidence, not qualification.
"""
import argparse
import json
import math
from pathlib import Path

SCHEMA = "luminumbra.farlod_horizon.v2"
STATIONS = ("eye_yaw_000", "eye_yaw_090", "eye_yaw_180", "eye_yaw_270", "elevated")
STATION_ORIENTATIONS = dict(zip(STATIONS, ((0, 0), (90, 0), (180, 0), (270, 0), (45, -20))))
POSE_TOLERANCE = 0.05  # degrees/metres; tolerates serialized float camera inputs
ELEVATED_DELTA_METRES = 80.0 - 1.8


class ArtifactError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ArtifactError(message)


def obj(value, fields, path):
    require(isinstance(value, dict), f"{path} must be an object")
    require(set(fields) <= value.keys(), f"{path} missing fields: {set(fields) - value.keys()}")
    return value


def integer(value, path, minimum=0):
    require(type(value) is int and value >= minimum, f"{path} must be an integer >= {minimum}")
    return value


def number(value, path):
    require(type(value) in (int, float) and math.isfinite(value), f"{path} must be finite")
    return value


def boolean(value, path):
    require(type(value) is bool, f"{path} must be boolean")
    return value


def camera_domain(x, z):
    for v in (x, z):
        number(v, "camera")
        require(abs(v) <= 8388608, "camera out of domain")


def axis_gap(index, edge, coordinate):
    return max(index * edge - coordinate, 0, coordinate - (index + 1) * edge)


def disc_cells(x, z, edge, radius):
    camera_domain(x, z)
    # Extra candidate ring handles inclusive contact on exact cell boundaries.
    for tz in range(math.floor((z - radius) / edge) - 1, math.floor((z + radius) / edge) + 2):
        dz = axis_gap(tz, edge, z)
        for tx in range(math.floor((x - radius) / edge) - 1, math.floor((x + radius) / edge) + 2):
            dx = axis_gap(tx, edge, x)
            if dx * dx + dz * dz <= radius * radius:
                yield tx, tz


def wanted_tiles(x, z):
    result = []
    for tier in range(1, 6):
        spacing = 4 * 2 ** (tier - 1)
        radius = 256 * spacing + (4 * spacing if tier < 5 else 0)
        result.extend((tier, tx, tz) for tx, tz in disc_cells(x, z, 128 * spacing, radius))
    return result


def legacy_count(x, z):
    return sum(1 for _ in disc_cells(x, z, 512, 3000))


def keys(values, path):
    require(type(values) is list, f"{path} must be an array")
    result = []
    for value in values:
        require(type(value) is list and len(value) == 3, f"{path} malformed tile")
        require(all(type(v) is int for v in value), f"{path} noninteger tile")
        require(1 <= value[0] <= 5 and all(abs(v) <= 2**63 - 1 for v in value[1:]), f"{path} invalid tile")
        result.append(tuple(value))
    require(len(result) == len(set(result)), f"{path} duplicate tile")
    return set(result)


IDENTITY = ("world_epoch", "content_identity", "cave_policy", "key", "bake_generation", "span_generation")
TIER_COUNTERS = ("wanted", "pending", "building", "ready", "settled", "uploaded_pairs", "empty_pairs",
                 "current_covering", "rejected", "budget_evicted", "evicted_range_total", "evicted_budget_total",
                 "stale_rebuilds_total", "retained_ready_cpu_bytes", "retained_gpu_logical_store_bytes",
                 "pool_cpu_bytes", "pool_gpu_logical_store_bytes", "pool_sampled_total_bytes", "pool_limit_bytes",
                 "pool_peak_bytes", "admission_refusals", "draws", "indices", "draw_refusals")
SNAPSHOT_FIELDS = ("requested_mode", "effective_mode", "refusal", "submission_refusal", "suppression", "selected", "world_epoch",
                   "frame_id", "content_identity", "bake_generation", "far_plane_m", "requested_cave_policy",
                   "bound_cave_policy", "numeric_policy",
                   "captured", "complete", "advancement_occurred", "submission_occurred", "draws", "indices",
                   "ladder_water_available", "memory_sampling", "gpu_bytes_kind", "camera", "frame_upload", "tiers", "tiles")


def identity(value, path):
    obj(value, IDENTITY, path)
    for field in IDENTITY:
        if field != "key":
            integer(value[field], f"{path}.{field}")
    return next(iter(keys([value["key"]], f"{path}.key")))


def span(value, path):
    require(type(value) is list and len(value) == 2, f"{path} malformed span")
    lo, hi = (number(v, path) for v in value)
    require(lo <= hi, f"{path} inverted span")
    return lo, hi


def validate_snapshot(s, mode, camera, expected):
    obj(s, SNAPSHOT_FIELDS, "snapshot")
    require(s["requested_mode"] == mode and s["effective_mode"] == mode, "wrong-mode/refused selection")
    require(s["refusal"] == "None" and s["submission_refusal"] == "None" and s["suppression"] == "None", "renderer refusal/suppression")
    for field in ("selected", "captured", "complete", "advancement_occurred", "submission_occurred", "ladder_water_available"):
        boolean(s[field], field)
    require(s["captured"] and s["complete"], "incomplete snapshot")
    require(s["selected"] == (mode == "ladder"), "selected differs from effective mode")
    require(number(s["far_plane_m"], "far_plane_m") == (17408 if mode == "ladder" else 3200), "wrong far plane")
    require(type(s["camera"]) is list and len(s["camera"]) == 3, "snapshot camera malformed")
    require(all(number(v, "snapshot.camera") == camera[i] for i, v in enumerate(s["camera"])), "stale camera snapshot")
    integer(s["world_epoch"], "world_epoch", 1)
    integer(s["frame_id"], "frame_id", 1)
    for f in ("content_identity", "bake_generation", "numeric_policy", "draws", "indices"):
        integer(s[f], f)
    for f in ("requested_cave_policy", "bound_cave_policy"):
        if s[f] is not None:
            require(integer(s[f], f) in (0, 1), f"invalid {f}")
    require(s["numeric_policy"] in (0, 1), "invalid numeric policy")
    if mode == "ladder":
        require(s["requested_cave_policy"] is not None and
                s["bound_cave_policy"] == s["requested_cave_policy"], "missing/mismatched cave policy binding")
    require(s["gpu_bytes_kind"] == "logical_stores" and
            s["memory_sampling"] == "asynchronous_diagnostic_not_atomic_admission", "invalid memory sampling labels")
    require(s["ladder_water_available"] is False, "unsupported ladder water claim")
    upload = obj(s["frame_upload"], ("attempts", "debited_bytes", "successful_bytes", "horizon_refused"), "frame_upload")
    for f in ("attempts", "debited_bytes", "successful_bytes"):
        integer(upload[f], f)
    require(upload["successful_bytes"] <= upload["debited_bytes"], "invalid upload debit")
    require(boolean(upload["horizon_refused"], "horizon_refused") is False, "horizon refusal")
    tiers = s["tiers"]
    require(type(tiers) is list and len(tiers) == 5, "omitted tier")
    for i, tier in enumerate(tiers, 1):
        obj(tier, ("tier",) + TIER_COUNTERS, "tier")
        require(integer(tier["tier"], "tier", 1) == i, "wrong or duplicated tier")
        for f in TIER_COUNTERS:
            integer(tier[f], f)
        require(tier["rejected"] == tier["budget_evicted"] == tier["draw_refusals"] == 0, "rejected tiles/draws")
        require(tier["indices"] % 3 == 0, "invalid submitted index count: not triangle-aligned")
        if tier["draws"]:
            require(tier["indices"] >= 3 * tier["draws"],
                    "invalid submitted index count: fewer than 3 indices per draw")
        else:
            require(tier["indices"] == 0, "indices without submission")
    require(s["submission_occurred"] == (s["draws"] > 0), "submission differs from actual draw counts")
    require((s["draws"] == 0) == (s["indices"] == 0) and s["indices"] % 3 == 0, "invalid draw/index counts")
    require(type(s["tiles"]) is list, "tiles malformed")
    if mode == "legacy":
        require(s["tiles"] == [], "legacy evidence contains ladder residency")
        return
    integer(s["content_identity"], "content_identity", 1)
    require(s["advancement_occurred"], "station did not advance")
    observed = set()
    states = {i: {"Uploaded": 0, "Empty": 0} for i in range(1, 6)}
    for tile in s["tiles"]:
        obj(tile, ("desired", "built", "has_pair", "state", "current", "span_ok", "eligibility", "requested_span", "built_span"), "tile")
        key = identity(tile["desired"], "desired")
        identity(tile["built"], "built")
        require(key not in observed, "duplicate coverage tile")
        observed.add(key)
        require(tile["built"] == tile["desired"], "stale-generation coverage identity")
        for f in ("world_epoch", "content_identity", "bake_generation"):
            require(tile["desired"][f] == s[f], "stale binding identity")
        require(tile["desired"]["cave_policy"] == s["bound_cave_policy"], "stale binding identity")
        require(all(boolean(tile[f], f) for f in ("has_pair", "current", "span_ok")), "missing current spanning pair")
        require(tile["state"] in ("Uploaded", "Empty"), "unsettled tile")
        require(integer(tile["eligibility"], "eligibility") == 4, "unsettled/rejected tile eligibility")
        built = span(tile["built_span"], "built_span")
        require(built[0] < built[1], "empty built span")
        if tile["requested_span"] is not None:
            requested = span(tile["requested_span"], "requested_span")
            require(built[0] <= requested[0] and built[1] >= requested[1], "span refusal")
        states[key[0]][tile["state"]] += 1
    require(observed == expected, "missing_expected coverage tile or extra tile")
    mixed = any(state["Uploaded"] for state in states.values())
    for tier in tiers:
        count = sum(key[0] == tier["tier"] for key in expected)
        state = states[tier["tier"]]
        require(tier["wanted"] == tier["current_covering"] == tier["settled"] == count, "missing_expected per tier")
        require(tier["uploaded_pairs"] == state["Uploaded"] and tier["empty_pairs"] == state["Empty"], "pair counters differ from tiles")
        require(tier["pending"] == tier["building"] == tier["ready"] == 0, "incomplete station")
        require(tier["draws"] == state["Uploaded"], "GL submission counts differ from current drawable pairs")
        if tier["draws"] == 0:
            require(tier["indices"] == 0, "indices without submission")
    require(sum(t["draws"] for t in tiers) == s["draws"] and sum(t["indices"] for t in tiers) == s["indices"], "per-tier draw totals differ")
    require(not mixed or s["submission_occurred"], "mixed fixture has no actual submission")


def validate_legacy_aggregate(a):
    # Preserve all #199 compatibility assertions on the actual gate path.
    camera = obj(a.get("camera"), ("world_x", "world_z"), "aggregate.camera")
    x, z = number(camera["world_x"], "aggregate.camera.world_x"), number(camera["world_z"], "aggregate.camera.world_z")
    camera_domain(x, z)
    legacy = obj(a.get("legacy"), ("region_size_m", "f1_outer_range_m", "f2_outer_range_m",
                                  "regions_wanted", "regions_resident", "regions_missing"), "aggregate.legacy")
    for field in legacy:
        if field in ("region_size_m", "f1_outer_range_m", "f2_outer_range_m",
                     "regions_wanted", "regions_resident", "regions_missing"):
            integer(legacy[field], "aggregate.legacy." + field)
    require(legacy["regions_wanted"] == legacy_count(x, z), "aggregate legacy oracle disagreement")
    require(legacy["regions_missing"] == 0, "aggregate missing legacy regions")
    ladder = obj(a.get("ladder"), ("wanted_error", "expected_by_tier", "expected_tiles", "resident_tiles", "stale_tiles"), "aggregate.ladder")
    require(ladder["wanted_error"] == "None", "aggregate wanted-set refusal")
    expected = set(wanted_tiles(x, z))
    require(keys(ladder["expected_tiles"], "aggregate.expected_tiles") == expected, "aggregate independent oracle disagreement")
    counts = ladder["expected_by_tier"]
    require(type(counts) is list and len(counts) == 5, "aggregate omitted expected tier")
    require(all(integer(v, "aggregate.expected_by_tier") == sum(k[0] == i for k in expected)
                for i, v in enumerate(counts, 1)), "aggregate oracle tier-count disagreement")
    require(not keys(ladder["resident_tiles"], "aggregate.resident_tiles"), "legacy aggregate contains ladder residents")
    require(integer(ladder["stale_tiles"], "aggregate.stale_tiles") == 0, "aggregate stale-generation tiles")


def validate_station_pose(name, camera):
    if name not in STATION_ORIENTATIONS:
        return  # file-only/custom fixtures have their own required name list
    yaw, pitch = STATION_ORIENTATIONS[name]
    yaw_error = abs((camera["yaw_degrees"] - yaw + 180) % 360 - 180)
    require(yaw_error <= POSE_TOLERANCE and
            abs(camera["pitch_degrees"] - pitch) <= POSE_TOLERANCE,
            f"station {name} orientation differs from required pose")


def validate_station_heights(poses):
    if "elevated" not in poses:
        return
    require(set(STATIONS) <= poses.keys(), "missing required eye-height station")
    eye = poses[STATIONS[0]]
    for name in STATIONS[1:4]:
        require(all(abs(poses[name][f] - eye[f]) <= POSE_TOLERANCE
                    for f in ("world_x", "world_y", "world_z")), "eye station position/height disagreement")
    high = poses["elevated"]
    require(all(abs(high[f] - eye[f]) <= POSE_TOLERANCE for f in ("world_x", "world_z")),
            "elevated station XZ differs from eye stations")
    require(abs(high["world_y"] - eye["world_y"] - ELEVATED_DELTA_METRES) <= POSE_TOLERANCE,
            "wrong elevated height: expected eye height + 78.2 m")


def validate_artifact(a, renderer="legacy", required_stations=None, oracle=wanted_tiles):
    obj(a, ("schema", "mode", "ladder_checked", "world_preset", "expected_stations", "stations", "complete", "ladder_water"), "artifact")
    require(a["schema"] == SCHEMA, "wrong schema")
    require(renderer in ("legacy", "ladder") and a["mode"] == renderer, "wrong-mode artifact")
    require(boolean(a["ladder_checked"], "ladder_checked") == (renderer == "ladder"), "wrong ladder_checked")
    require(boolean(a["complete"], "complete"), "incomplete artifact")
    require(a["ladder_water"] == "absent_S13_out_of_scope", "missing water refusal")
    expected_count = integer(a["expected_stations"], "expected_stations", 1)
    require(type(a["stations"]) is list and len(a["stations"]) == expected_count, "missing station or truncated artifact")
    if renderer == "legacy":
        validate_legacy_aggregate(a)
    poses = {}
    names, previous_frame, binding = set(), 0, None
    for row in a["stations"]:
        obj(row, ("name", "camera", "settle_seconds", "ready_within_deadline", "paired_capture_preserved", "snapshot", "legacy", "ladder"), "station")
        require(type(row["name"]) is str and row["name"] and row["name"] not in names, "missing/duplicate station name")
        names.add(row["name"])
        camera = obj(row["camera"], ("world_x", "world_y", "world_z", "yaw_degrees", "pitch_degrees"), "camera")
        xyz = [number(camera[f], f) for f in ("world_x", "world_y", "world_z")]
        for f in ("yaw_degrees", "pitch_degrees"):
            number(camera[f], f)
        validate_station_pose(row["name"], camera)
        poses[row["name"]] = camera
        camera_domain(xyz[0], xyz[2])
        require(0 <= number(row["settle_seconds"], "settle_seconds") <= 90, "station timeout")
        require(boolean(row["ready_within_deadline"], "ready_within_deadline"), "station not ready")
        require(boolean(row["paired_capture_preserved"], "paired_capture_preserved"), "paired ON capture not preserved")
        ladder = obj(row["ladder"], ("wanted_error", "expected_tiles", "expected_by_tier", "resident_tiles", "stale_tiles"), "ladder")
        require(ladder["wanted_error"] == "None", "wanted-set refusal")
        expected = set(oracle(xyz[0], xyz[2]))
        require(keys(ladder["expected_tiles"], "expected_tiles") == expected, "independent oracle disagreement")
        counts = ladder["expected_by_tier"]
        require(type(counts) is list and len(counts) == 5, "omitted expected tier")
        require(all(integer(v, "expected_by_tier") == sum(key[0] == i for key in expected) for i, v in enumerate(counts, 1)), "oracle tier-count disagreement")
        require(integer(ladder["stale_tiles"], "stale_tiles") == 0, "stale-generation tiles")
        resident = keys(ladder["resident_tiles"], "resident_tiles")
        require(resident == (expected if renderer == "ladder" else set()), "missing_expected resident tile")
        s = row["snapshot"]
        validate_snapshot(s, renderer, xyz, expected)
        require(s["frame_id"] > previous_frame, "stale frame snapshot")
        previous_frame = s["frame_id"]
        identity_binding = tuple(s[f] for f in ("world_epoch", "content_identity", "bake_generation",
                                               "requested_cave_policy", "bound_cave_policy", "numeric_policy"))
        require(binding is None or binding == identity_binding, "stale station binding")
        binding = identity_binding
        if renderer == "legacy":
            legacy = obj(row["legacy"], ("regions_wanted", "regions_resident", "regions_missing"), "legacy")
            require(integer(legacy["regions_wanted"], "regions_wanted") == legacy_count(xyz[0], xyz[2]), "legacy oracle disagreement")
            require(integer(legacy["regions_missing"], "regions_missing") == 0, "missing legacy regions")
            require(integer(legacy["regions_resident"], "regions_resident") >= legacy["regions_wanted"], "incomplete legacy residency")
    require(required_stations is None or names == set(required_stations), "missing required station")
    validate_station_heights(poses)
    return {"Mode": renderer, "LadderChecked": renderer == "ladder", "MissingExpected": 0}


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON field {key}")
        result[key] = value
    return result


def read_artifact(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=no_duplicates,
                          parse_constant=lambda value: (_ for _ in ()).throw(ArtifactError(f"nonfinite {value}")))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ArtifactError(f"missing/malformed/truncated artifact: {error}") from error


def main():
    p = argparse.ArgumentParser()
    p.add_argument("command", choices=("wanted", "legacy", "validate"))
    p.add_argument("values", nargs="*")
    p.add_argument("--renderer", choices=("legacy", "ladder"), default="legacy")
    p.add_argument("--station", action="append")
    args = p.parse_args()
    try:
        if args.command == "validate":
            require(len(args.values) == 1, "validate requires artifact path")
            value = validate_artifact(read_artifact(args.values[0]), args.renderer, args.station or STATIONS)
        else:
            require(len(args.values) == 2, "oracle requires X Z")
            x, z = map(float, args.values)
            value = ([f"{tier},{tx},{tz}" for tier, tx, tz in wanted_tiles(x, z)] if args.command == "wanted" else legacy_count(x, z))
        print(json.dumps(value))
        return 0
    except (ArtifactError, ValueError) as error:
        p.exit(1, f"far horizon refused: {error}\n")


if __name__ == "__main__":
    main()
