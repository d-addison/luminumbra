#!/usr/bin/env python3
"""Capture repeatable populated simulation measurements without enforcing budgets."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import os
from pathlib import Path
import subprocess
from typing import Any

from resolved_fixture import PRESETS, resolved_fixture_hash as capture_fixture_hash, unique_object


STAGES = (
    "server_physics", "animation", "instinct", "perception", "scent", "locomotion",
    "creatures", "wind", "weather", "wind_weather", "aether", "energy", "irrigation", "soil", "plants",
    "pollination", "disease", "crops", "fire", "grazing", "lifespan", "alarm", "decay",
    "circadian", "territory", "packs", "migration", "events", "server_streaming",
    "server_water",
)


def integer(value: Any) -> bool:
    return type(value) is int and 0 <= value < 2**64


REGION_SCHEMA = "luminumbra.sim_budget.regions.v1"
MAX_REGION_SAMPLES = 108000
MAX_REGION_SAVE_SAMPLES = 4096
MAX_REGION_BLOCK_BYTES = 16383
REGION_COUNTS = {
    "scheduler": ("due_total", "accepted_work_total", "work_limit", "pressure_max",
                  "rejected_anchors", "rejected_work", "rejected_capacity"),
    "creatures": ("full", "coarse", "frozen", "promotions", "demotions", "full_updates",
                  "coarse_updates", "births", "deaths", "removals", "parked_travel"),
    "population": ("successes", "failures", "pending_activations"),
    "controllers": ("created", "removed", "stale_handle_rejections"),
    "persistence": ("saves_total", "failures_total", "save_samples"),
    "water": ("reserved", "used", "unused", "unallocated", "completed_cells"),
}


def finite_duration(value: Any) -> bool:
    if type(value) not in (int, float):
        return False
    try:
        return value >= 0 and math.isfinite(value)
    except OverflowError:
        return False


def validate_distribution(value: Any, samples: int) -> None:
    if samples == 0:
        if value is not None:
            raise ValueError("empty regional distribution must be null")
        return
    if not isinstance(value, dict):
        raise ValueError("missing regional duration distribution")
    values = [value.get(key) for key in ("p50", "p95", "p99", "maximum")]
    if (any(not finite_duration(v) for v in values)
            or values != sorted(values)):
        raise ValueError("invalid regional duration distribution")


def validate_regions(block: Any, ticks: int | None = None) -> None:
    """Validate the bounded extension, including all shipping observation groups."""
    if not isinstance(block, dict) or block.get("schema") != REGION_SCHEMA:
        raise ValueError("missing or unsupported regions schema")
    # Count every extension key too. Bound before inspecting any variable-size fields.
    try:
        pending = [block]
        visited = 0
        while pending:
            value = pending.pop()
            visited += 1
            if visited > MAX_REGION_BLOCK_BYTES:
                raise ValueError("regional structure exceeds byte cap")
            if isinstance(value, (str, list, dict)) and len(value) > MAX_REGION_BLOCK_BYTES:
                raise ValueError("regional field exceeds byte cap")
            if isinstance(value, dict):
                if any(not isinstance(key, str) or len(key) > MAX_REGION_BLOCK_BYTES for key in value):
                    raise ValueError("invalid regional object key")
                if len(pending) + len(value) > MAX_REGION_BLOCK_BYTES:
                    raise ValueError("regional structure exceeds byte cap")
                pending.extend(value.values())
            elif isinstance(value, list):
                if len(pending) + len(value) > MAX_REGION_BLOCK_BYTES:
                    raise ValueError("regional structure exceeds byte cap")
                pending.extend(value)
        encoded_size = 0
        encoder = json.JSONEncoder(ensure_ascii=False, allow_nan=False, indent=8)
        for fragment in encoder.iterencode(block):
            encoded_size += len(fragment.encode("utf-8"))
            if encoded_size > MAX_REGION_BLOCK_BYTES:
                raise ValueError("regions block exceeds byte cap")
    except (TypeError, ValueError, OverflowError, RecursionError) as exc:
        raise ValueError("invalid regional JSON") from exc
    for name in ("samples", "dropped_samples"):
        if not integer(block.get(name)):
            raise ValueError(f"invalid regions {name}")
    samples = block["samples"]
    if (samples > MAX_REGION_SAMPLES or block.get("complete") is not True
            or block["dropped_samples"] != 0):
        raise ValueError("incomplete or over-cap regions capture")
    if samples == 0:
        if block.get("first_tick", 0) is not None or block.get("last_tick", 0) is not None:
            raise ValueError("empty regions capture has ticks")
    else:
        first, last = block.get("first_tick"), block.get("last_tick")
        if (not integer(first) or first == 0 or not integer(last) or first > last
                or last - first + 1 != samples):
            raise ValueError("invalid regional tick coverage")
    if ticks is not None and (samples != ticks or block.get("first_tick") != 1
                              or block.get("last_tick") != ticks):
        raise ValueError("incomplete requested regional tick coverage")
    if "duration_ms" not in block:
        raise ValueError("missing regional timing")
    validate_distribution(block["duration_ms"], samples)
    for group, names in REGION_COUNTS.items():
        value = block.get(group)
        if not isinstance(value, dict) or any(not integer(value.get(name)) for name in names):
            raise ValueError(f"invalid regional counts: {group}")
    scheduler = block["scheduler"]
    for name in ("ledger_records", "active", "reduced", "frozen"):
        validate_region_gauge(scheduler.get(name))
    validate_region_gauge(block["persistence"].get("parked_bytes"))
    if (sum(scheduler[name]["current"] for name in ("active", "reduced", "frozen"))
            != scheduler["ledger_records"]["current"]
            or any(scheduler[name]["maximum"] > scheduler["ledger_records"]["maximum"]
                   for name in ("active", "reduced", "frozen"))):
        raise ValueError("inconsistent regional state gauges")
    persistence = block["persistence"]
    if (persistence["save_samples"] > MAX_REGION_SAVE_SAMPLES
            or persistence["save_samples"] != persistence["saves_total"]
            or persistence["failures_total"] > persistence["saves_total"]
            or "save_duration_ms" not in persistence):
        raise ValueError("invalid regional save coverage")
    validate_distribution(persistence["save_duration_ms"], persistence["save_samples"])
    water = block["water"]
    if water["reserved"] != water["used"] + water["unused"]:
        raise ValueError("invalid regional water accounting")
    known = water.get("bound_known")
    bound = water.get("bound")
    if (type(known) is not bool or "bound" not in water
            or (known and not integer(bound))
            or (not known and bound is not None)):
        raise ValueError("invalid regional water bound")
    replay = block.get("replay")
    if (not isinstance(replay, dict) or replay.get("work_replay_match") is not True
            or not isinstance(replay.get("trace_digest"), str)
            or re.fullmatch(r"[0-9a-f]{16}", replay["trace_digest"]) is None):
        raise ValueError("regional work did not reproduce")
    if "fault" not in block:
        raise ValueError("missing regional fault status")
    fault = block["fault"]
    if fault is not None:
        if (not isinstance(fault, dict) or not integer(fault.get("tick"))
                or not isinstance(fault.get("message"), str)
                or len(fault["message"].encode("utf-8")) > 256
                or any(ord(c) < 32 or ord(c) >= 127 for c in fault["message"])):
            raise ValueError("invalid regional fault")
        raise ValueError("faulted regional capture")


def validate_region_gauge(value: Any) -> None:
    if (not isinstance(value, dict) or not integer(value.get("current"))
            or not integer(value.get("maximum")) or value["current"] > value["maximum"]):
        raise ValueError("invalid regional gauge")


def shipping_simulation_gates() -> tuple[str, ...]:
    """Use the shipping systems document, ignoring its explanatory comment entries."""
    path = Path(__file__).resolve().parents[2] / "data/common/systems.json"
    systems = json.loads(path.read_text(encoding="utf-8-sig"))
    return tuple(sorted(name for name, gate in systems["sim"].items()
                        if isinstance(gate, dict) and "enabled" in gate))


def validate_artifact(data: dict[str, Any], preset: str, seed: str, ticks: int, require_regions: bool = False) -> None:
    """Ignore unknown object keys; refuse incomplete, corrupt or future evidence."""
    for field in ("world_hash", "world_hash_replay"):
        value = data.get(field)
        if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{16}", value) is None:
            raise ValueError(f"invalid checksum: {field}")
    if (data.get("schema") != "luminumbra.server_tick.v1"
            or data.get("preset") != preset or data.get("seed") != seed
            or not integer(data.get("ticks_requested")) or data["ticks_requested"] != ticks
            or data.get("passed") is not True
            or data.get("world_hash") != data.get("world_hash_replay")):
        raise ValueError("smoke identity, tick count or replay verdict mismatch")
    effective = data.get("effective_simulation")
    required = shipping_simulation_gates() + ("finite_hydrology", "weather_rain")
    if not isinstance(effective, dict) or any(effective.get(key) is not True for key in required):
        raise ValueError("capture did not activate the shipping simulation defaults")
    budget = data.get("sim_budget")
    if not isinstance(budget, dict) or budget.get("schema") != "luminumbra.sim_budget.v2":
        raise ValueError("missing or unsupported sim_budget schema")
    if budget.get("work_replay_match") is not True:
        raise ValueError("simulation work counts did not reproduce")
    if "regions" in budget:
        validate_regions(budget["regions"], ticks)
    elif require_regions:
        raise ValueError("requested regional capture is absent")
    stages = budget.get("stages")
    if (not isinstance(stages, list) or any(not isinstance(s, dict) for s in stages)
            or [s.get("name") for s in stages] != list(STAGES)):
        raise ValueError("missing, duplicate, reordered or unsupported simulation stage")
    by_name = {stage["name"]: stage for stage in stages}
    combined = by_name["wind_weather"].get("samples") == ticks
    unexecuted = {"wind", "weather"} if combined else {"wind_weather"}
    for stage in stages:
        if stage["name"] in unexecuted:
            if (type(stage.get("samples")) is not int or stage["samples"] != 0
                    or type(stage.get("work_total")) is not int or stage["work_total"] != 0
                    or stage.get("work_trace") != [] or "duration_ms" not in stage
                    or stage["duration_ms"] is not None):
                raise ValueError(f"unexpected wind/weather path coverage: {stage['name']}")
            continue
        if not integer(stage.get("samples")) or stage["samples"] != ticks:
            raise ValueError(f"incomplete timer coverage: {stage['name']}")
        trace = stage.get("work_trace")
        if not isinstance(trace, list) or len(trace) != ticks:
            raise ValueError(f"incomplete work trace: {stage['name']}")
        for tick, sample in enumerate(trace, start=1):
            if (not isinstance(sample, dict) or not integer(sample.get("tick")) or sample["tick"] != tick
                    or not integer(sample.get("work"))):
                raise ValueError(f"invalid work sample: {stage['name']}")
        if (not integer(stage.get("work_total"))
                or stage["work_total"] != sum(s["work"] for s in trace)):
            raise ValueError(f"invalid work total: {stage['name']}")
        duration = stage.get("duration_ms")
        if not isinstance(duration, dict):
            raise ValueError(f"missing duration distribution: {stage['name']}")
        values = [duration.get(key) for key in ("p50", "p95", "p99", "maximum")]
        if (any(not finite_duration(v) for v in values)
                or values != sorted(values)):
            raise ValueError(f"invalid duration distribution: {stage['name']}")
    # The capture command promises populated simulation, not just a successful empty tick.
    totals = {stage["name"]: stage["work_total"] for stage in stages}
    required_work = ("creatures", "plants", "scent") + (
        ("wind_weather",) if combined else ("wind", "weather"))
    if any(totals[name] == 0 for name in required_work):
        raise ValueError("capture did not exercise the populated simulation")


def command(binary: Path, root: Path, artifact: Path, preset: str,
            seed: str, ticks: int, radius: int, collision_radius: int) -> list[str]:
    return [str(binary), "--root", str(root), "--smoke", "--no-audio", "--sim-budget",
            "--ecology-roster", "--planted-roster", "--preset", preset, "--seed", seed,
            "--ticks", str(ticks), "--autosave-ticks", "30", "--radius", str(radius),
            "--collision-radius",
            str(collision_radius), "--artifact", str(artifact)]


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def main() -> int:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    binary_name = "luminumbra_server_app.exe" if os.name == "nt" else "luminumbra_server_app"
    parser.add_argument("--binary", type=Path, default=repository / "build/perf/bin" / binary_name)
    parser.add_argument("--root", type=Path, default=repository,
                        help="fixture root, including its effective systems.json")
    parser.add_argument("--regions", action="store_true",
                        help="require complete regional telemetry; fixture must enable sim.active_regions")
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--ticks", type=positive_int, default=3600)
    parser.add_argument("--seed", default="1337")
    parser.add_argument("--radius", type=positive_int, default=1)
    parser.add_argument("--collision-radius", type=positive_int, default=1)
    args = parser.parse_args()
    root = args.root.resolve()
    binary = args.binary.resolve()
    evidence = args.evidence_dir.resolve()
    if not binary.is_file():
        parser.error(f"server executable does not exist: {binary}")
    if evidence.exists():
        parser.error(f"evidence directory already exists: {evidence}")
    if not args.seed:
        parser.error("seed must be explicit and nonempty")
    if args.regions:
        try:
            systems = json.loads((root / "data/common/systems.json").read_text(encoding="utf-8-sig"),
                                 object_pairs_hook=unique_object)
            if systems["sim"]["active_regions"]["enabled"] is not True:
                parser.error("--regions requires sim.active_regions enabled in the fixture root")
        except (OSError, ValueError, KeyError, TypeError) as exc:
            parser.error(f"cannot qualify regional fixture configuration: {exc}")
        if args.ticks > MAX_REGION_SAMPLES:
            parser.error("requested ticks exceed the regional sample cap")
    try:
        fixture_identity = capture_fixture_hash(root)
    except (OSError, ValueError, TypeError, AttributeError, RecursionError) as exc:
        parser.error(f"cannot identify simulation fixture inputs: {exc}")
    evidence.mkdir(parents=True)
    binary_digest = hashlib.sha256()
    with binary.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            binary_digest.update(chunk)
    manifest = {
        "schema": "luminumbra.sim_budget_capture.v1",
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repository, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=repository, text=True)),
        "binary_sha256": binary_digest.hexdigest(),
        "fixture_hash": fixture_identity,
        "seed": args.seed, "ticks": args.ticks, "surface_radius": args.radius,
        "collision_radius": args.collision_radius, "runs": [],
    }
    if args.regions:
        manifest["regions_required"] = True
    try:
        for preset in PRESETS:
            artifact = evidence / f"{preset}.json"
            argv = command(binary, root, artifact, preset, args.seed, args.ticks,
                           args.radius, args.collision_radius)
            print(f"Capturing {preset}: {args.ticks} ticks per replay", flush=True)
            with (evidence / f"{preset}.log").open("wb") as log:
                subprocess.run(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
            data = json.loads(artifact.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)
            validate_artifact(data, preset, args.seed, args.ticks, args.regions)
            manifest["runs"].append({"preset": preset, "artifact": artifact.name, "command": argv})
        if capture_fixture_hash(root) != fixture_identity:
            raise ValueError("simulation fixture inputs changed during capture")
        (evidence / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, TypeError, KeyError, AttributeError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Capture refused: {exc}; incomplete evidence retained in {evidence}\n")
    print(f"Validated all three captures in {evidence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
