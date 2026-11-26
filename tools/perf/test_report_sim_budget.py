#!/usr/bin/env python3
"""Representative report contracts over two-tick artifacts; no engine launch."""

from __future__ import annotations

import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_sim_budget as capture
import report_sim_budget as report
from test_capture_sim_budget import artifact
from test_sim_budget_gate import run_tool


class SimBudgetReportTests(unittest.TestCase):
    def setUp(self):
        # Derive every shipping gate from the real document once per case;
        # repeated validation need not reopen that unchanged document.
        gates = mock.patch.object(capture, "shipping_simulation_gates",
                                  return_value=capture.shipping_simulation_gates())
        gates.start()
        self.addCleanup(gates.stop)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.before = self.write_capture("before")
        self.after = self.write_capture("after")

    @staticmethod
    def read(path):
        return json.loads(path.read_text(encoding="utf-8"))

    @staticmethod
    def write(path, data):
        path.write_text(json.dumps(data), encoding="utf-8")

    def write_capture(self, name):
        directory = self.root / name
        directory.mkdir()
        runs = []
        for preset in capture.PRESETS:
            data = artifact()
            data["preset"] = preset
            data["world_hash"] = data["world_hash_replay"] = (
                "0123456789abcdef" if name == "before" else "fedcba9876543210")
            data["effective_simulation"]["future_gate"] = False
            traced = directory / f"{preset}.json"
            disabled = directory / f"{preset}-telemetry-off.json"
            self.write(traced, data)
            del data["sim_budget"]
            self.write(disabled, data)
            command = capture.command(directory / "server", directory / "runtime", traced,
                                      preset, "1337", 2, 1, 1)
            off_command = command.copy()
            off_command.remove("--sim-budget")
            off_command[-1] = str(disabled)
            runs.append({"preset": preset, "artifact": traced.name, "command": command,
                         "telemetry_off": {"artifact": disabled.name, "command": off_command}})
        self.write(directory / "capture.json", {
            "schema": "luminumbra.sim_budget_capture.v1",
            "revision": ("a" if name == "before" else "b") * 40,
            "source_dirty": False, "binary_sha256": ("c" if name == "before" else "d") * 64,
            "fixture_hash": "e" * 64, "preset_hash": "f" * 64,
            "seed": "1337", "ticks": 2, "surface_radius": 1, "collision_radius": 1,
            "runs": runs,
        })
        return directory

    def invoke(self):
        return run_tool(report, [str(self.before), str(self.after)])

    def assert_refused(self):
        code, output, error = self.invoke()
        self.assertEqual((code, output), (2, ""))
        self.assertTrue(error.startswith("Report refused: "), error)
        return error

    def test_report_covers_three_presets_and_every_stage(self):
        # Keep a byte-for-byte snapshot of every raw capture and manifest.
        paths = sorted(self.root.rglob("*.json"))
        original = {path: path.read_bytes() for path in paths}
        code, output, error = self.invoke()
        self.assertEqual(code, 0, error)
        result = json.loads(output)
        self.assertEqual(result["schema"], report.REPORT_SCHEMA)
        self.assertEqual(result["stage_order"], list(capture.STAGES))
        for endpoint, directory in (("before", self.before), ("after", self.after)):
            measured = result[endpoint]
            manifest = self.read(directory / "capture.json")
            for key in (*report.budget_io.MANIFEST_KEYS, "preset_hash"):
                self.assertEqual(measured["identity"][key],
                                 int(manifest[key]) if key in ("seed", "ticks") else manifest[key])
            self.assertEqual(set(measured["presets"]), set(capture.PRESETS))
            for preset, data in measured["presets"].items():
                self.assertEqual(set(data["stages"]), set(capture.STAGES))
                self.assertEqual(data["effective_simulation"],
                                 self.read(directory / f"{preset}.json")["effective_simulation"])
                self.assertIs(data["telemetry_neutrality"]["passed"], True)
                self.assertIs(data["telemetry_neutrality"]["work_replay_match"], True)
                for name, stage in data["stages"].items():
                    inactive = name == "wind_weather"
                    self.assertEqual(stage["samples"], 0 if inactive else 2)
                    self.assertEqual(stage["work_total"], 0 if inactive else 3)
                    self.assertEqual(stage["max_work_per_tick"], 0 if inactive else 2)
        self.assertEqual(result["workload"], {"ecology_roster": True, "planted_roster": True,
                                              "autosave_ticks": 30, "no_audio": True})
        self.assertIsNone(result["proposal"]["region_scheduler"]["work_limit"])
        self.assertIn("RegionWork", result["proposal"]["region_scheduler"]["reason"])
        self.assertEqual(result["integer_proposal"],
                         report.proposal.build_budgets(result["proposal"]))
        for rows in result["comparison"]["presets"].values():
            self.assertEqual(set(rows), set(capture.STAGES))
            self.assertTrue(all(row["work"] == "same" for row in rows.values()))
        self.assertEqual(original, {path: path.read_bytes() for path in paths})
        self.assertEqual(self.invoke()[1], output)

    def test_report_preserves_all_duration_percentiles(self):
        # Cover both mutually exclusive wind/weather paths and sampled zero work.
        for directory in (self.before, self.after):
            for preset in capture.PRESETS:
                path = directory / f"{preset}.json"
                data = self.read(path)
                for index, stage in enumerate(data["sim_budget"]["stages"]):
                    if preset == "mountains" and stage["name"] in ("wind", "weather", "wind_weather"):
                        active = stage["name"] == "wind_weather"
                        stage.update(samples=2 if active else 0, work_total=3 if active else 0,
                                     work_trace=[{"tick": 1, "work": 1}, {"tick": 2, "work": 2}]
                                     if active else [], duration_ms={} if active else None)
                    if stage["samples"]:
                        stage["duration_ms"] = {key: (index + 1) * (number + 1) / 100
                                                for number, key in enumerate(report.DURATION_KEYS)}
                    if stage["name"] == "animation":
                        stage["work_total"] = 0
                        for sample in stage["work_trace"]:
                            sample["work"] = 0
                self.write(path, data)
        result = report.build_report(self.before, self.after)
        for endpoint, directory in (("before", self.before), ("after", self.after)):
            for preset in capture.PRESETS:
                for source in self.read(directory / f"{preset}.json")["sim_budget"]["stages"]:
                    row = result[endpoint]["presets"][preset]["stages"][source["name"]]
                    self.assertEqual(row["duration_ms"], source["duration_ms"])
        self.assertEqual(result["proposal"]["presets"]["default"]["stages"]["animation"]["status"],
                         "idle")

    def test_report_refuses_incompatible_workload_identity(self):
        path = self.after / "capture.json"
        original = self.read(path)
        original_bytes = {entry: entry.read_bytes() for entry in self.after.glob("*.json")}
        for key, value in (("fixture_hash", "0" * 64), ("preset_hash", "0" * 64),
                           ("surface_radius", 2), ("collision_radius", 2),
                           ("seed", "42"), ("ticks", 3)):
            with self.subTest(key=key):
                changed = copy.deepcopy(original)
                changed[key] = value
                # Keep each endpoint valid; exercise the shared comparator itself.
                if key in ("surface_radius", "collision_radius", "seed", "ticks"):
                    flag = {"surface_radius": "--radius", "collision_radius": "--collision-radius",
                            "seed": "--seed", "ticks": "--ticks"}[key]
                    for run in changed["runs"]:
                        for entry in (run, run["telemetry_off"]):
                            command = entry["command"]
                            command[command.index(flag) + 1] = str(value)
                            artifact_path = self.after / entry["artifact"]
                            data = self.read(artifact_path)
                            data["seed"] = str(changed["seed"])
                            data["ticks_requested"] = changed["ticks"]
                            data["runs"] = [{"ticks_executed": changed["ticks"]}] * 2
                            if "sim_budget" in data:
                                for stage in data["sim_budget"]["stages"]:
                                    if stage["samples"]:
                                        stage["samples"] = changed["ticks"]
                                        stage["work_trace"] = [
                                            {"tick": tick, "work": tick}
                                            for tick in range(1, changed["ticks"] + 1)]
                                        stage["work_total"] = sum(
                                            sample["work"] for sample in stage["work_trace"])
                            self.write(artifact_path, data)
                self.write(path, changed)
                self.assertIn(key, self.assert_refused())
                for entry, contents in original_bytes.items():
                    entry.write_bytes(contents)
        for missing in ("fixture_hash", "preset_hash"):
            changed = copy.deepcopy(original)
            del changed[missing]
            self.write(path, changed)
            self.assert_refused()
        self.write(path, original)

    def test_duration_changes_do_not_change_integer_proposal(self):
        first = report.build_report(self.before, self.after)
        for preset in capture.PRESETS:
            path = self.after / f"{preset}.json"
            data = self.read(path)
            for stage in data["sim_budget"]["stages"]:
                if stage["duration_ms"] is not None:
                    stage["duration_ms"] = dict.fromkeys(report.DURATION_KEYS, 100.0)
            self.write(path, data)
        second = report.build_report(self.before, self.after)
        self.assertEqual(first["integer_proposal"], second["integer_proposal"])
        self.assertNotEqual(first["comparison"], second["comparison"])
        self.assertNotEqual(first["proposal"], second["proposal"])

    def test_unrepresentable_or_zero_duration_ratios_are_null(self):
        for directory, duration in ((self.before, 1e-308), (self.after, 1e308)):
            path = directory / "default.json"
            data = self.read(path)
            data["sim_budget"]["stages"][0]["duration_ms"] = dict.fromkeys(
                report.DURATION_KEYS, duration)
            data["sim_budget"]["stages"][1]["duration_ms"] = dict.fromkeys(
                report.DURATION_KEYS, 0)
            self.write(path, data)
        code, output, error = self.invoke()
        self.assertEqual(code, 0, error)
        result = json.loads(output, parse_constant=lambda value: self.fail(value))
        rows = result["comparison"]["presets"]["default"]
        for name in ("server_physics", "animation", "wind_weather"):
            self.assertIsNone(rows[name]["p99_ratio"])

    def test_report_refuses_missing_neutrality_evidence(self):
        path = self.after / "capture.json"
        original = self.read(path)
        for counterpart in (None, {}, [], {"artifact": "missing.json"}):
            with self.subTest(counterpart=counterpart):
                changed = copy.deepcopy(original)
                changed["runs"][-1]["telemetry_off"] = counterpart
                self.write(path, changed)
                self.assert_refused()
        changed = copy.deepcopy(original)
        del changed["runs"][-1]["telemetry_off"]
        self.write(path, changed)
        self.assert_refused()
        self.write(path, original)
        disabled_path = self.after / "archipelago-telemetry-off.json"
        for contents in (None, "{", "[]"):
            if contents is None:
                disabled_path.unlink()
            else:
                disabled_path.write_text(contents, encoding="utf-8")
            self.assert_refused()

    def test_report_refuses_corrupt_or_escaping_artifacts(self):
        path = self.after / "default-telemetry-off.json"
        original = self.read(path)
        for changes in ({"sim_budget": None}, {"world_hash": "0" * 16},
                        {"runs": []}, {"passed": False},
                        {"effective_simulation": {**original["effective_simulation"], "future_gate": 0}}):
            self.write(path, {**original, **changes})
            self.assert_refused()
        self.write(path, original)
        manifest_path = self.after / "capture.json"
        manifest = self.read(manifest_path)
        for reference in ("../before/default.json", "C:artifact.json", "/artifact.json", ""):
            for entry in (manifest["runs"][0], manifest["runs"][0]["telemetry_off"]):
                original_reference = entry["artifact"]
                entry["artifact"] = reference
                self.write(manifest_path, manifest)
                self.assert_refused()
                entry["artifact"] = original_reference
        self.write(manifest_path, manifest)
        with mock.patch.object(report.budget_io, "read_json", side_effect=PermissionError("denied")):
            self.assert_refused()

    def test_report_refuses_incomplete_presets_and_workloads(self):
        path = self.after / "capture.json"
        original = self.read(path)
        malformed = ["{", "[]"] + [json.dumps({**original, key: value}) for key, value in (
            ("schema", "future"), ("source_dirty", None), ("binary_sha256", "invalid"),
            ("preset_hash", None), ("seed", True), ("ticks", 0), ("surface_radius", 0))]
        for contents in malformed:
            path.write_text(contents, encoding="utf-8")
            self.assert_refused()
        self.write(path, original)
        for runs in ([], original["runs"][:2], [original["runs"][0]] * 3,
                     [None] * 3, [{"preset": "unknown"}] * 3):
            self.write(path, {**original, "runs": runs})
            self.assert_refused()
        for endpoint in ("command", "telemetry_off"):
            for invalid in (None, [], ["unknown"], "command"):
                changed = copy.deepcopy(original)
                if endpoint == "command":
                    changed["runs"][0]["command"] = invalid
                else:
                    changed["runs"][0]["telemetry_off"]["command"] = invalid
                self.write(path, changed)
                self.assert_refused()
        for flag in ("--ecology-roster", "--planted-roster", "--no-audio", "--autosave-ticks"):
            changed = copy.deepcopy(original)
            command = changed["runs"][0]["command"]
            command[command.index(flag)] = "--unknown"
            self.write(path, changed)
            self.assert_refused()
        changed = copy.deepcopy(original)
        changed["runs"][0]["telemetry_off"]["command"][2] = "different-root"
        self.write(path, changed)
        self.assert_refused()

    def test_report_refuses_invalid_trace_duration_and_cross_revision_gates(self):
        path = self.after / "default.json"
        original = self.read(path)
        for changes in ({"samples": 1}, {"work_trace": []}, {"duration_ms": None},
                        {"duration_ms": dict.fromkeys(report.DURATION_KEYS, 10**400)},
                        {"duration_ms": {"p50": 5, "p95": 2, "p99": 3, "maximum": 4}}):
            changed = copy.deepcopy(original)
            changed["sim_budget"]["stages"][0].update(changes)
            self.write(path, changed)
            self.assert_refused()
        self.write(path, original)
        for name in ("default.json", "default-telemetry-off.json"):
            path = self.after / name
            changed = self.read(path)
            changed["effective_simulation"]["future_gate"] = True
            self.write(path, changed)
        self.assertIn("effective simulation gates", self.assert_refused())

    def test_report_cli_supports_isolation_and_output_failure(self):
        result = subprocess.run([sys.executable, "-I", "-B", str(Path(report.__file__)),
                                 str(self.before), str(self.after)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), report.build_report(self.before, self.after))
        arguments = [str(self.before), str(self.after), "--headroom-num", "5", "--headroom-den", "2"]
        code, output, error = run_tool(report, arguments)
        self.assertEqual(code, 0, error)
        proposed = json.loads(output)["integer_proposal"]
        self.assertEqual(proposed["headroom"], {"num": 5, "den": 2})
        self.assertEqual(proposed["presets"]["default"]["stages"]["creatures"]["max_work_per_tick"], 5)
        for flag in ("--headroom-num", "--headroom-den"):
            for value in ("0", "-1", "invalid"):
                with mock.patch.object(report, "build_report") as build, \
                        mock.patch.object(sys, "stderr", io.StringIO()):
                    with self.assertRaises(SystemExit) as refused:
                        report.main([str(self.before), str(self.after), flag, value])
                    self.assertEqual(refused.exception.code, 2)
                    build.assert_not_called()
        for operation in ("write", "flush"):
            output = io.StringIO()
            with mock.patch.object(output, operation, side_effect=OSError("full")), \
                    mock.patch.object(sys, "stdout", output), \
                    mock.patch.object(sys, "stderr", io.StringIO()):
                self.assertEqual(report.main([str(self.before), str(self.after)]), 2)

    def test_report_keeps_cross_platform_command_paths_and_ignores_additive_metadata(self):
        for binary, runtime, output in (("C:\\measurements\\server.exe", "C:\\fixture", "C:\\captures"),
                                        ("/measurements/server", "/fixture", "/captures")):
            for directory in (self.before, self.after):
                path = directory / "capture.json"
                manifest = self.read(path)
                manifest["additional_metadata"] = {"future": True}
                for run in manifest["runs"]:
                    for entry in (run, run["telemetry_off"]):
                        entry["command"][0] = binary
                        entry["command"][2] = runtime
                        entry["command"][-1] = output + "/" + entry["artifact"]
                        artifact_path = directory / entry["artifact"]
                        data = self.read(artifact_path)
                        data["additional_metadata"] = {"future": True}
                        self.write(artifact_path, data)
                self.write(path, manifest)
            code, _, error = self.invoke()
            self.assertEqual(code, 0, error)


if __name__ == "__main__":
    unittest.main()
