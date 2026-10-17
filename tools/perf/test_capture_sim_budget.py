#!/usr/bin/env python3
"""Capture command and evidence refusal tests; no engine or third-party modules needed."""

from __future__ import annotations

import copy
import contextlib
import io
import json
from pathlib import Path
import shlex
import tempfile
import unittest
from unittest import mock

import capture_sim_budget as capture
from fixture_hash import fixture_hash
from test_fixture_hash import ROOTED_REFERENCES, ESCAPING_REFERENCES


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


class SimBudgetCaptureTests(unittest.TestCase):
    def test_capture_refuses_external_preset_references_before_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "data/common").mkdir(parents=True)
            presets = root / "worlds/atlas/presets"
            presets.mkdir(parents=True)
            binary = root / "server"
            binary.write_bytes(b"synthetic server")
            evidence = root / "evidence"
            for reference in ROOTED_REFERENCES + ESCAPING_REFERENCES:
                (presets / "default.json").write_text(json.dumps({
                    "generation_params": {"biomes": {"table": reference}}}))
                with self.subTest(reference=reference), \
                        mock.patch.object(capture, "__file__", str(root / "tools/perf/capture_sim_budget.py")), \
                        mock.patch("sys.argv", ["capture_sim_budget.py", "--binary", str(binary),
                                                "--evidence-dir", str(evidence)]), \
                        mock.patch.object(capture.subprocess, "run") as launch, \
                        contextlib.redirect_stderr(io.StringIO()) as error:
                    with self.assertRaises(SystemExit) as refused:
                        capture.main()
                    self.assertEqual(refused.exception.code, 1)
                    self.assertIn("fixture reference", error.getvalue())
                    launch.assert_not_called()
                    self.assertFalse(evidence.exists())

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
        with mock.patch.object(capture, "read_text", return_value=json.dumps(systems)):
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
        for value in (None, -1, float("nan"), float("inf"), True, 2.0):
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
            self.assertEqual(manifest["fixture_hash"], fixture_hash(root / "data/common",
                                                                   scope="headless-server"))
            self.assertEqual(manifest["preset_hash"], fixture_hash(root / "worlds/atlas/presets",
                                                                  reference_root=root / "data"))
            self.assertEqual(manifest["seed"], "1337")
            self.assertEqual(manifest["surface_radius"], 1)
            self.assertEqual(manifest["collision_radius"], 1)
            self.assertEqual([run["command"] for run in manifest["runs"]], commands)
            self.assertEqual([run["preset"] for run in manifest["runs"]], list(capture.PRESETS))

    def test_capture_refuses_fixture_changes_during_runs(self):
        systems = (Path(capture.__file__).resolve().parents[2] / "data/common/systems.json").read_bytes()
        data = artifact()
        for changed in ("data/common/value.json", "worlds/atlas/presets/default.json"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "data/common").mkdir(parents=True)
                (root / "data/common/systems.json").write_bytes(systems)
                (root / "worlds/atlas/presets").mkdir(parents=True)
                (root / changed).write_text('{}')
                binary = root / "server"
                binary.write_bytes(b"synthetic server")
                evidence = root / "evidence"
                runs = []

                def run(argv, **kwargs):
                    current = dict(data, preset=argv[argv.index("--preset") + 1])
                    Path(argv[argv.index("--artifact") + 1]).write_text(json.dumps(current))
                    runs.append(current["preset"])
                    if len(runs) == 2:
                        (root / changed).write_text('{"changed":1}')

                with mock.patch.object(capture, "__file__", str(root / "tools/perf/capture_sim_budget.py")), \
                        mock.patch("sys.argv", ["capture_sim_budget.py", "--binary", str(binary),
                                               "--evidence-dir", str(evidence), "--ticks", "2"]), \
                        mock.patch.object(capture.subprocess, "check_output", side_effect=["a" * 40, ""]), \
                        mock.patch.object(capture.subprocess, "run", side_effect=run), \
                        contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(io.StringIO()) as error, \
                        self.assertRaises(SystemExit) as refused:
                    capture.main()
                self.assertEqual(refused.exception.code, 1)
                self.assertIn("fixture hashes changed during capture", error.getvalue())
                self.assertEqual(runs, list(capture.PRESETS))
                self.assertFalse((evidence / "capture.json").exists())

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


if __name__ == "__main__":
    unittest.main()
