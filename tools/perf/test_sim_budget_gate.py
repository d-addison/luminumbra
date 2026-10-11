#!/usr/bin/env python3
"""Sim budget gate tests over synthetic capture directories; no engine or third-party modules."""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import tempfile
from typing import Any
import unittest

import capture_sim_budget as capture
import check_sim_budgets
import propose_sim_budgets
import sim_budget_compare
import sim_budget_io


PRESETS = capture.PRESETS
HASH = "0123456789abcdef"
ZERO_HASH = "0" * 64
DURATION = {"p50": 0.5, "p95": 0.95, "p99": 0.99, "maximum": 1.0}


def artifact_for(preset: str, ticks: int, work_scale: int) -> dict[str, Any]:
    stages = []
    for name in capture.STAGES:
        if name == "wind_weather":
            stages.append({"name": name, "samples": 0, "work_total": 0,
                           "work_trace": [], "duration_ms": None})
            continue
        trace = [{"tick": tick, "work": tick * work_scale} for tick in range(1, ticks + 1)]
        stages.append({"name": name, "samples": ticks,
                       "work_total": sum(sample["work"] for sample in trace),
                       "work_trace": trace, "duration_ms": dict(DURATION)})
    return {
        "schema": "luminumbra.server_tick.v1", "preset": preset, "seed": "1337",
        "ticks_requested": ticks, "passed": True,
        "world_hash": HASH, "world_hash_replay": HASH,
        "sim_budget": {"schema": "luminumbra.sim_budget.v2", "work_replay_match": True,
                       "stages": stages},
    }


def write_capture(directory: Path, ticks: int = 4, work_scale: int = 1,
                  presets: tuple[str, ...] = PRESETS,
                  overrides: dict[str, Any] | None = None) -> Path:
    """Write a synthetic capture whose manifest satisfies sim_budget_io.validate_manifest.

    Overrides replace manifest values so tests can inject bad identity or types.
    """
    directory.mkdir(parents=True, exist_ok=True)
    runs = []
    for preset in presets:
        name = f"{preset}.json"
        (directory / name).write_text(
            json.dumps(artifact_for(preset, ticks, work_scale), indent=2), encoding="utf-8")
        runs.append({"preset": preset, "artifact": name, "command": ["synthetic"]})
    manifest = {
        "schema": "luminumbra.sim_budget_capture.v1", "revision": "a" * 40,
        "source_dirty": False, "binary_sha256": ZERO_HASH, "fixture_hash": HASH,
        "seed": "1337", "ticks": ticks, "surface_radius": 1, "collision_radius": 1,
        "runs": runs,
    }
    manifest.update(overrides or {})
    (directory / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                            encoding="utf-8")
    return directory


def run_tool(module: Any, argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = module.main(argv)
    return code, out.getvalue(), err.getvalue()


def emit_budgets(capture_dir: Path, budgets: Path) -> None:
    code, _, err = run_tool(propose_sim_budgets, [str(capture_dir), "--output",
                                                  str(budgets.with_suffix(".proposal.json")),
                                                  "--emit-budgets", str(budgets)])
    if code != 0:
        raise AssertionError(err)


def read_budgets(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


class SimBudgetGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def capture_dir(self, name: str, **kwargs: Any) -> Path:
        return write_capture(self.root / name, **kwargs)

    def test_emitted_budgets_carry_radius_identity(self) -> None:
        directory = self.capture_dir("radius-identity", overrides={"surface_radius": 2,
                                                                   "collision_radius": 3})
        budgets = self.root / "radius-identity-budgets.json"
        emit_budgets(directory, budgets)
        identity = read_budgets(budgets)["identity"]
        self.assertEqual(identity["surface_radius"], 2)
        self.assertEqual(identity["collision_radius"], 3)

    def test_surface_or_collision_radius_mismatch_refuses_with_exit_two(self) -> None:
        budgets = self.root / "radius-base-budgets.json"
        emit_budgets(self.capture_dir("radius-base"), budgets)
        for key in ("surface_radius", "collision_radius"):
            with self.subTest(key=key):
                other = self.capture_dir(f"radius-{key}", overrides={key: 2})
                code, out, err = run_tool(check_sim_budgets, [str(other), str(budgets)])
                self.assertEqual(code, 2)
                self.assertEqual(out, "")
                self.assertIn(key, err)

    def test_null_budget_radius_is_not_pinned_and_skips_the_check(self) -> None:
        budgets = self.root / "radius-null-budgets.json"
        emit_budgets(self.capture_dir("radius-null-base"), budgets)
        data = read_budgets(budgets)
        data["identity"]["surface_radius"] = None
        data["identity"]["collision_radius"] = None
        budgets.write_text(json.dumps(data), encoding="utf-8")
        other = self.capture_dir("radius-null-other", overrides={"surface_radius": 2,
                                                                 "collision_radius": 2})
        code, _, _ = run_tool(check_sim_budgets, [str(other), str(budgets)])
        self.assertEqual(code, 0)
        # The committed baseline pins ticks 600, so its capture must match that tick count.
        baseline_other = self.capture_dir("radius-null-baseline", ticks=600,
                                          overrides={"surface_radius": 2, "collision_radius": 2})
        code, _, err = run_tool(check_sim_budgets, [str(baseline_other), str(capture_baseline())])
        self.assertEqual(code, 0, err)

    def test_null_fixture_hash_is_refused_by_propose_check_and_compare(self) -> None:
        budgets = self.root / "null-fixture-budgets.json"
        emit_budgets(self.capture_dir("null-fixture-base"), budgets)
        nulled = self.capture_dir("null-fixture", overrides={"fixture_hash": None})
        code, out, err = run_tool(propose_sim_budgets, [str(nulled)])
        self.assertEqual((code, out), (2, ""))
        self.assertIn("fixture_hash", err)
        code, out, _ = run_tool(check_sim_budgets, [str(nulled), str(budgets)])
        self.assertEqual((code, out), (2, ""))
        code, out, _ = run_tool(sim_budget_compare, [str(nulled), str(nulled)])
        self.assertEqual((code, out), (2, ""))

    def test_reordered_presets_compare_as_the_same_set(self) -> None:
        before = self.capture_dir("order-before")
        after = self.capture_dir("order-after", presets=tuple(reversed(PRESETS)))
        code, out, err = run_tool(sim_budget_compare, [str(before), str(after)])
        self.assertEqual(code, 0, err)
        report = json.loads(out)
        self.assertEqual(set(report["presets"]), set(PRESETS))

    def test_mistyped_manifest_values_are_refused_by_every_tool(self) -> None:
        bad_values = [
            ("revision", None), ("revision", ""), ("revision", 5),
            ("source_dirty", "false"), ("source_dirty", 0), ("source_dirty", None),
            ("binary_sha256", None), ("binary_sha256", "0" * 63), ("binary_sha256", "A" * 64),
            ("binary_sha256", 5), ("fixture_hash", None), ("fixture_hash", ""),
            ("fixture_hash", 5),
            ("surface_radius", None), ("surface_radius", 0), ("surface_radius", -1),
            ("surface_radius", True), ("surface_radius", "1"), ("surface_radius", 1.0),
            ("collision_radius", None), ("collision_radius", 0), ("collision_radius", True),
            ("collision_radius", "1"), ("collision_radius", 1.0),
        ]
        budgets = self.root / "mistyped-budgets.json"
        emit_budgets(self.capture_dir("mistyped-base"), budgets)
        for index, (key, value) in enumerate(bad_values):
            with self.subTest(key=key, value=value):
                bad = self.capture_dir(f"mistyped-{index}", overrides={key: value})
                with self.assertRaises(ValueError):
                    sim_budget_io.load_capture(bad)
                code, out, _ = run_tool(propose_sim_budgets, [str(bad)])
                self.assertEqual((code, out), (2, ""))
                code, out, _ = run_tool(check_sim_budgets, [str(bad), str(budgets)])
                self.assertEqual((code, out), (2, ""))
                code, out, _ = run_tool(sim_budget_compare, [str(bad), str(bad)])
                self.assertEqual((code, out), (2, ""))

    def test_load_capture_reads_presets_in_manifest_order(self) -> None:
        loaded = sim_budget_io.load_capture(self.capture_dir("base"))
        self.assertEqual(list(loaded["presets"]), list(PRESETS))
        stages = loaded["presets"]["default"]["stages"]
        self.assertEqual(stages["wind_weather"]["samples"], 0)
        self.assertEqual(stages["wind_weather"]["trace"], [])
        self.assertEqual(stages["creatures"]["trace"], [1, 2, 3, 4])
        self.assertEqual(stages["creatures"]["work_total"], 10)
        self.assertEqual(stages["creatures"]["p99_ms"], 0.99)

    def test_load_capture_refuses_corrupt_and_mismatched_artifacts(self) -> None:
        corrupt = self.capture_dir("corrupt")
        (corrupt / "mountains.json").write_text("{not json", encoding="utf-8")
        with self.assertRaises(ValueError):
            sim_budget_io.load_capture(corrupt)

        mismatched = self.capture_dir("mismatched")
        path = mismatched / "archipelago.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        data["seed"] = "999"
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(ValueError):
            sim_budget_io.load_capture(mismatched)

        missing = self.capture_dir("missing")
        (missing / "default.json").unlink()
        with self.assertRaises(ValueError):
            sim_budget_io.load_capture(missing)

    def test_load_capture_refuses_duplicate_preset_runs(self) -> None:
        directory = self.capture_dir("duplicate")
        manifest_path = directory / "capture.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["runs"].append(dict(manifest["runs"][0]))
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(ValueError):
            sim_budget_io.load_capture(directory)

    def test_headroom_arithmetic_is_integer_ceiling(self) -> None:
        self.assertEqual(sim_budget_io.headroom(7, 3, 2), 11)
        self.assertEqual(sim_budget_io.headroom(0, 3, 2), 0)
        self.assertEqual(sim_budget_io.headroom(2, 3, 2), 3)
        with self.assertRaises(ValueError):
            sim_budget_io.headroom(7, 3, 0)
        with self.assertRaises(ValueError):
            sim_budget_io.headroom(7, 0, 2)

    def test_proposal_is_deterministic_and_marks_inactive_stage(self) -> None:
        directory = self.capture_dir("propose")
        first, second = self.root / "first.json", self.root / "second.json"
        self.assertEqual(run_tool(propose_sim_budgets, [str(directory), "--output", str(first)])[0], 0)
        self.assertEqual(run_tool(propose_sim_budgets, [str(directory), "--output", str(second)])[0], 0)
        self.assertEqual(first.read_bytes(), second.read_bytes())

        proposal = json.loads(first.read_text(encoding="utf-8"))
        self.assertEqual(proposal["schema"], "luminumbra.sim_budget_proposal.v1")
        self.assertEqual(proposal["headroom"], {"num": 3, "den": 2})
        self.assertIsNone(proposal["region_scheduler"]["work_limit"])
        default = proposal["presets"]["default"]
        self.assertEqual(default["stages"]["wind_weather"]["status"], "inactive")
        self.assertIsNone(default["stages"]["wind_weather"]["proposed_max_work_per_tick"])
        creatures = default["stages"]["creatures"]
        self.assertEqual(creatures["status"], "observed")
        self.assertEqual(creatures["observed_max_work_per_tick"], 4)
        self.assertEqual(creatures["proposed_max_work_per_tick"], 6)
        active = len(capture.STAGES) - 1
        self.assertEqual(default["total"]["observed_max_work_per_tick"], 4 * active)
        self.assertEqual(default["total"]["proposed_max_work_per_tick"],
                         sim_budget_io.headroom(4 * active, 3, 2))

    def test_proposal_refuses_invalid_capture_without_traceback(self) -> None:
        directory = self.capture_dir("refused")
        (directory / "capture.json").write_text("{", encoding="utf-8")
        code, out, err = run_tool(propose_sim_budgets, [str(directory)])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertTrue(err.startswith("Proposal refused: "))

    def test_same_capture_passes_against_its_own_budgets(self) -> None:
        directory = self.capture_dir("same")
        budgets = self.root / "same-budgets.json"
        emit_budgets(directory, budgets)
        code, out, _ = run_tool(check_sim_budgets, [str(directory), str(budgets),
                                                    "--require-complete"])
        self.assertEqual(code, 0)
        report = json.loads(out)
        self.assertEqual(report["schema"], "luminumbra.sim_budget_check.v1")
        self.assertEqual(report["summary"]["fail"], 0)
        self.assertEqual(report["summary"]["inactive"], len(PRESETS))
        self.assertEqual(report["presets"]["default"]["stages"]["wind_weather"]["verdict"], "inactive")

    def test_doubled_work_fails_even_though_durations_are_unchanged(self) -> None:
        budgets = self.root / "base-budgets.json"
        emit_budgets(self.capture_dir("base"), budgets)
        doubled = self.capture_dir("doubled", work_scale=2)
        code, out, _ = run_tool(check_sim_budgets, [str(doubled), str(budgets)])
        self.assertEqual(code, 1)
        report = json.loads(out)
        self.assertEqual(report["presets"]["default"]["stages"]["creatures"]["verdict"], "fail")
        self.assertGreater(report["summary"]["fail"], 0)

    def test_unpinned_budgets_report_but_only_fail_or_require_complete_gates(self) -> None:
        directory = self.capture_dir("unpinned")
        budgets = self.root / "unpinned.json"
        committed = read_budgets(capture_baseline())
        committed["identity"] = {"revision": None, "fixture_hash": None,
                                 "seed": None, "ticks": None}
        budgets.write_text(json.dumps(committed), encoding="utf-8")
        code, out, _ = run_tool(check_sim_budgets, [str(directory), str(budgets)])
        self.assertEqual(code, 0)
        self.assertGreater(json.loads(out)["summary"]["unpinned"], 0)
        code, _, _ = run_tool(check_sim_budgets, [str(directory), str(budgets),
                                                  "--require-complete"])
        self.assertEqual(code, 3)

    def test_identity_mismatch_refuses_with_exit_two(self) -> None:
        directory = self.capture_dir("identity")
        budgets = self.root / "identity.json"
        emit_budgets(directory, budgets)
        data = read_budgets(budgets)
        data["identity"]["ticks"] = 600
        budgets.write_text(json.dumps(data), encoding="utf-8")
        code, out, err = run_tool(check_sim_budgets, [str(directory), str(budgets)])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("different workload", err)

    def test_stage_missing_from_budgets_is_unbudgeted_and_extra_is_missing(self) -> None:
        directory = self.capture_dir("partial")
        budgets = self.root / "partial.json"
        emit_budgets(directory, budgets)
        data = read_budgets(budgets)
        del data["presets"]["default"]["stages"]["creatures"]
        data["presets"]["default"]["stages"]["retired"] = {"max_work_per_tick": 5}
        budgets.write_text(json.dumps(data), encoding="utf-8")
        code, out, _ = run_tool(check_sim_budgets, [str(directory), str(budgets)])
        self.assertEqual(code, 0)
        default = json.loads(out)["presets"]["default"]
        self.assertEqual(default["stages"]["creatures"]["verdict"], "unbudgeted")
        self.assertEqual(default["missing"], ["retired"])
        code, _, _ = run_tool(check_sim_budgets, [str(directory), str(budgets),
                                                  "--require-complete"])
        self.assertEqual(code, 3)

    def test_committed_baseline_is_structurally_unpinned(self) -> None:
        data = read_budgets(capture_baseline())
        self.assertEqual(data["schema"], "luminumbra.sim_budgets.v1")
        self.assertEqual(data["identity"]["seed"], "1337")
        self.assertEqual(data["identity"]["ticks"], 600)
        for key in ("fixture_hash", "revision", "surface_radius", "collision_radius"):
            self.assertIsNone(data["identity"][key], key)
        self.assertEqual(set(data["presets"]), set(PRESETS))
        for preset in PRESETS:
            entry = data["presets"][preset]
            self.assertEqual(set(entry["stages"]), set(capture.STAGES))
            self.assertIsNone(entry["total"]["max_work_per_tick"])
            for stage in entry["stages"].values():
                self.assertIsNone(stage["max_work_per_tick"])

    def test_compare_same_captures_are_unchanged(self) -> None:
        before = self.capture_dir("cmp-before")
        after = self.capture_dir("cmp-after")
        code, out, _ = run_tool(sim_budget_compare, [str(before), str(after)])
        self.assertEqual(code, 0)
        report = json.loads(out)
        self.assertEqual(report["schema"], "luminumbra.sim_budget_compare.v1")
        for stages in report["presets"].values():
            for row in stages.values():
                self.assertEqual(row["work"], "same")
                self.assertEqual(row["p99_ratio"], 1.0 if row["work_total_before"] else None)

    def test_compare_changed_work_fails_only_for_expected_stage(self) -> None:
        before = self.capture_dir("changed-before")
        after = self.capture_dir("changed-after", work_scale=2)
        code, out, _ = run_tool(sim_budget_compare, [str(before), str(after),
                                                     "--expect-unchanged-work", "wind"])
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out)["presets"]["default"]["wind"]["work"], "different")
        code, _, _ = run_tool(sim_budget_compare, [str(before), str(after),
                                                   "--expect-unchanged-work", "wind_weather"])
        self.assertEqual(code, 0)

    def test_compare_refuses_mismatched_manifests_and_unknown_stage(self) -> None:
        before = self.capture_dir("refuse-before")
        other_ticks = self.capture_dir("refuse-ticks", ticks=5)
        code, _, err = run_tool(sim_budget_compare, [str(before), str(other_ticks)])
        self.assertEqual(code, 2)
        self.assertIn("ticks", err)
        fewer = self.capture_dir("refuse-presets", presets=("default", "mountains"))
        code, _, _ = run_tool(sim_budget_compare, [str(before), str(fewer)])
        self.assertEqual(code, 2)
        code, _, err = run_tool(sim_budget_compare, [str(before), str(before),
                                                     "--expect-unchanged-work", "nonsense"])
        self.assertEqual(code, 2)
        self.assertIn("nonsense", err)


def capture_baseline() -> Path:
    return Path(__file__).resolve().parent / "baselines" / "sim-budgets.json"


if __name__ == "__main__":
    unittest.main()
