#!/usr/bin/env python3
"""Report comparable representative captures as JSON on stdout; never rewrite evidence."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys
from typing import Any

# Support the registered isolated Python invocation as well as direct execution.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_sim_budget as capture
from fixture_paths import reference_parts
import propose_sim_budgets as proposal
import sim_budget_compare
import sim_budget_io as budget_io


REPORT_SCHEMA = "luminumbra.sim_budget_report.v1"
DURATION_KEYS = ("p50", "p95", "p99", "maximum")


def read_artifact(directory: Path, reference: Any) -> dict[str, Any]:
    data = budget_io.read_json(directory.joinpath(*reference_parts(reference)), root=directory)
    if not isinstance(data, dict):
        raise ValueError("artifact is not a JSON object")
    return data


def validate_commands(run: dict[str, Any], manifest: dict[str, Any]) -> None:
    """Require the capture harness workload, allowing endpoint-specific paths."""
    traced = run.get("command")
    disabled = run["telemetry_off"].get("command")
    if (not isinstance(traced, list) or len(traced) != 22
            or any(not isinstance(arg, str) or not arg for arg in traced)):
        raise ValueError("missing or invalid capture command")
    # The emitter only stringifies paths. Keep their authored spelling so a
    # retained capture can be read on a different host without path conversion.
    expected = capture.command(traced[0], traced[2], traced[-1],
                               run["preset"], str(manifest["seed"]),
                               budget_io.validate_workload_integer(manifest["ticks"], "ticks"),
                               manifest["surface_radius"], manifest["collision_radius"])
    if traced != expected:
        raise ValueError("capture command differs from the representative workload")
    expected = traced.copy()
    expected.remove("--sim-budget")
    if (not isinstance(disabled, list) or len(disabled) != len(expected)
            or not isinstance(disabled[-1], str) or not disabled[-1]):
        raise ValueError("missing or invalid telemetry-off command")
    expected[-1] = disabled[-1]
    if disabled != expected:
        raise ValueError("telemetry-off command differs beyond telemetry and output")


def load_report_capture(directory: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    """Use shared validators on each original object, retaining its duration fields.

    The shared loader discards all durations except p99. Read each file once here
    so the report and integer summary use the same validated object. Work is
    bounded by three presets, thirty stages and each artifact's finite tick trace.
    """
    manifest = budget_io.read_json(directory / "capture.json", root=directory)
    if not isinstance(manifest, dict) or manifest.get("schema") != budget_io.CAPTURE_SCHEMA:
        raise ValueError("capture.json has a missing or unsupported schema")
    if any(key not in manifest for key in (*budget_io.MANIFEST_KEYS, "preset_hash")):
        raise ValueError("capture.json lacks required workload identity")
    budget_io.validate_manifest(manifest)
    budget_io.validate_workload_integer(manifest["seed"], "seed")
    ticks = budget_io.validate_workload_integer(manifest["ticks"], "ticks")
    runs = manifest.get("runs")
    if (not isinstance(runs, list) or len(runs) != len(capture.PRESETS)
            or any(not isinstance(run, dict) or not isinstance(run.get("preset"), str)
                   for run in runs)
            or sorted(run["preset"] for run in runs) != sorted(capture.PRESETS)):
        raise ValueError("capture.json must contain all three representative presets exactly once")
    summaries: dict[str, Any] = {}
    presets: dict[str, Any] = {}
    by_preset = {run["preset"]: run for run in runs}
    for preset in capture.PRESETS:
        run = by_preset[preset]
        counterpart = run.get("telemetry_off")
        if not isinstance(counterpart, dict):
            raise ValueError(f"missing telemetry neutrality evidence: {preset}")
        validate_commands(run, manifest)
        traced = read_artifact(directory, run.get("artifact"))
        capture.validate_artifact(traced, preset, str(manifest["seed"]), ticks)
        disabled = read_artifact(directory, counterpart.get("artifact"))
        capture.validate_neutrality(traced, disabled, preset, str(manifest["seed"]), ticks)
        stages = traced["sim_budget"]["stages"]
        summary = budget_io.summarize_stages(stages)
        summaries[preset] = {"stages": summary}
        presets[preset] = {
            "artifact": run["artifact"],
            "telemetry_neutrality": {
                "passed": True, "telemetry_off_artifact": counterpart["artifact"],
                "world_hash": traced["world_hash"],
                "world_hash_replay": traced["world_hash_replay"],
                "telemetry_off_world_hash": disabled["world_hash"],
                "telemetry_off_world_hash_replay": disabled["world_hash_replay"],
                "work_replay_match": traced["sim_budget"]["work_replay_match"],
            },
            "effective_simulation": dict(traced["effective_simulation"]),
            "stages": {
                stage["name"]: {
                    "samples": stage["samples"], "work_total": stage["work_total"],
                    "max_work_per_tick": budget_io.stage_max(summary[stage["name"]]),
                    "duration_ms": ({key: stage["duration_ms"][key] for key in DURATION_KEYS}
                                    if stage["duration_ms"] is not None else None),
                } for stage in stages
            },
        }
    identity = {key: manifest[key] for key in (*budget_io.MANIFEST_KEYS, "preset_hash")}
    for key in ("seed", "ticks"):
        identity[key] = budget_io.validate_workload_integer(manifest[key], key)
    endpoint = {"identity": identity, "presets": presets}
    return {"manifest": manifest, "presets": summaries}, endpoint


def build_report(before_dir: Path, after_dir: Path, num: int = 3, den: int = 2) -> dict[str, Any]:
    before, before_endpoint = load_report_capture(before_dir)
    after, after_endpoint = load_report_capture(after_dir)
    comparison, _ = sim_budget_compare.build_comparison(before, after, set())
    for preset in capture.PRESETS:
        if (before_endpoint["presets"][preset]["effective_simulation"]
                != after_endpoint["presets"][preset]["effective_simulation"]):
            raise ValueError(f"captures differ in effective simulation gates: {preset}")
        # Finite artifact durations can still produce an overflowing ratio.
        # Keep the original measurements and emit null instead of JSON Infinity.
        for row in comparison["presets"][preset].values():
            ratio = row["p99_ratio"]
            if ratio is not None and not math.isfinite(ratio):
                row["p99_ratio"] = None
    proposed = proposal.build_proposal(after, num, den)
    return {
        "schema": REPORT_SCHEMA,
        "durations": "advisory; ratios are after/before and do not gate work budgets",
        "stage_order": list(capture.STAGES),
        "workload": {"ecology_roster": True, "planted_roster": True,
                     "autosave_ticks": 30, "no_audio": True},
        "before": before_endpoint, "after": after_endpoint, "comparison": comparison,
        "proposal": proposed, "integer_proposal": proposal.build_budgets(proposed),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before_dir", type=Path)
    parser.add_argument("after_dir", type=Path)
    parser.add_argument("--headroom-num", type=proposal.positive_int, default=3)
    parser.add_argument("--headroom-den", type=proposal.positive_int, default=2)
    args = parser.parse_args(argv)
    try:
        report = build_report(args.before_dir, args.after_dir, args.headroom_num, args.headroom_den)
        text = proposal.format_json(report)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, OverflowError) as exc:
        print(f"Report refused: {exc}", file=sys.stderr)
        return 2
    try:
        sys.stdout.write(text)
        sys.stdout.flush()
    except OSError as exc:
        print(f"Report refused: cannot write output: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
