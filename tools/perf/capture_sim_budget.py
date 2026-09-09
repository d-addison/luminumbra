#!/usr/bin/env python3
"""Capture repeatable populated simulation measurements without enforcing budgets."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
from typing import Any

from fixture_hash import fixture_hash


PRESETS = ("default", "mountains", "archipelago")
STAGES = (
    "server_physics", "animation", "instinct", "perception", "scent", "locomotion",
    "creatures", "wind", "weather", "aether", "energy", "irrigation", "soil", "plants",
    "pollination", "disease", "crops", "fire", "grazing", "lifespan", "alarm", "decay",
    "circadian", "territory", "packs", "migration", "events", "server_streaming",
    "server_water",
)


def integer(value: Any) -> bool:
    return type(value) is int and 0 <= value < 2**64


def validate_artifact(data: dict[str, Any], preset: str, seed: str, ticks: int) -> None:
    """Ignore unknown object keys; refuse incomplete, corrupt or future evidence."""
    if (data.get("schema") != "luminumbra.server_tick.v1"
            or data.get("preset") != preset or data.get("seed") != seed
            or not integer(data.get("ticks_requested")) or data["ticks_requested"] != ticks
            or data.get("passed") is not True
            or not data.get("world_hash")
            or data.get("world_hash") != data.get("world_hash_replay")):
        raise ValueError("smoke identity, tick count or replay verdict mismatch")
    budget = data.get("sim_budget")
    if not isinstance(budget, dict) or budget.get("schema") != "luminumbra.sim_budget.v1":
        raise ValueError("missing or unsupported sim_budget schema")
    if budget.get("work_replay_match") is not True:
        raise ValueError("simulation work counts did not reproduce")
    stages = budget.get("stages")
    if (not isinstance(stages, list) or any(not isinstance(s, dict) for s in stages)
            or [s.get("name") for s in stages] != list(STAGES)):
        raise ValueError("missing, duplicate, reordered or unsupported simulation stage")
    for stage in stages:
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
        if (any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in values)
                or values != sorted(values)):
            raise ValueError(f"invalid duration distribution: {stage['name']}")
    # The capture command promises populated simulation, not just a successful empty tick.
    totals = {stage["name"]: stage["work_total"] for stage in stages}
    if any(totals[name] == 0 for name in ("creatures", "plants", "scent", "wind", "weather")):
        raise ValueError("capture did not exercise the populated simulation")


def command(binary: Path, root: Path, artifact: Path, preset: str,
            seed: str, ticks: int, radius: int, collision_radius: int) -> list[str]:
    return [str(binary), "--root", str(root), "--smoke", "--no-audio", "--sim-budget",
            "--ecology-roster", "--planted-roster", "--preset", preset, "--seed", seed,
            "--ticks", str(ticks), "--radius", str(radius), "--collision-radius",
            str(collision_radius), "--artifact", str(artifact)]


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    binary_name = "luminumbra_server_app.exe" if os.name == "nt" else "luminumbra_server_app"
    parser.add_argument("--binary", type=Path, default=root / "build/perf/bin" / binary_name)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--ticks", type=positive_int, default=3600)
    parser.add_argument("--seed", default="1337")
    parser.add_argument("--radius", type=positive_int, default=1)
    parser.add_argument("--collision-radius", type=positive_int, default=1)
    args = parser.parse_args()
    binary = args.binary.resolve()
    evidence = args.evidence_dir.resolve()
    if not binary.is_file():
        parser.error(f"server executable does not exist: {binary}")
    if evidence.exists():
        parser.error(f"evidence directory already exists: {evidence}")
    if not args.seed:
        parser.error("seed must be explicit and nonempty")
    evidence.mkdir(parents=True)
    binary_digest = hashlib.sha256()
    with binary.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            binary_digest.update(chunk)
    manifest = {
        "schema": "luminumbra.sim_budget_capture.v1",
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True)),
        "binary_sha256": binary_digest.hexdigest(),
        "fixture_hash": fixture_hash(root / "data/common", scope="headless-server"),
        "seed": args.seed, "ticks": args.ticks, "surface_radius": args.radius,
        "collision_radius": args.collision_radius, "runs": [],
    }
    try:
        for preset in PRESETS:
            artifact = evidence / f"{preset}.json"
            argv = command(binary, root, artifact, preset, args.seed, args.ticks,
                           args.radius, args.collision_radius)
            print(f"Capturing {preset}: {args.ticks} ticks per replay", flush=True)
            with (evidence / f"{preset}.log").open("wb") as log:
                subprocess.run(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
            data = json.loads(artifact.read_text(encoding="utf-8-sig"))
            validate_artifact(data, preset, args.seed, args.ticks)
            manifest["runs"].append({"preset": preset, "artifact": artifact.name, "command": argv})
        (evidence / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, TypeError, KeyError, AttributeError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Capture refused: {exc}; incomplete evidence retained in {evidence}\n")
    print(f"Validated all three captures in {evidence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
