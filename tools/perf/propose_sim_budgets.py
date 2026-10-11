#!/usr/bin/env python3
"""Propose integer per-tick work limits from a sim budget capture; durations stay advisory."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import sim_budget_io


PROPOSAL_SCHEMA = "luminumbra.sim_budget_proposal.v1"
BUDGETS_SCHEMA = "luminumbra.sim_budgets.v1"
REGION_SCHEDULER_REASON = (
    "RegionWork units are not derivable from stage telemetry; the game supplies the value"
)


def format_json(data: Any) -> str:
    return json.dumps(data, indent=2, sort_keys=True) + "\n"


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def build_proposal(capture: dict[str, Any], num: int, den: int) -> dict[str, Any]:
    manifest = capture["manifest"]
    presets: dict[str, Any] = {}
    for preset, data in capture["presets"].items():
        stages: dict[str, Any] = {}
        for name, stage in data["stages"].items():
            observed = sim_budget_io.stage_max(stage)
            if stage["samples"] == 0:
                status, proposed = "inactive", None
            elif observed == 0:
                status, proposed = "idle", None
            else:
                status, proposed = "observed", sim_budget_io.headroom(observed, num, den)
            stages[name] = {
                "status": status,
                "observed_max_work_per_tick": observed,
                "proposed_max_work_per_tick": proposed,
                "observed_p99_ms": stage["p99_ms"],
            }
        total_max = max(sim_budget_io.per_tick_totals(data["stages"]), default=0)
        presets[preset] = {
            "stages": stages,
            "total": {
                "observed_max_work_per_tick": total_max,
                "proposed_max_work_per_tick": (
                    sim_budget_io.headroom(total_max, num, den) if total_max else None),
            },
        }
    return {
        "schema": PROPOSAL_SCHEMA,
        "identity": {key: manifest[key] for key in (
            "revision", "fixture_hash", "seed", "ticks", "binary_sha256", "source_dirty",
            "surface_radius", "collision_radius")},
        "headroom": {"num": num, "den": den},
        "presets": presets,
        "region_scheduler": {"work_limit": None, "reason": REGION_SCHEDULER_REASON},
    }


def build_budgets(proposal: dict[str, Any]) -> dict[str, Any]:
    identity = proposal["identity"]
    return {
        "schema": BUDGETS_SCHEMA,
        "identity": {key: identity[key] for key in (
            "revision", "fixture_hash", "seed", "ticks", "surface_radius", "collision_radius")},
        "headroom": dict(proposal["headroom"]),
        "presets": {
            preset: {
                "stages": {name: {"max_work_per_tick": stage["proposed_max_work_per_tick"]}
                           for name, stage in data["stages"].items()},
                "total": {"max_work_per_tick": data["total"]["proposed_max_work_per_tick"]},
            }
            for preset, data in proposal["presets"].items()
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("--headroom-num", type=positive_int, default=3)
    parser.add_argument("--headroom-den", type=positive_int, default=2)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--emit-budgets", type=Path)
    args = parser.parse_args(argv)
    try:
        capture = sim_budget_io.load_capture(args.capture_dir)
        proposal = build_proposal(capture, args.headroom_num, args.headroom_den)
    except ValueError as exc:
        print(f"Proposal refused: {exc}", file=sys.stderr)
        return 2
    text = format_json(proposal)
    try:
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        if args.emit_budgets:
            args.emit_budgets.write_text(format_json(build_budgets(proposal)), encoding="utf-8")
    except OSError as exc:
        print(f"Proposal refused: cannot write output: {exc.strerror or exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
