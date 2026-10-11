#!/usr/bin/env python3
"""Compare two sim budget captures stage by stage; durations are reported but never gate."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import capture_sim_budget
import sim_budget_io
from propose_sim_budgets import format_json


COMPARE_SCHEMA = "luminumbra.sim_budget_compare.v1"
WORKLOAD_KEYS = ("seed", "ticks", "fixture_hash", "surface_radius", "collision_radius")


def p99_ratio(before: float | None, after: float | None) -> float | None:
    if before is None or after is None or before == 0:
        return None
    return round(after / before, 3)


def build_comparison(before: dict[str, Any], after: dict[str, Any],
                     expected: set[str]) -> tuple[dict[str, Any], int]:
    for key in WORKLOAD_KEYS:
        if before["manifest"].get(key) != after["manifest"].get(key):
            raise ValueError(f"captures differ in {key}")
    if list(before["presets"]) != list(after["presets"]):
        raise ValueError("captures have different preset lists")
    unknown = sorted(expected - set(capture_sim_budget.STAGES))
    if unknown:
        raise ValueError(f"unknown stage in --expect-unchanged-work: {', '.join(unknown)}")
    presets: dict[str, Any] = {}
    expectation_broken = False
    for preset, data in before["presets"].items():
        before_stages = data["stages"]
        after_stages = after["presets"][preset]["stages"]
        if set(before_stages) != set(after_stages):
            raise ValueError(f"captures have different stages for {preset}")
        rows: dict[str, Any] = {}
        for name, old in before_stages.items():
            new = after_stages[name]
            same = old["trace"] == new["trace"]
            if not same and name in expected:
                expectation_broken = True
            rows[name] = {
                "work": "same" if same else "different",
                "work_total_before": old["work_total"],
                "work_total_after": new["work_total"],
                "p99_ratio": p99_ratio(old["p99_ms"], new["p99_ms"]),
            }
        presets[preset] = rows
    report = {"schema": COMPARE_SCHEMA, "presets": presets}
    return report, 1 if expectation_broken else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before_dir", type=Path)
    parser.add_argument("after_dir", type=Path)
    parser.add_argument("--expect-unchanged-work", default="",
                        help="comma-separated stage names whose work must not change")
    args = parser.parse_args(argv)
    expected = {name.strip() for name in args.expect_unchanged_work.split(",") if name.strip()}
    try:
        before = sim_budget_io.load_capture(args.before_dir)
        after = sim_budget_io.load_capture(args.after_dir)
        report, code = build_comparison(before, after, expected)
    except ValueError as exc:
        print(f"Compare refused: {exc}", file=sys.stderr)
        return 2
    sys.stdout.write(format_json(report))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
