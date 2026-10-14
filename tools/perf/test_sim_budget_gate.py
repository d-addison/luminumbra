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
FIXTURE_HASH = HASH * 4
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
        "effective_simulation": dict.fromkeys(("active_regions", "aether_state", "water_high_res",
            "finite_hydrology", "weather_rain", "weather_events"), True),
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
        "source_dirty": False, "binary_sha256": ZERO_HASH, "fixture_hash": FIXTURE_HASH,
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

    def assert_workload_acceptance(self, directory: Path, reference: Path, budgets: Path,
                                   key: str, accepted: bool) -> None:
        try:
            sim_budget_io.load_capture(directory)
        except ValueError as exc:
            self.assertFalse(accepted, str(exc))
            self.assertIn(key, str(exc))
            loader_accepted = False
        else:
            loader_accepted = True
        outcomes = {"loader": loader_accepted}
        for module, argv in (
                (propose_sim_budgets, [str(directory)]),
                (check_sim_budgets, [str(directory), str(budgets), "--require-complete"]),
                (sim_budget_compare, [str(reference), str(directory)])):
            code, out, err = run_tool(module, argv)
            self.assertEqual(code, 0 if accepted else 2, err)
            if not accepted:
                self.assertEqual(out, "")
                self.assertIn(key, err)
            outcomes[module.__name__] = code == 0
        self.assertEqual(outcomes, dict.fromkeys(outcomes, accepted))

    def test_seed_and_ticks_syntax_is_consistent_across_tools(self) -> None:
        # Match artifact seed spelling too: rejection must come from syntax validation.
        for key in ("seed", "ticks"):
            reference = self.capture_dir(f"syntax-reference-{key}", ticks=1337,
                                         presets=("default",))
            budgets = self.root / f"syntax-budgets-{key}.json"
            emit_budgets(reference, budgets)
            for index, value in enumerate(("1_337", "١٣٣٧", " 1337", "+1337", "1337",
                                           "1337 ", "1337\n", "-1337", "", "1337.0",
                                           "01337", 1337)):
                with self.subTest(key=key, value=value):
                    directory = self.capture_dir(f"syntax-{key}-{index}", ticks=1337,
                                                 presets=("default",), overrides={key: value})
                    if key == "seed":
                        artifact_path = directory / "default.json"
                        data = sim_budget_io.read_json(artifact_path)
                        data["seed"] = value
                        artifact_path.write_text(json.dumps(data), encoding="utf-8")
                    self.assert_workload_acceptance(directory, reference, budgets, key,
                                                    value in ("1337", "01337", 1337))

    def test_invalid_seed_and_ticks_values_are_consistent_across_tools(self) -> None:
        reference = self.capture_dir("range-reference", presets=("default",))
        budgets = self.root / "range-budgets.json"
        emit_budgets(reference, budgets)
        for key in ("seed", "ticks"):
            values = (True, False, None, 1337.0, -1, 2**64, str(2**64))
            if key == "ticks":
                values += (0, "0")
            for index, value in enumerate(values):
                with self.subTest(key=key, value=value):
                    directory = self.capture_dir(f"range-{key}-{index}", presets=("default",),
                                                 overrides={key: value})
                    self.assert_workload_acceptance(directory, reference, budgets, key, False)

    def test_workload_integer_validator_accepts_uint64_boundaries(self) -> None:
        for key in ("seed", "ticks"):
            for value in (1, 1337, 2**64 - 1):
                for form in (value, str(value)):
                    with self.subTest(key=key, value=form):
                        self.assertEqual(sim_budget_io.validate_workload_integer(form, key), value)
        for value in (0, "0"):
            self.assertEqual(sim_budget_io.validate_workload_integer(value, "seed"), 0)
            with self.assertRaisesRegex(ValueError, "ticks"):
                sim_budget_io.validate_workload_integer(value, "ticks")

    def test_tool_helpers_use_shared_workload_validation(self) -> None:
        original = sim_budget_io.load_capture(self.capture_dir("helper-reference"))
        identity = propose_sim_budgets.build_proposal(original, 3, 2)["identity"]
        for key in ("seed", "ticks"):
            for value in ("1_337", "١٣٣٧", " 1337", "+1337", True, -1, 2**64):
                with self.subTest(key=key, value=value):
                    bad = {**original, "manifest": {**original["manifest"], key: value}}
                    with self.assertRaisesRegex(ValueError, key):
                        check_sim_budgets.identity_mismatch(identity, bad["manifest"])
                    with self.assertRaisesRegex(ValueError, key):
                        propose_sim_budgets.build_proposal(bad, 3, 2)
                    for before, after in ((bad, original), (original, bad)):
                        with self.assertRaisesRegex(ValueError, key):
                            sim_budget_compare.build_comparison(before, after, set())

    def test_budget_seed_and_ticks_out_of_range_are_rejected(self) -> None:
        directory = self.capture_dir("budget-range-reference")
        budgets = self.root / "budget-range.json"
        emit_budgets(directory, budgets)
        original = read_budgets(budgets)
        for key in ("seed", "ticks"):
            for value in (-1, 2**64) + ((0,) if key == "ticks" else ()):
                with self.subTest(key=key, value=value):
                    data = {**original, "identity": {**original["identity"], key: value}}
                    budgets.write_text(json.dumps(data), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, key):
                        check_sim_budgets.load_budgets(budgets)

    def test_emitted_budgets_carry_radius_identity(self) -> None:
        directory = self.capture_dir("radius-identity", overrides={"surface_radius": 2,
                                                                   "collision_radius": 3})
        budgets = self.root / "radius-identity-budgets.json"
        emit_budgets(directory, budgets)
        identity = read_budgets(budgets)["identity"]
        self.assertEqual(identity["surface_radius"], 2)
        self.assertEqual(identity["collision_radius"], 3)
        self.assertEqual(identity["seed"], 1337)
        self.assertIs(type(identity["seed"]), int)

    def test_null_or_omitted_seed_or_ticks_reports_unpinned_identity(self) -> None:
        directory = self.capture_dir("unpinned-identity")
        budgets = self.root / "unpinned-identity.json"
        emit_budgets(directory, budgets)
        original = read_budgets(budgets)
        for key in ("seed", "ticks"):
            for omitted in (False, True):
                with self.subTest(key=key, omitted=omitted):
                    data = json.loads(json.dumps(original))
                    if omitted:
                        del data["identity"][key]
                    else:
                        data["identity"][key] = None
                    budgets.write_text(json.dumps(data), encoding="utf-8")
                    for complete in (False, True):
                        argv = [str(directory), str(budgets)]
                        if complete:
                            argv.append("--require-complete")
                        code, out, err = run_tool(check_sim_budgets, argv)
                        self.assertEqual(code, 3 if complete else 0, err)
                        report = json.loads(out)
                        self.assertEqual(report["identity"][key]["verdict"], "unpinned")
                        self.assertEqual(report["summary"]["unpinned"], 1)

    def test_mistyped_budget_seed_or_ticks_is_rejected(self) -> None:
        directory = self.capture_dir("mistyped-identity")
        budgets = self.root / "mistyped-identity.json"
        emit_budgets(directory, budgets)
        original = read_budgets(budgets)
        for key in ("seed", "ticks"):
            for value in (True, False, "1337", 1337.0):
                with self.subTest(key=key, value=value):
                    data = json.loads(json.dumps(original))
                    data["identity"][key] = value
                    budgets.write_text(json.dumps(data), encoding="utf-8")
                    code, out, err = run_tool(check_sim_budgets, [str(directory), str(budgets)])
                    self.assertEqual((code, out), (2, ""))
                    self.assertIn(key, err)

    def test_malformed_fixture_hash_in_capture_is_rejected_by_every_tool(self) -> None:
        budgets = self.root / "malformed-fixture.json"
        emit_budgets(self.capture_dir("malformed-fixture-base"), budgets)
        for index, value in enumerate(("A" * 64, "a" * 63, "g" * 64)):
            with self.subTest(value=value):
                directory = self.capture_dir(f"malformed-fixture-{index}",
                                             overrides={"fixture_hash": value})
                with self.assertRaisesRegex(ValueError, "fixture_hash"):
                    sim_budget_io.load_capture(directory)
                for module, argv in (
                        (propose_sim_budgets, [str(directory)]),
                        (check_sim_budgets, [str(directory), str(budgets)]),
                        (sim_budget_compare, [str(directory), str(directory)])):
                    code, out, err = run_tool(module, argv)
                    self.assertEqual((code, out), (2, ""))
                    self.assertIn("fixture_hash", err)

    def test_malformed_fixture_hash_in_budget_is_rejected(self) -> None:
        directory = self.capture_dir("malformed-budget-fixture")
        budgets = self.root / "malformed-budget-fixture.json"
        emit_budgets(directory, budgets)
        data = read_budgets(budgets)
        for value in ("A" * 64, "a" * 63, "g" * 64, "", 5, True):
            with self.subTest(value=value):
                data["identity"]["fixture_hash"] = value
                budgets.write_text(json.dumps(data), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "fixture_hash"):
                    check_sim_budgets.load_budgets(budgets)
                code, out, err = run_tool(check_sim_budgets, [str(directory), str(budgets)])
                self.assertEqual((code, out), (2, ""))
                self.assertIn("fixture_hash", err)

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
        self.assertEqual(report["summary"]["unexpected_inactive"], 0)
        self.assertEqual(report["presets"]["default"]["stages"]["wind_weather"]["verdict"], "inactive")

    def test_inactive_stage_requires_explicit_expectation_only_with_require_complete(self) -> None:
        directory = self.capture_dir("inactive")
        budgets = self.root / "inactive.json"
        emit_budgets(directory, budgets)
        data = read_budgets(budgets)
        stage = data["presets"]["default"]["stages"]["wind_weather"]
        for expected in (None, False, True):
            with self.subTest(expected=expected):
                if expected is None:
                    del stage["expect_inactive"]
                else:
                    stage["expect_inactive"] = expected
                budgets.write_text(json.dumps(data), encoding="utf-8")
                for complete in (False, True):
                    argv = [str(directory), str(budgets)]
                    if complete:
                        argv.append("--require-complete")
                    code, out, err = run_tool(check_sim_budgets, argv)
                    self.assertEqual(code, 3 if complete and not expected else 0, err)
                    report = json.loads(out)
                    self.assertEqual(report["summary"]["unexpected_inactive"],
                                     0 if expected else 1)
                    row = report["presets"]["default"]["stages"]["wind_weather"]
                    self.assertEqual(row["verdict"], "inactive")
                    self.assertEqual(row["expect_inactive"], bool(expected))

    def test_mistyped_expect_inactive_is_rejected(self) -> None:
        directory = self.capture_dir("mistyped-inactive")
        budgets = self.root / "mistyped-inactive.json"
        emit_budgets(directory, budgets)
        data = read_budgets(budgets)
        for value in (None, "true", 0, 1, 1.0):
            with self.subTest(value=value):
                data["presets"]["default"]["stages"]["wind_weather"]["expect_inactive"] = value
                budgets.write_text(json.dumps(data), encoding="utf-8")
                code, out, err = run_tool(check_sim_budgets, [str(directory), str(budgets)])
                self.assertEqual((code, out), (2, ""))
                self.assertIn("expect_inactive", err)

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

    def test_pinned_limit_failure_takes_precedence_over_incomplete_identity(self) -> None:
        budgets = self.root / "incomplete-failure.json"
        emit_budgets(self.capture_dir("incomplete-failure-base"), budgets)
        data = read_budgets(budgets)
        data["identity"]["seed"] = None
        budgets.write_text(json.dumps(data), encoding="utf-8")
        directory = self.capture_dir("incomplete-failure-doubled", work_scale=2)
        for complete in (False, True):
            with self.subTest(complete=complete):
                argv = [str(directory), str(budgets)]
                if complete:
                    argv.append("--require-complete")
                code, out, err = run_tool(check_sim_budgets, argv)
                self.assertEqual(code, 1, err)
                report = json.loads(out)
                self.assertGreater(report["summary"]["fail"], 0)
                self.assertEqual(report["identity"]["seed"]["verdict"], "unpinned")

    def test_identity_mismatch_refuses_with_exit_two(self) -> None:
        directory = self.capture_dir("identity")
        budgets = self.root / "identity.json"
        emit_budgets(directory, budgets)
        data = read_budgets(budgets)
        for key in ("seed", "ticks"):
            with self.subTest(key=key):
                mismatched = json.loads(json.dumps(data))
                mismatched["identity"][key] = 600
                budgets.write_text(json.dumps(mismatched), encoding="utf-8")
                code, out, err = run_tool(check_sim_budgets, [str(directory), str(budgets)])
                self.assertEqual(code, 2)
                self.assertEqual(out, "")
                self.assertIn("different workload", err)
                self.assertIn(key, err)

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
        data = check_sim_budgets.load_budgets(capture_baseline())
        self.assertEqual(data["schema"], "luminumbra.sim_budgets.v1")
        self.assertEqual(data["identity"]["seed"], 1337)
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
