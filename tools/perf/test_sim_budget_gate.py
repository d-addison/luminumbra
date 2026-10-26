#!/usr/bin/env python3
"""Sim budget gate tests over synthetic capture directories; no engine or third-party modules."""

from __future__ import annotations

import contextlib
import copy
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock

# The registered contract also runs with Python isolated mode.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_sim_budget as capture
import check_sim_budgets
import propose_sim_budgets
import sim_budget_compare
import sim_budget_io
from test_fixture_hash import (
    ROOTED_REFERENCES, ESCAPING_REFERENCES, directory_link, remove_directory_link, symlink_or_skip,
)


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
        "effective_simulation": dict.fromkeys(
            capture.shipping_simulation_gates() + ("finite_hydrology", "weather_rain"), True),
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
    def test_work_item_extension_is_validated_by_every_capture_consumer(self):
        reference = self.capture_dir("work-reference")
        budgets = self.root / "work-budgets.json"
        emit_budgets(reference, budgets)
        valid = {"schema": "luminumbra.sim_work.v1", "counters": [
            {"name": name, "work_trace": [{"tick": tick, "work": tick}
                                         for tick in range(1, 5)]}
            for name in capture.WORK_ITEMS]}
        invalid = [None, {}, {"schema": "luminumbra.sim_work.v2", "counters": []}]
        for counters in (None, [], valid["counters"][:-1], valid["counters"][::-1],
                         valid["counters"] + valid["counters"][:1],
                         [None] + valid["counters"][1:]):
            invalid.append(dict(valid, counters=counters))
        for name in capture.WORK_ITEMS:
            index = capture.WORK_ITEMS.index(name)
            for trace in (None, [], [{"tick": 1, "work": 1}], [None] * 4,
                          [{}] * 4):
                changed = copy.deepcopy(valid)
                changed["counters"][index]["work_trace"] = trace
                invalid.append(changed)
            changed = copy.deepcopy(valid)
            changed["counters"][index]["name"] = "unknown"
            invalid.append(changed)
            for key, values in (("tick", (0, 2, True, "1", None, -1, 2**64)),
                                ("work", (True, "1", None, -1, 1.5, 2**64))):
                for value in values:
                    changed = copy.deepcopy(valid)
                    changed["counters"][index]["work_trace"][0][key] = value
                    invalid.append(changed)
        for index, extension in enumerate([valid] + invalid):
            with self.subTest(case=index):
                directory = self.capture_dir(f"work-extension-{index}")
                path = directory / "default.json"
                data = sim_budget_io.read_json(path)
                data["sim_budget"]["work_items"] = extension
                path.write_text(json.dumps(data), encoding="utf-8")
                self.assert_workload_acceptance(directory, reference, budgets,
                                                "work_items", index == 0)
        # Older v2 evidence remains readable; valid uint64 boundaries and extra
        # object metadata remain permitted by the additive artifact contract.
        self.assert_workload_acceptance(reference, reference, budgets, "", True)
        for work in (0, 2**64 - 1):
            valid["counters"][0]["work_trace"][0]["work"] = work
            valid["metadata"] = "diagnostic"
            capture.validate_work_items(valid, 4)

    def test_work_counter_names_match_all_native_producers(self):
        root = Path(__file__).resolve().parents[2]
        header = (root / "src/luminumbra_common/simulation/SimWorkCounters.h").read_text(encoding="utf-8")
        names = re.search(r'kSimWorkNames\{(.*?)\};', header, re.S).group(1)
        self.assertEqual(tuple(re.findall(r'"([a-z_]+)"', names)), capture.WORK_ITEMS)
        server = (root / "src/luminumbra_server/ServerStreamingBudget.h").read_text(encoding="utf-8")
        session = (root / "src/luminumbra_common/world/GameSession.cpp").read_text(encoding="utf-8")
        declared = re.search(r'enum class SimWork[^\{]*\{(.*?)Count', header, re.S).group(1)
        counters = re.findall(r'\b([A-Z][A-Za-z]+)\s*,', declared)
        self.assertEqual(len(counters), len(capture.WORK_ITEMS))
        for counter in counters:
            self.assertEqual((server + session).count(f"RecordWork(SimWork::{counter},"), 1)

    def test_deterministic_work_fixtures_and_existing_registration_stay_pinned(self):
        root = Path(__file__).resolve().parents[2]
        fixture = read_budgets(root / "test/fixtures/deterministic-work-budgets.json")
        self.assertEqual(fixture["schema"], "luminumbra.deterministic_work_budgets.v1")
        self.assertEqual(fixture["limits"], {
            "idle": dict(uploaded_bytes=0, hashed_bytes=0, uploads=0, surface_misses=0,
                         surface_samples=0, deferred_columns=0),
            "foliage": dict(surface_misses=4, surface_samples=32768, uploaded_bytes=2700000,
                            uploads=4, copied_bytes=5400000, hashed_bytes=0, deferred_columns=5,
                            backlog_growth=5, drain_steps=3),
            "simulation": dict(water_cells=64, streaming_generation=64, streaming_meshing=64,
                               physics_bodies=1, physics_characters=2, active_region_records=18,
                               active_region_due=18, snapshot_copies=128, activation_queries=5,
                               backlog=128, backlog_growth=64, drain_steps=64),
            "terrain_upload": dict(uploaded_bytes=4194304, uploads=8, backlog=17,
                                   backlog_growth=17, drain_steps=3),
            "water_upload": dict(uploaded_bytes=1048576, uploads=4, backlog=17,
                                 backlog_growth=17, drain_steps=5),
        })
        registration = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
        cmake = re.sub(r"#[^\n]*", "", registration)
        common = cmake.split("add_executable(common_tests", 1)[1].split(")", 1)[0]
        self.assertIn("common/DeterministicWorkBudget_test.cpp", common)
        native = (root / "test/common/DeterministicWorkBudget_test.cpp").read_text(encoding="utf-8")
        for name in ("EveryPinnedLimitRejectsAnInjectedExcess",
                     "IdleRejectsOneUploadByteAndOneHashByteIndependently",
                     "TerrainAndWaterUploadWorkAndProgressStayPinned",
                     "LiveSubsystemMovementEditAndDrainStayBounded"):
            self.assertIn(f"TEST(DeterministicWorkBudget, {name})", native)
        self.assertIn('COMMAND "${Python3_EXECUTABLE}" -I -B '
                      '"${CMAKE_SOURCE_DIR}/tools/perf/test_sim_budget_gate.py"', registration)
        ci_paths = (root / "tools/ci/ci_paths.py").read_text(encoding="utf-8")
        tests = ci_paths.split("PYTHON_TESTS = (", 1)[1].split(")", 1)[0]
        self.assertIn('"tools/perf/test_sim_budget_gate.py"', tests)
        foliage = (root / "test/rendering/foliage_rebuild_test.cpp").read_text(encoding="utf-8")
        for name in ("DeterministicWorkBudgetMovementEditDrainAndIdle",
                     "OutdatedRevisionReadbackIsDiscardedBeforeAdoption"):
            self.assertIn(f"TEST_F(FoliageRebuildTest, {name})", foliage)
        passes = cmake.split("add_executable(pass_context_test", 1)[1].split(")", 1)[0]
        self.assertIn("rendering/foliage_rebuild_test.cpp", passes)

    def test_preset_identity_is_preserved_and_enforced(self):
        before = self.capture_dir("preset-before", overrides={"preset_hash": FIXTURE_HASH})
        after = self.capture_dir("preset-after", overrides={"preset_hash": ZERO_HASH})
        budgets = self.root / "preset-budgets.json"
        emit_budgets(before, budgets)
        self.assertEqual(read_budgets(budgets)["identity"]["preset_hash"], FIXTURE_HASH)
        code, output, error = run_tool(propose_sim_budgets, [str(before)])
        self.assertEqual(code, 0, error)
        self.assertEqual(json.loads(output)["identity"]["preset_hash"], FIXTURE_HASH)
        for module, argv in ((check_sim_budgets, [str(after), str(budgets)]),
                             (sim_budget_compare, [str(before), str(after)])):
            code, output, error = run_tool(module, argv)
            self.assertEqual((code, output), (2, ""))
            self.assertIn("preset_hash", error)
        historical = self.capture_dir("historical-preset")
        code, _, error = run_tool(sim_budget_compare, [str(before), str(historical)])
        self.assertEqual(code, 2)
        self.assertIn("preset_hash", error)

    def test_invalid_preset_hash_is_refused(self):
        budgets = self.root / "preset-format-budgets.json"
        emit_budgets(self.capture_dir("preset-format-base"), budgets)
        data = read_budgets(budgets)
        for index, value in enumerate((None, 3, True, "a" * 63, "A" * 64, "")):
            directory = self.capture_dir(f"bad-preset-{index}", overrides={"preset_hash": value})
            with self.assertRaisesRegex(ValueError, "preset_hash"):
                sim_budget_io.load_capture(directory)
            data["identity"]["preset_hash"] = value
            budgets.write_text(json.dumps(data), encoding="utf-8")
            if value is None:
                self.assertIsNone(check_sim_budgets.load_budgets(budgets)["identity"]["preset_hash"])
            else:
                with self.assertRaisesRegex(ValueError, "preset_hash"):
                    check_sim_budgets.load_budgets(budgets)

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def capture_dir(self, name: str, **kwargs: Any) -> Path:
        return write_capture(self.root / name, **kwargs)

    def test_every_budget_tool_refuses_external_artifact_references(self):
        reference = self.capture_dir("reference")
        budgets = self.root / "budgets.json"
        emit_budgets(reference, budgets)
        directory = self.capture_dir("unsafe")
        manifest_path = directory / "capture.json"
        manifest = read_budgets(manifest_path)
        for artifact in ROOTED_REFERENCES + ESCAPING_REFERENCES:
            manifest["runs"][0]["artifact"] = artifact
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.subTest(artifact=artifact):
                with self.assertRaisesRegex(ValueError, "fixture reference"):
                    sim_budget_io.load_capture(directory)
                for module, argv in (
                        (propose_sim_budgets, [str(directory)]),
                        (check_sim_budgets, [str(directory), str(budgets)]),
                        (sim_budget_compare, [str(reference), str(directory)])):
                    code, out, err = run_tool(module, argv)
                    self.assertEqual((code, out), (2, ""))
                    self.assertIn("fixture reference", err)

    def test_artifact_links_cannot_leave_capture_directory(self):
        directory = self.capture_dir("linked")
        target = self.root / "outside.json"
        artifact = directory / "default.json"
        artifact.rename(target)
        symlink_or_skip(self, artifact, target)
        with self.assertRaisesRegex(ValueError, "resolves outside root"):
            sim_budget_io.load_capture(directory)

    def test_manifest_and_run_require_regular_files_before_read(self):
        directory = self.capture_dir("non-regular")
        original_stat = Path.stat
        original_read = Path.read_text
        for name in ("capture.json", "default.json"):
            for mode in (stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFDIR):
                def status(path, *args, **kwargs):
                    if path == directory / name:
                        return os.stat_result((mode,) + (0,) * 9)
                    return original_stat(path, *args, **kwargs)

                def read(path, *args, **kwargs):
                    if path == directory / name:
                        raise AssertionError("unchecked read")
                    return original_read(path, *args, **kwargs)

                with self.subTest(name=name, mode=mode), \
                        mock.patch.object(Path, "stat", status), \
                        mock.patch.object(Path, "read_text", read), \
                        self.assertRaisesRegex(ValueError, "not a regular file"):
                    sim_budget_io.load_capture(directory)

    def test_selected_capture_directory_alias_keeps_metadata_checks(self):
        actual = self.capture_dir("actual")
        directory = self.root / "alias"
        directory_link(directory, actual.resolve())
        original_stat = Path.stat
        try:
            sim_budget_io.load_capture(directory)
            for name in ("capture.json", "default.json"):
                for mode in (stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFDIR):
                    def status(path, *args, **kwargs):
                        if path == directory / name:
                            return os.stat_result((mode,) + (0,) * 9)
                        return original_stat(path, *args, **kwargs)

                    with self.subTest(name=name, mode=mode), \
                            mock.patch.object(Path, "stat", status), \
                            self.assertRaisesRegex(ValueError, "not a regular file"):
                        sim_budget_io.load_capture(directory)
        finally:
            remove_directory_link(directory)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "OS has no mkfifo")
    def test_fifo_manifest_and_run_are_refused_without_reading(self):
        directory = self.capture_dir("fifo")
        original_read = Path.read_text
        for name in ("capture.json", "default.json"):
            path = directory / name
            content = path.read_bytes()
            path.unlink()
            os.mkfifo(path)

            def read(candidate, *args, **kwargs):
                if candidate == path:
                    raise AssertionError("unchecked read")
                return original_read(candidate, *args, **kwargs)

            try:
                with self.subTest(name=name), mock.patch.object(Path, "read_text", read), \
                        self.assertRaisesRegex(ValueError, "not a regular file"):
                    sim_budget_io.load_capture(directory)
            finally:
                path.unlink()
                path.write_bytes(content)

    def test_normalized_nested_artifact_reference_stays_valid(self):
        directory = self.capture_dir("nested")
        (directory / "nested").mkdir()
        (directory / "default.json").rename(directory / "nested/default.json")
        (directory / "nested\\default.json").write_bytes((directory / "nested/default.json").read_bytes())
        manifest_path = directory / "capture.json"
        manifest = read_budgets(manifest_path)
        for artifact in ("nested/./unused/../default.json", "nested\\default.json"):
            manifest["runs"][0]["artifact"] = artifact
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            self.assertEqual(list(sim_budget_io.load_capture(directory)["presets"]), list(PRESETS))

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

    def test_partial_shipping_simulation_is_refused_by_every_consumer(self) -> None:
        reference = self.capture_dir("shipping-reference")
        budgets = self.root / "shipping-budgets.json"
        emit_budgets(reference, budgets)
        self.assert_workload_acceptance(reference, reference, budgets, "", True)
        for gate in ("plant_growth", "erosion", "ecology", "reproduction"):
            with self.subTest(gate=gate):
                directory = self.capture_dir(f"shipping-disabled-{gate}")
                path = directory / "default.json"
                data = sim_budget_io.read_json(path)
                data["effective_simulation"][gate] = False
                path.write_text(json.dumps(data), encoding="utf-8")
                self.assert_workload_acceptance(directory, reference, budgets,
                                                "shipping simulation", False)

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
        code, _, err = run_tool(check_sim_budgets, [str(other), str(budgets), "--require-complete"])
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
        emit_budgets(directory, budgets)
        unpinned = read_budgets(budgets)
        unpinned["identity"] = {"revision": None, "fixture_hash": None,
                                "seed": None, "ticks": None}
        for preset in unpinned["presets"].values():
            preset["total"]["max_work_per_tick"] = None
            for stage in preset["stages"].values():
                stage["max_work_per_tick"] = None
        budgets.write_text(json.dumps(unpinned), encoding="utf-8")
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

    def test_committed_baseline_is_complete_for_shipping_capture(self) -> None:
        data = check_sim_budgets.load_budgets(capture_baseline())
        self.assertEqual(data["schema"], "luminumbra.sim_budgets.v1")
        self.assertEqual(data["headroom"], {"num": 3, "den": 2})
        self.assertEqual(data["identity"]["seed"], 1337)
        self.assertEqual(data["identity"]["ticks"], 600)
        self.assertEqual(data["identity"]["surface_radius"], 1)
        self.assertEqual(data["identity"]["collision_radius"], 1)
        self.assertIsNone(data["identity"]["revision"])
        self.assertIsNone(data["identity"]["fixture_hash"])
        self.assertEqual(set(data["presets"]), set(PRESETS))
        idle = {"animation", "instinct", "perception", "locomotion", "energy",
                "irrigation", "disease", "fire", "circadian", "events", "server_streaming"}
        for preset in PRESETS:
            entry = data["presets"][preset]
            self.assertEqual(set(entry["stages"]), set(capture.STAGES))
            self.assertEqual(entry["total"]["max_work_per_tick"], 221289)
            self.assertEqual({name for name, stage in entry["stages"].items()
                              if stage["expect_inactive"]}, {"wind", "weather"})
            self.assertEqual({name for name, stage in entry["stages"].items()
                              if stage["max_work_per_tick"] == 0}, idle | {"wind", "weather"})
            for name, stage in entry["stages"].items():
                self.assertIs(type(stage["max_work_per_tick"]), int, f"{preset}/{name}")
                self.assertGreaterEqual(stage["max_work_per_tick"], 0)

    def test_committed_limits_pass_complete_and_reject_each_stage_regression(self) -> None:
        budgets = check_sim_budgets.load_budgets(capture_baseline())
        # Synthetic traces exercise the pinned combined path, including sampled idle stages.
        measured = {"server_physics": 1, "scent": 65536, "creatures": 11,
                    "wind_weather": 8192, "aether": 4096, "soil": 65536,
                    "plants": 6, "pollination": 6, "crops": 6, "grazing": 6,
                    "lifespan": 8, "alarm": 6, "decay": 6, "territory": 6,
                    "packs": 2, "migration": 6, "server_water": 4096}
        ticks = budgets["identity"]["ticks"]
        # Capture evidence keeps its own valid hash even though the baseline leaves it unpinned.
        directory = self.capture_dir("shipping", ticks=ticks)
        for preset in PRESETS:
            artifact = artifact_for(preset, ticks, 1)
            for stage in artifact["sim_budget"]["stages"]:
                name = stage["name"]
                inactive = name in {"wind", "weather"}
                work = measured.get(name, 0)
                stage.update(samples=0 if inactive else ticks,
                             work_total=work * ticks,
                             work_trace=[] if inactive else [
                                 {"tick": tick, "work": work} for tick in range(1, ticks + 1)],
                             duration_ms=None if inactive else dict(DURATION))
                self.assertEqual(budgets["presets"][preset]["stages"][name]["max_work_per_tick"],
                                 sim_budget_io.headroom(work, 3, 2))
            (directory / f"{preset}.json").write_text(json.dumps(artifact), encoding="utf-8")
        code, out, err = run_tool(check_sim_budgets, [str(directory), str(capture_baseline()),
                                                    "--require-complete"])
        self.assertEqual(code, 0, err)
        summary = json.loads(out)["summary"]
        self.assertEqual(summary, {"pass": 89, "inactive": 6, "fail": 0, "unpinned": 0,
                                   "unbudgeted": 0, "missing": 0, "unexpected_inactive": 0})
        loaded = sim_budget_io.load_capture(directory)
        for preset in PRESETS:
            for name in capture.STAGES:
                with self.subTest(preset=preset, stage=name):
                    regressed = copy.deepcopy(loaded)
                    stage = regressed["presets"][preset]["stages"][name]
                    limit = budgets["presets"][preset]["stages"][name]["max_work_per_tick"]
                    stage.update(samples=ticks, trace=[limit + 1] * ticks)
                    report, code = check_sim_budgets.build_report(regressed, budgets, True)
                    self.assertEqual(code, 1)
                    self.assertEqual(report["presets"][preset]["stages"][name]["verdict"], "fail")
            with self.subTest(preset=preset, stage="total"):
                regressed = copy.deepcopy(loaded)
                for name, stage in regressed["presets"][preset]["stages"].items():
                    if stage["samples"]:
                        limit = budgets["presets"][preset]["stages"][name]["max_work_per_tick"]
                        stage["trace"] = [limit] * ticks
                report, code = check_sim_budgets.build_report(regressed, budgets, True)
                self.assertEqual(code, 1)
                self.assertEqual(report["presets"][preset]["total"]["verdict"], "fail")
                self.assertTrue(all(stage["verdict"] in ("pass", "inactive")
                                    for stage in report["presets"][preset]["stages"].values()))

    def test_idle_proposals_require_calibration_and_zero_limits_enforce_no_work(self) -> None:
        directory = self.capture_dir("idle")
        for preset in PRESETS:
            path = directory / f"{preset}.json"
            artifact = json.loads(path.read_text(encoding="utf-8"))
            stage = artifact["sim_budget"]["stages"][capture.STAGES.index("animation")]
            stage["work_total"] = 0
            for sample in stage["work_trace"]:
                sample["work"] = 0
            path.write_text(json.dumps(artifact), encoding="utf-8")
        budgets = self.root / "idle-budgets.json"
        emit_budgets(directory, budgets)
        proposal = read_budgets(budgets.with_suffix(".proposal.json"))
        data = read_budgets(budgets)
        for preset in PRESETS:
            stage = proposal["presets"][preset]["stages"]["animation"]
            self.assertEqual(stage["status"], "idle")
            self.assertIsNone(stage["proposed_max_work_per_tick"])
            self.assertFalse(data["presets"][preset]["stages"]["animation"]["expect_inactive"])
        self.assertEqual(run_tool(check_sim_budgets, [str(directory), str(budgets),
                                                    "--require-complete"])[0], 3)
        for preset in PRESETS:
            data["presets"][preset]["stages"]["animation"]["max_work_per_tick"] = 0
        budgets.write_text(json.dumps(data), encoding="utf-8")
        self.assertEqual(run_tool(check_sim_budgets, [str(directory), str(budgets),
                                                    "--require-complete"])[0], 0)

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
