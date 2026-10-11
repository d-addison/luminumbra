#!/usr/bin/env python3
"""Check a sim budget capture against pinned per-tick work limits; durations are advisory only."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import sim_budget_io
from propose_sim_budgets import format_json


BUDGETS_SCHEMA = "luminumbra.sim_budgets.v1"
CHECK_SCHEMA = "luminumbra.sim_budget_check.v1"
VERDICTS = ("pass", "fail", "unpinned", "unbudgeted", "inactive", "missing")


def check_limit(entry: Any, label: str) -> None:
    if not isinstance(entry, dict) or "max_work_per_tick" not in entry:
        raise ValueError(f"budget entry lacks max_work_per_tick: {label}")
    limit = entry["max_work_per_tick"]
    if limit is not None and (type(limit) is not int or limit < 0):
        raise ValueError(f"budget limit must be null or a non-negative integer: {label}")


def load_budgets(path: Path) -> dict[str, Any]:
    data = sim_budget_io.read_json(path)
    if not isinstance(data, dict) or data.get("schema") != BUDGETS_SCHEMA:
        raise ValueError("budgets file has a missing or unsupported schema")
    if not isinstance(data.get("identity"), dict):
        raise ValueError("budgets file lacks identity")
    presets = data.get("presets")
    if not isinstance(presets, dict):
        raise ValueError("budgets file lacks presets")
    for preset, entry in presets.items():
        if (not isinstance(entry, dict) or not isinstance(entry.get("stages"), dict)
                or not isinstance(entry.get("total"), dict)):
            raise ValueError(f"malformed budget entry: {preset}")
        for name, stage in entry["stages"].items():
            check_limit(stage, f"{preset}/{name}")
        check_limit(entry["total"], f"{preset}/total")
    return data


def identity_mismatch(identity: dict[str, Any], manifest: dict[str, Any]) -> str | None:
    for key in ("seed", "ticks", "fixture_hash"):
        expected = identity.get(key)
        if expected is not None and manifest.get(key) != expected:
            return f"budgets {key} {expected!r} differs from capture {manifest.get(key)!r}"
    return None


def judge(observed: int, limit: int | None) -> str:
    if limit is None:
        return "unpinned"
    return "pass" if observed <= limit else "fail"


def build_report(capture: dict[str, Any], budgets: dict[str, Any],
                 require_complete: bool) -> tuple[dict[str, Any], int]:
    captured = capture["presets"]
    budgeted = budgets["presets"]
    names = list(captured) + [preset for preset in budgeted if preset not in captured]
    counts = dict.fromkeys(VERDICTS, 0)
    presets: dict[str, Any] = {}
    for preset in names:
        observed_stages = captured.get(preset, {"stages": {}})["stages"]
        budget = budgeted.get(preset)
        stages: dict[str, Any] = {}
        for name, stage in observed_stages.items():
            entry = None if budget is None else budget["stages"].get(name)
            limit = None if entry is None else entry["max_work_per_tick"]
            observed = sim_budget_io.stage_max(stage)
            if stage["samples"] == 0:
                verdict = "inactive"
            elif entry is None:
                verdict = "unbudgeted"
            else:
                verdict = judge(observed, limit)
            stages[name] = {"verdict": verdict, "observed_max": observed, "limit": limit}
            counts[verdict] += 1
        missing = [] if budget is None else sorted(
            name for name in budget["stages"] if name not in observed_stages)
        counts["missing"] += len(missing)
        totals = sim_budget_io.per_tick_totals(observed_stages)
        total_max = max(totals, default=0)
        total_limit = None if budget is None else budget["total"]["max_work_per_tick"]
        if preset not in captured:
            total = {"verdict": "missing", "observed_max": None, "limit": total_limit}
        elif budget is None:
            total = {"verdict": "unbudgeted", "observed_max": total_max, "limit": None}
        else:
            total = {"verdict": judge(total_max, total_limit),
                     "observed_max": total_max, "limit": total_limit}
        counts[total["verdict"]] += 1
        presets[preset] = {"stages": stages, "total": total, "missing": missing}
    report = {
        "schema": CHECK_SCHEMA,
        "presets": presets,
        "summary": counts,
        "durations": "advisory only, never enforced",
    }
    incomplete = counts["unpinned"] + counts["unbudgeted"] + counts["missing"] > 0
    if counts["fail"]:
        return report, 1
    if require_complete and incomplete:
        return report, 3
    return report, 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("budgets", type=Path)
    parser.add_argument("--require-complete", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        capture = sim_budget_io.load_capture(args.capture_dir)
        budgets = load_budgets(args.budgets)
        mismatch = identity_mismatch(budgets["identity"], capture["manifest"])
        if mismatch:
            raise ValueError(f"budgets were measured under a different workload: {mismatch}")
    except ValueError as exc:
        print(f"Check refused: {exc}", file=sys.stderr)
        return 2
    report, code = build_report(capture, budgets, args.require_complete)
    text = format_json(report)
    try:
        if args.output:
            args.output.write_text(text, encoding="utf-8")
    except OSError as exc:
        print(f"Check refused: cannot write output: {exc.strerror or exc}", file=sys.stderr)
        return 2
    sys.stdout.write(text)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
