#!/usr/bin/env python3
"""Capture command and evidence refusal tests; no engine or third-party modules needed."""

from __future__ import annotations

import copy
import contextlib
import io
import json
import sys
from pathlib import Path
import shlex
import tempfile
import unittest
from unittest import mock

import capture_sim_budget as capture
from fixture_hash import fixture_hash


def artifact() -> dict:
    return {
        "schema": "luminumbra.server_tick.v1", "preset": "default", "seed": "1337",
        "ticks_requested": 2, "passed": True,
        "world_hash": "0123456789abcdef", "world_hash_replay": "0123456789abcdef",
        "effective_simulation": dict.fromkeys(
            capture.shipping_simulation_gates() + ("finite_hydrology", "weather_rain"), True),
        "sim_budget": {
            "schema": "luminumbra.sim_budget.v2", "work_replay_match": True,
            "stages": [{"name": name, "samples": 2, "work_total": 3,
                        "work_trace": [{"tick": 1, "work": 1}, {"tick": 2, "work": 2}],
                        "duration_ms": {"p50": 0.5, "p95": 0.95, "p99": 0.99, "maximum": 1.0}}
                        if name != "wind_weather" else
                        {"name": name, "samples": 0, "work_total": 0,
                         "work_trace": [], "duration_ms": None}
                       for name in capture.STAGES],
        },
    }


def regions(samples: int = 2) -> dict:
    block = {
        "schema": capture.REGION_SCHEMA, "samples": samples,
        "first_tick": 1 if samples else None, "last_tick": samples if samples else None,
        "complete": True, "dropped_samples": 0,
        "duration_ms": {"p50": 0.5, "p95": 0.95, "p99": 0.99, "maximum": 1.0}
                       if samples else None,
        **{group: {name: 0 for name in names} for group, names in capture.REGION_COUNTS.items()},
        "replay": {"trace_digest": "0123456789abcdef", "work_replay_match": True},
        "fault": None,
    }
    block["scheduler"].update({name: {"current": 0, "maximum": 0}
                               for name in ("ledger_records", "active", "reduced", "frozen")})
    block["persistence"].update({"parked_bytes": {"current": 0, "maximum": 0},
                                  "save_duration_ms": None})
    block["water"].update({"bound_known": False, "bound": None})
    return block


class SimBudgetCaptureTests(unittest.TestCase):
    def validate(self, data: dict) -> None:
        capture.validate_artifact(data, "default", "1337", 2)

    def test_artifact_reports_every_shipping_gate_and_all_on_passes(self):
        systems = json.loads((Path(__file__).resolve().parents[2] /
                              "data/common/systems.json").read_text(encoding="utf-8-sig"))
        expected = {name for name, value in systems["sim"].items()
                    if isinstance(value, dict) and "enabled" in value}
        self.assertEqual(set(capture.shipping_simulation_gates()), expected)
        self.assertEqual(set(artifact()["effective_simulation"]),
                         expected | {"finite_hydrology", "weather_rain"})
        self.validate(artifact())

    def test_gate_outside_original_subset_is_required(self):
        for value in (False, None, 1, "true"):
            data = artifact()
            data["effective_simulation"]["plant_growth"] = value
            with self.assertRaisesRegex(ValueError, "shipping simulation"):
                self.validate(data)
        data = artifact()
        del data["effective_simulation"]["reproduction"]
        with self.assertRaisesRegex(ValueError, "shipping simulation"):
            self.validate(data)

    def test_new_shipping_gate_is_automatically_required(self):
        path = Path(__file__).resolve().parents[2] / "data/common/systems.json"
        systems = json.loads(path.read_text(encoding="utf-8-sig"))
        data = artifact()
        systems["sim"]["future_system"] = {"enabled": True}
        with mock.patch.object(Path, "read_text", return_value=json.dumps(systems)):
            self.assertIn("future_system", capture.shipping_simulation_gates())
            with self.assertRaisesRegex(ValueError, "shipping simulation"):
                self.validate(data)
            data["effective_simulation"]["future_system"] = False
            with self.assertRaisesRegex(ValueError, "shipping simulation"):
                self.validate(data)
            data["effective_simulation"]["future_system"] = True
            self.validate(data)

    def test_shipping_activation_is_required(self):
        for field in artifact()["effective_simulation"]:
            data = artifact()
            data["effective_simulation"][field] = False
            with self.assertRaisesRegex(ValueError, "shipping simulation"):
                self.validate(data)
        data = artifact()
        del data["effective_simulation"]
        with self.assertRaisesRegex(ValueError, "shipping simulation"):
            self.validate(data)

    def test_unknown_object_keys_are_ignored(self) -> None:
        data = artifact()
        data["extension"] = 1
        data["sim_budget"]["extension"] = 1
        data["sim_budget"]["stages"][0]["extension"] = 1
        self.validate(data)
        # The combined path has its own complete trace; neither separate stage runs.
        stages = {stage["name"]: stage for stage in data["sim_budget"]["stages"]}
        for key in ("samples", "work_total", "work_trace", "duration_ms"):
            stages["wind_weather"][key] = copy.deepcopy(stages["wind"][key])
            for name in ("wind", "weather"):
                stages[name][key] = [] if key == "work_trace" else None if key == "duration_ms" else 0
        self.validate(data)
        stages["wind"]["samples"] = 2
        with self.assertRaises(ValueError):
            self.validate(data)

    def test_absent_corrupt_future_and_mismatched_replay_are_refused(self) -> None:
        for value in (None, {}, {"schema": "luminumbra.sim_budget.v1"},
                      {"schema": "luminumbra.sim_budget.v3"},
                      {**artifact()["sim_budget"], "work_replay_match": False}):
            with self.subTest(value=value):
                data = artifact()
                data["sim_budget"] = value
                with self.assertRaises(ValueError):
                    self.validate(data)
        data = artifact()
        data["world_hash_replay"] = "fedcba9876543210"
        with self.assertRaises(ValueError):
            self.validate(data)

    def test_invalid_world_hash_types_and_formats_are_refused(self) -> None:
        for value in (True, False, 1, 1234567890123456, ["0123456789abcdef"], [],
                      {}, None, "", "abc", "0123456789abcde", "0123456789abcdef0",
                      "0123456789abcdeg", "0123456789ABCDEF", "0123456789abcdef\n",
                      " 0123456789abcdef", "0x0123456789abcdef"):
            for fields in (("world_hash",), ("world_hash_replay",),
                           ("world_hash", "world_hash_replay")):
                with self.subTest(value=value, fields=fields):
                    data = artifact()
                    for field in fields:
                        data[field] = value
                    with self.assertRaises(ValueError):
                        self.validate(data)

    def test_missing_reordered_unknown_and_duplicate_stages_are_refused(self) -> None:
        original = artifact()["sim_budget"]["stages"]
        for stages in (original[:-1], list(reversed(original)),
                       original + [original[0]], [{**original[0], "name": "future"}] + original[1:]):
            with self.subTest(stages=stages):
                data = artifact()
                data["sim_budget"]["stages"] = stages
                with self.assertRaises(ValueError):
                    self.validate(data)

    def test_invalid_counts_coverage_and_timers_are_refused(self) -> None:
        bad_stages = []
        for field, values in (("samples", (0, 1, True, 2.0)),
                              ("work_total", (-1, True, 3.0, 4, 2**64))):
            for value in values:
                bad_stages.append({**artifact()["sim_budget"]["stages"][0], field: value})
        for value in (-1, True, 1.0, 2**64):
            stage = copy.deepcopy(artifact()["sim_budget"]["stages"][0])
            stage["work_trace"][0]["work"] = value
            bad_stages.append(stage)
        stage = copy.deepcopy(artifact()["sim_budget"]["stages"][0])
        stage["work_trace"][1]["tick"] = 1
        bad_stages.append(stage)
        for value in (None, -1, float("nan"), float("inf"), True, 2.0, 10**500):
            stage = copy.deepcopy(artifact()["sim_budget"]["stages"][0])
            stage["duration_ms"]["p50"] = value
            bad_stages.append(stage)
        for stage in bad_stages:
            with self.subTest(stage=stage):
                data = artifact()
                data["sim_budget"]["stages"][0] = stage
                with self.assertRaises(ValueError):
                    self.validate(data)

    def test_empty_population_is_refused(self) -> None:
        data = artifact()
        plants = data["sim_budget"]["stages"][capture.STAGES.index("plants")]
        plants["work_total"] = 0
        for sample in plants["work_trace"]:
            sample["work"] = 0
        with self.assertRaises(ValueError):
            self.validate(data)

    def test_capture_command_pins_presets_population_audio_and_switch(self) -> None:
        self.assertEqual(capture.PRESETS, ("default", "mountains", "archipelago"))
        for preset in capture.PRESETS:
            argv = capture.command(Path("server"), Path("root"), Path("evidence.json"),
                                   preset, "1337", 3600, 1, 1)
            for flag in ("--smoke", "--no-audio", "--sim-budget", "--ecology-roster", "--planted-roster"):
                self.assertIn(flag, argv)
            self.assertEqual(argv[argv.index("--preset") + 1], preset)
            self.assertEqual(argv[argv.index("--ticks") + 1], "3600")
            self.assertEqual(argv[argv.index("--autosave-ticks") + 1], "30")
            self.assertEqual(argv[argv.index("--seed") + 1], "1337")

    def test_capture_uses_shipping_checkout_root_and_headless_fixture_hash(self) -> None:
        root = Path(capture.__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "server"
            binary.write_bytes(b"synthetic server")
            evidence = Path(temporary) / "evidence"
            commands = []

            def run(argv, **kwargs):
                self.assertEqual(kwargs["cwd"], root)
                self.assertTrue(kwargs["check"])
                self.assertEqual(argv[argv.index("--root") + 1], str(root))
                data = artifact()
                data["preset"] = argv[argv.index("--preset") + 1]
                Path(argv[argv.index("--artifact") + 1]).write_text(json.dumps(data), encoding="utf-8")
                commands.append(argv)

            with mock.patch("sys.argv", ["capture_sim_budget.py", "--binary", str(binary),
                                         "--evidence-dir", str(evidence), "--ticks", "2"]), \
                    mock.patch.object(capture.subprocess, "check_output", side_effect=["a" * 40, ""]), \
                    mock.patch.object(capture.subprocess, "run", side_effect=run), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(capture.main(), 0)
            manifest = json.loads((evidence / "capture.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["fixture_hash"], capture.capture_fixture_hash(root))
            self.assertNotEqual(manifest["fixture_hash"], fixture_hash(root / "data/common",
                                                                      scope="headless-server"))
            self.assertEqual(manifest["seed"], "1337")
            self.assertEqual(manifest["surface_radius"], 1)
            self.assertEqual(manifest["collision_radius"], 1)
            self.assertEqual([run["command"] for run in manifest["runs"]], commands)
            self.assertEqual([run["preset"] for run in manifest["runs"]], list(capture.PRESETS))

    def test_ci_shipping_capture_pins_workload_and_requires_complete_budgets(self) -> None:
        root = Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/performance.yml").read_text(encoding="utf-8")
        step = workflow.split("- name: Capture simulation budget evidence\n", 1)[1].split(
            "      - name:", 1)[0]
        capture_command = shlex.split(step.split("run: |", 1)[1].split(
            "python3 head/tools/perf/propose_sim_budgets.py", 1)[0].replace("\\\n", ""))
        self.assertEqual(capture_command[:2], ["python3", "head/tools/perf/capture_sim_budget.py"])
        for flag, value in (("--ticks", "600"), ("--seed", "1337"), ("--radius", "1"),
                            ("--collision-radius", "1")):
            self.assertEqual(capture_command[capture_command.index(flag) + 1], value)
        check_command = shlex.split(step.split(
            "python3 head/tools/perf/check_sim_budgets.py", 1)[1].split("||", 1)[0].replace("\\\n", ""))
        self.assertIn("head/tools/perf/baselines/sim-budgets.json", check_command)
        self.assertIn("--require-complete", check_command)


class CaptureInputIdentityTests(unittest.TestCase):
    @staticmethod
    def write_fixture(root: Path, *, height: int = 8, table: str = "extra/biomes.json") -> None:
        common = root / "data/common"
        common.mkdir(parents=True)
        (common / "systems.json").write_text('{"sim":{}}', encoding="utf-8")
        (common / "creatures/species").mkdir(parents=True)
        (common / "creatures/species/one.json").write_text('{"id":"one"}', encoding="utf-8")
        (common / "structures/hut").mkdir(parents=True)
        (common / "structures/hut/piece.json").write_text('{"boxes":[]}', encoding="utf-8")
        presets = root / "worlds/atlas/presets"
        presets.mkdir(parents=True)
        for preset in capture.PRESETS:
            (presets / f"{preset}.json").write_text(json.dumps({"generation_params": {
                "terrain": {"height_offset": height}, "biomes": {"table": table}}}),
                encoding="utf-8")
        (root / "data/extra").mkdir(parents=True)
        (root / "data/extra/biomes.json").write_text('{"biomes":[]}', encoding="utf-8")

    def test_existing_shipping_identity_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_fixture(root)
            self.assertEqual(capture.capture_fixture_hash(root),
                             "c3f1ba1a6dd9df17f88a5ac98f589d6fd542ad4f581894dd6274056c696f6196")

    def test_resolved_presets_and_all_referenced_inputs_affect_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = root / "first", root / "second"
            self.write_fixture(first)
            self.write_fixture(second)
            baseline = capture.capture_fixture_hash(first)
            self.assertEqual(baseline, capture.capture_fixture_hash(second))
            import sim_budget_compare
            for relative, replacement in (
                    ("worlds/atlas/presets/default.json", '{"generation_params":{"terrain":{"height_offset":9}}}'),
                    ("worlds/atlas/presets/mountains.json", '{"generation_params":{"terrain":{"height_offset":9}}}'),
                    ("worlds/atlas/presets/archipelago.json", '{"generation_params":{"terrain":{"height_offset":9}}}'),
                    ("data/extra/biomes.json", '{"biomes":[{"id":"different"}]}'),
                    ("data/common/structures/hut/piece.json", '{"boxes":[{"material":1}]}'),
                    ("data/common/creatures/species/one.json", '{"id":"one","lifespan":99}'),
                    ("data/common/systems.json", '{"sim":{"active_regions":{"enabled":true}}}')):
                with self.subTest(relative=relative):
                    path = second / relative
                    original = path.read_bytes()
                    path.write_text(replacement, encoding="utf-8")
                    changed = capture.capture_fixture_hash(second)
                    self.assertNotEqual(baseline, changed)
                    workload = {"seed": "1337", "ticks": 2, "surface_radius": 1,
                                "collision_radius": 1, "fixture_hash": baseline}
                    with self.assertRaisesRegex(ValueError, "fixture_hash"):
                        sim_budget_compare.build_comparison(
                            {"manifest": workload}, {"manifest": {**workload, "fixture_hash": changed}},
                            set())
                    path.write_bytes(original)
                    self.assertEqual(baseline, capture.capture_fixture_hash(second))
            table = second / "data/extra/biomes.json"
            table.unlink()
            self.assertNotEqual(baseline, capture.capture_fixture_hash(second))
            table.write_text('{"biomes":[]}', encoding="utf-8")
            # The headless scope still ignores render-only descriptors and JSON comments.
            (second / "data/common/particles").mkdir()
            (second / "data/common/particles/precip_rain.json").write_text('{}', encoding="utf-8")
            table.write_text('{"_comment":"different formatting", "biomes": []}', encoding="utf-8")
            self.assertEqual(baseline, capture.capture_fixture_hash(second))

    def test_absolute_and_parent_relative_references_hash_resolved_content(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            external = root / "table.json"
            external.write_text('{"biomes":[]}', encoding="utf-8")
            for name, reference in (("absolute", str(external)), ("relative", "../../table.json")):
                with self.subTest(name=name):
                    fixture = root / name
                    self.write_fixture(fixture, table=reference)
                    baseline = capture.capture_fixture_hash(fixture)
                    external.write_text('{"biomes":[1]}', encoding="utf-8")
                    self.assertNotEqual(baseline, capture.capture_fixture_hash(fixture))
                    external.write_text('{"biomes":[]}', encoding="utf-8")

    def test_linked_common_directories_are_hashed_and_cycles_refuse(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_fixture(root)
            linked = root / "linked"
            linked.mkdir()
            (linked / "input.json").write_text('{"rate":1}', encoding="utf-8")
            common = root / "data/common"
            original_iterdir = Path.iterdir
            original_resolve = Path.resolve

            def children(path):
                yield from original_iterdir(path)
                if path == common:
                    yield linked

            # Emulate directory-link traversal without platform-specific link privileges.
            with mock.patch.object(Path, "iterdir", children):
                baseline = capture.capture_fixture_hash(root)
                (linked / "input.json").write_text('{"rate":2}', encoding="utf-8")
                self.assertNotEqual(baseline, capture.capture_fixture_hash(root))
                with mock.patch.object(Path, "resolve", lambda path: original_resolve(common)
                                       if path == linked else original_resolve(path)):
                    with self.assertRaisesRegex(ValueError, "cyclic"):
                        capture.capture_fixture_hash(root)

    def test_biome_paths_normalize_before_probing_and_hashing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, reference in (
                    ("missing", "missing/../extra/biomes.json"),
                    ("file", "extra/biomes.json/../biomes.json"),
                    ("absolute", str(root / "absolute/data/missing/../extra/biomes.json"))):
                with self.subTest(reference=reference):
                    fixture = root / name
                    self.write_fixture(fixture, table=reference)
                    self.assertFalse((fixture / "data/missing").exists())
                    baseline = capture.capture_fixture_hash(fixture)
                    table = fixture / "data/extra/biomes.json"
                    table.write_text('{"biomes":[1]}', encoding="utf-8")
                    self.assertNotEqual(baseline, capture.capture_fixture_hash(fixture))
                    table.unlink()
                    self.assertNotEqual(baseline, capture.capture_fixture_hash(fixture))
                    table.write_text('{"biomes":[]}', encoding="utf-8")
                    self.assertEqual(baseline, capture.capture_fixture_hash(fixture))

    def test_non_object_biomes_are_ignored_as_optional_blocks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_fixture(root)
            table = root / "data/extra/biomes.json"
            for block in (None, False, True, 0, 1.5, "", "ignored", [], ["ignored"]):
                with self.subTest(block=block):
                    for preset in capture.PRESETS:
                        path = root / "worlds/atlas/presets" / f"{preset}.json"
                        document = json.loads(path.read_text(encoding="utf-8"))
                        document["generation_params"]["biomes"] = block
                        path.write_text(json.dumps(document), encoding="utf-8")
                    baseline = capture.capture_fixture_hash(root)
                    self.assertRegex(baseline, r"^[0-9a-f]{64}$")
                    table.write_text('{"biomes":[1]}', encoding="utf-8")
                    self.assertEqual(baseline, capture.capture_fixture_hash(root))
                    table.write_text('{"biomes":[]}', encoding="utf-8")

    def test_main_accepts_normalized_paths_and_optional_biomes_in_both_modes(self) -> None:
        for required in (False, True):
            for block in (None, False, True, 0, 1.5, "", "ignored", [], ["ignored"], {},
                          {"table": "missing/../extra/biomes.json"}):
                with self.subTest(required=required, block=block), \
                        tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    self.write_fixture(root)
                    systems = root / "data/common/systems.json"
                    systems.write_text(json.dumps({"sim": {"active_regions": {"enabled": required}}}),
                                       encoding="utf-8")
                    for preset in capture.PRESETS:
                        path = root / "worlds/atlas/presets" / f"{preset}.json"
                        document = json.loads(path.read_text(encoding="utf-8"))
                        document["generation_params"]["biomes"] = block
                        path.write_text(json.dumps(document), encoding="utf-8")
                    binary = root / "server"
                    binary.write_bytes(b"fixture")
                    evidence = root / "evidence"
                    argv = ["capture_sim_budget.py", "--binary", str(binary), "--root", str(root),
                            "--ticks", "2", "--evidence-dir", str(evidence)]
                    if required:
                        argv.append("--regions")

                    def run(command, **kwargs):
                        data = artifact()
                        data["preset"] = command[command.index("--preset") + 1]
                        if required:
                            data["sim_budget"]["regions"] = regions()
                        Path(command[command.index("--artifact") + 1]).write_text(
                            json.dumps(data), encoding="utf-8")

                    with mock.patch.object(sys, "argv", argv), \
                            mock.patch.object(capture.subprocess, "run", side_effect=run) as launched, \
                            mock.patch.object(capture.subprocess, "check_output", side_effect=["a" * 40, ""]), \
                            contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(capture.main(), 0)
                    self.assertEqual(launched.call_count, 3)
                    manifest = json.loads((evidence / "capture.json").read_text(encoding="utf-8"))
                    self.assertEqual(manifest["fixture_hash"], capture.capture_fixture_hash(root))
                    self.assertEqual([run["preset"] for run in manifest["runs"]], list(capture.PRESETS))
                    if required:
                        self.assertIs(manifest["regions_required"], True)
                    else:
                        self.assertNotIn("regions_required", manifest)

    def test_changed_fixture_during_capture_refuses_success_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_fixture(root, table="missing/../extra/biomes.json")
            binary = root / "server"
            binary.write_bytes(b"fixture")
            evidence = root / "evidence"
            argv = ["capture_sim_budget.py", "--binary", str(binary), "--root", str(root),
                    "--ticks", "2", "--evidence-dir", str(evidence)]

            def run(command, **kwargs):
                data = artifact()
                data["preset"] = command[command.index("--preset") + 1]
                Path(command[command.index("--artifact") + 1]).write_text(
                    json.dumps(data), encoding="utf-8")
                (root / "data/extra/biomes.json").write_text('{"biomes":[1]}', encoding="utf-8")

            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(capture.subprocess, "run", side_effect=run), \
                    mock.patch.object(capture.subprocess, "check_output", side_effect=["a" * 40, ""]), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    capture.main()
                self.assertFalse((evidence / "capture.json").exists())

    def test_invalid_fixture_refuses_before_capture_or_success_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_fixture(root)
            binary = root / "server"
            binary.write_bytes(b"fixture")
            path = root / "worlds/atlas/presets/default.json"
            for invalid in (None, b"{", b'{"generation_params":{"biomes":{"table":0}}}'):
                with self.subTest(invalid=invalid):
                    if invalid is None:
                        path.unlink()
                    else:
                        path.write_bytes(invalid)
                    evidence = root / "evidence"
                    argv = ["capture_sim_budget.py", "--binary", str(binary), "--root", str(root),
                            "--evidence-dir", str(evidence)]
                    with mock.patch.object(sys, "argv", argv), \
                            mock.patch.object(capture.subprocess, "run") as launched, \
                            contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            capture.main()
                        launched.assert_not_called()
                        self.assertFalse(evidence.exists())


class RegionCaptureTests(unittest.TestCase):
    def test_optional_block_and_requested_coverage(self) -> None:
        data = artifact()
        capture.validate_artifact(data, "default", "1337", 2)
        with self.assertRaises(ValueError):
            capture.validate_artifact(data, "default", "1337", 2, require_regions=True)
        data["sim_budget"]["regions"] = regions()
        capture.validate_artifact(data, "default", "1337", 2, require_regions=True)
        for gate in artifact()["effective_simulation"]:
            for value in (False, None, 1, "true"):
                invalid = copy.deepcopy(data)
                invalid["effective_simulation"][gate] = value
                with self.subTest(gate=gate, value=value), \
                        self.assertRaisesRegex(ValueError, "shipping simulation"):
                    capture.validate_artifact(invalid, "default", "1337", 2, require_regions=True)
            invalid = copy.deepcopy(data)
            del invalid["effective_simulation"][gate]
            with self.subTest(gate=gate), \
                    self.assertRaisesRegex(ValueError, "shipping simulation"):
                capture.validate_artifact(invalid, "default", "1337", 2, require_regions=True)
        for samples in (0, 1, 3):
            data["sim_budget"]["regions"] = regions(samples)
            with self.assertRaises(ValueError):
                capture.validate_artifact(data, "default", "1337", 2, require_regions=True)

    def test_empty_single_and_cap_captures(self) -> None:
        for samples in (0, 1, capture.MAX_REGION_SAMPLES):
            capture.validate_regions(regions(samples))
        for samples in (capture.MAX_REGION_SAMPLES + 1, True, -1, 2.0, 2**64):
            block = regions()
            block["samples"] = samples
            with self.assertRaises(ValueError):
                capture.validate_regions(block)

    def test_every_required_field_and_group_is_checked(self) -> None:
        original = regions()
        paths = [(name,) for name in original]
        for group in capture.REGION_COUNTS:
            paths += [(group, name) for name in original[group]]
        paths += [("replay", name) for name in original["replay"]]
        for group in ("scheduler", "persistence"):
            for name, value in original[group].items():
                if isinstance(value, dict):
                    paths += [(group, name, child) for child in value]
        for path in paths:
            with self.subTest(path=path):
                block = copy.deepcopy(original)
                target = block
                for name in path[:-1]:
                    target = target[name]
                del target[path[-1]]
                with self.assertRaises(ValueError):
                    capture.validate_regions(block)

    def test_all_counter_types_and_ranges_are_checked(self) -> None:
        for group, names in capture.REGION_COUNTS.items():
            for name in names:
                for value in (True, -1, 0.0, 2**64, None, []):
                    with self.subTest(group=group, name=name, value=value):
                        block = regions()
                        block[group][name] = value
                        with self.assertRaises(ValueError):
                            capture.validate_regions(block)

    def test_timing_replay_gauges_water_saves_fault_and_overflow_refuse(self) -> None:
        mutations = [
            (("schema",), "future"), (("complete",), False), (("complete",), 1),
            (("dropped_samples",), 1), (("dropped_samples",), True),
            (("first_tick",), 0), (("last_tick",), 3),
            (("duration_ms", "p50"), -1), (("duration_ms", "p50"), True),
            (("duration_ms", "p50"), float("nan")),
            (("duration_ms", "p50"), 10**500),
            (("duration_ms", "p99"), float("inf")), (("duration_ms", "maximum"), 0),
            (("scheduler", "active", "current"), 1),
            (("scheduler", "active", "maximum"), 1),
            (("persistence", "parked_bytes", "maximum"), -1),
            (("persistence", "saves_total"), 1),
            (("persistence", "failures_total"), 1),
            (("persistence", "save_samples"), capture.MAX_REGION_SAVE_SAMPLES + 1),
            (("water", "reserved"), 1), (("water", "bound_known"), True),
            (("water", "bound_known"), 0), (("water", "bound"), 1),
            (("replay", "trace_digest"), "0123456789ABCDEF"),
            (("replay", "work_replay_match"), False),
            (("fault",), {"tick": 2, "message": "fault"}),
            (("fault",), {"tick": True, "message": "x"}),
            (("fault",), {"tick": 2, "message": "x" * 257}),
            (("fault",), {"tick": 2, "message": "\n"}),
            (("fault",), {"tick": 2, "message": "é"}),
        ]
        for path, value in mutations:
            with self.subTest(path=path, value=value):
                block = regions()
                target = block
                for name in path[:-1]:
                    target = target[name]
                target[path[-1]] = value
                with self.assertRaises(ValueError):
                    capture.validate_regions(block)
        empty = regions(0)
        empty["duration_ms"] = regions()["duration_ms"]
        with self.assertRaises(ValueError):
            capture.validate_regions(empty)

    def test_byte_cap_includes_extensions_and_saves_have_independent_cap(self) -> None:
        block = regions()
        block["future"] = "compatible"
        capture.validate_regions(block)
        block["future"] = "x" * capture.MAX_REGION_BLOCK_BYTES
        with self.assertRaises(ValueError):
            capture.validate_regions(block)
        block = regions()
        saves = block["persistence"]
        saves["saves_total"] = saves["save_samples"] = capture.MAX_REGION_SAVE_SAMPLES
        saves["save_duration_ms"] = block["duration_ms"]
        capture.validate_regions(block)
        saves["saves_total"] += 1
        saves["save_samples"] += 1
        with self.assertRaises(ValueError):
            capture.validate_regions(block)

    def test_duplicate_json_keys_refuse(self) -> None:
        import json
        with self.assertRaises(ValueError):
            json.loads('{"regions":{"complete":true,"complete":false}}',
                       object_pairs_hook=capture.unique_object)


    def test_allowed_known_bound_and_unknown_extensions(self) -> None:
        block = regions()
        block["water"]["bound_known"] = True
        block["water"]["bound"] = 0
        capture.validate_regions(block)
        block["water"]["bound"] = 30
        block["future"] = {"description": "compatible"}
        capture.validate_regions(block)
        block["future"] = "é" * 4000
        self.assertLess(len(json.dumps(block, ensure_ascii=False, indent=8).encode("utf-8")),
                        capture.MAX_REGION_BLOCK_BYTES)
        capture.validate_regions(block)

    def test_shared_reader_honors_regional_manifest_and_duplicates(self) -> None:
        import sim_budget_io
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = {
                "schema": "luminumbra.sim_budget_capture.v1", "revision": "a" * 40,
                "source_dirty": False, "binary_sha256": "0" * 64, "fixture_hash": "1" * 64,
                "seed": "1337", "ticks": 2, "surface_radius": 1, "collision_radius": 1,
                "regions_required": True,
                "runs": [{"preset": "default", "artifact": "default.json"}],
            }
            (root / "capture.json").write_text(json.dumps(manifest), encoding="utf-8")
            (root / "default.json").write_text(json.dumps(artifact()), encoding="utf-8")
            with self.assertRaises(ValueError):
                sim_budget_io.load_capture(root)
            data = artifact()
            data["sim_budget"]["regions"] = regions()
            (root / "default.json").write_text(json.dumps(data), encoding="utf-8")
            sim_budget_io.load_capture(root)
            for invalid in (None, 0, 1, "true"):
                manifest["regions_required"] = invalid
                (root / "capture.json").write_text(json.dumps(manifest), encoding="utf-8")
                with self.assertRaises(ValueError):
                    sim_budget_io.load_capture(root)
            (root / "default.json").write_text('{"passed":true,"passed":false}', encoding="utf-8")
            with self.assertRaises(ValueError):
                sim_budget_io.read_json(root / "default.json")

    def test_main_requires_both_flags_and_complete_regional_artifacts(self) -> None:
        for enabled, supplied, required in ((False, False, True), (0, False, True),
                                            ("true", False, True), (True, False, True),
                                            (True, True, True), (False, False, False)):
            with self.subTest(enabled=enabled, supplied=supplied), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                systems = root / "data/common/systems.json"
                systems.parent.mkdir(parents=True)
                systems.write_text(json.dumps({"sim": {"active_regions": {"enabled": enabled}}}),
                                   encoding="utf-8")
                binary = root / "server"
                binary.write_bytes(b"fixture")
                evidence = root / "evidence"
                argv = ["capture_sim_budget.py", "--binary", str(binary), "--root", str(root),
                        "--ticks", "2", "--evidence-dir", str(evidence)]
                if required:
                    argv.append("--regions")
                def run(command, **kwargs):
                    self.assertIn("--sim-budget", command)
                    self.assertEqual(command[command.index("--root") + 1], str(root))
                    data = artifact()
                    data["preset"] = command[command.index("--preset") + 1]
                    if supplied:
                        data["sim_budget"]["regions"] = regions()
                    Path(command[command.index("--artifact") + 1]).write_text(
                        json.dumps(data), encoding="utf-8")
                with mock.patch.object(sys, "argv", argv), \
                        mock.patch.object(capture.subprocess, "run", side_effect=run) as launched, \
                        mock.patch.object(capture.subprocess, "check_output", side_effect=["a" * 40, ""]), \
                        mock.patch.object(capture, "capture_fixture_hash", return_value="1" * 64), \
                        contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(io.StringIO()):
                    if not required or (enabled is True and supplied):
                        self.assertEqual(capture.main(), 0)
                        self.assertEqual(launched.call_count, 3)
                        manifest = json.loads((evidence / "capture.json").read_text(encoding="utf-8"))
                        if required:
                            self.assertIs(manifest["regions_required"], True)
                        else:
                            self.assertNotIn("regions_required", manifest)
                    else:
                        with self.assertRaises(SystemExit):
                            capture.main()
                        self.assertFalse((evidence / "capture.json").exists())
                        if enabled is not True:
                            launched.assert_not_called()


if __name__ == "__main__":
    unittest.main()
