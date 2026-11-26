#!/usr/bin/env python3
"""Fast file-only regional admission report regressions."""

from __future__ import annotations

import contextlib
import copy
import hashlib
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
import regional_admission_report as report
from test_sim_budget_gate import write_capture


def write_json(path: Path, value: object) -> dict:
    path.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")
    return {"artifact": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


class RegionalAdmissionReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def evidence(self, name="capture", workload="populated") -> Path:
        directory = write_capture(self.root / name, overrides={"preset_hash": "b" * 64})
        setup = {"schema": "luminumbra.regional_admission_setup.v1", "active_regions": 3,
                 "preparation": "Synthetic file-only preparation", "inputs": [],
                 "frozen_history_records": 128 if workload == "growing-frozen-history" else 0,
                 "boundary_transitions": 1 if workload == "mixed-cadence-boundary" else 0,
                 "cadence_ticks": [1, 2] if workload == "mixed-cadence-boundary" else [1]}
        sidecar = {"schema": "luminumbra.regional_admission_evidence.v1", "workload": workload,
                   "setup": write_json(directory / "setup.json", setup),
                   "host": dict.fromkeys(report.HOST_KEYS, "synthetic"),
                   "build": dict.fromkeys(report.BUILD_KEYS, "synthetic")}
        write_json(directory / "regional-admission.json", sidecar)
        return directory

    def change_sidecar(self, directory, mutate):
        path = directory / "regional-admission.json"
        sidecar = json.loads(path.read_text(encoding="utf-8"))
        mutate(sidecar)
        write_json(path, sidecar)

    def receipt(self, directory, observations):
        result = report.load_report(directory)
        sidecar = json.loads((directory / "regional-admission.json").read_text(encoding="utf-8"))
        receipts = {}
        for preset in capture.PRESETS:
            receipt = {"schema": "luminumbra.regional_admission_paths.v1",
                       "workload_sha256": result["workload_sha256"],
                       "capture_sha256": result["presets"][preset]["capture_sha256"],
                       "observations": observations}
            receipts[preset] = write_json(directory / f"{preset}-paths.json", receipt)
        sidecar["path_evidence"] = receipts
        write_json(directory / "regional-admission.json", sidecar)

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = report.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_report_is_deterministic(self):
        directory = self.evidence()
        observations = [{"path": path, "tick": 1, "count": 1}
                        for path in report.document(report.DEFINITIONS.parent,
                                                    report.DEFINITIONS.name)[0]["paths"]]
        self.receipt(directory, observations)
        code, first, err = self.run_tool(str(directory))
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(self.run_tool(str(directory)), (code, first, err))
        data = json.loads(first)
        for preset in data["presets"].values():
            self.assertEqual(preset["coverage"], "complete")
            self.assertEqual(preset["world_hash"], preset["world_hash_replay"])
            self.assertEqual(preset["stages"][0]["duration_ms"]["maximum"], 1)
            self.assertEqual(len(preset["stages"][0]["work_trace"]), 4)
        # JSON object order and capture location do not change workload identity.
        second = self.evidence("second")
        self.assertEqual(report.load_report(second)["identity"], data["identity"])
        self.assertEqual(report.digest({"b": 1, "a": 2}), report.digest({"a": 2, "b": 1}))

    def test_mismatched_workloads_refuse(self):
        before = report.load_report(self.evidence())
        changes = {key: "changed" for key in before["identity"]}
        changes.update(seed=2, ticks=5, surface_radius=2, collision_radius=2)
        for key, value in changes.items():
            with self.subTest(key=key):
                after = copy.deepcopy(before)
                after["identity"][key] = value
                with self.assertRaisesRegex(ValueError, "reports differ"):
                    report.compare_reports(before, after)
        after = copy.deepcopy(before)
        after["build"]["binary_sha256"] = "c" * 64
        after["build"]["revision"] = "d" * 40
        self.assertEqual(report.compare_reports(before, after)["presets"]["default"]
                         ["plants"]["work"], "same")
        self.assertEqual(report.compare_reports(before, after)["schema"],
                         "luminumbra.regional_admission_compare.v1")
        different = self.evidence("history", "growing-frozen-history")
        code, out, err = self.run_tool(str(self.root / "capture"), "--compare", str(different))
        self.assertEqual((code, out), (2, ""))
        self.assertIn("reports differ", err)

    def test_unexercised_paths_remain_unevaluated(self):
        directory = self.evidence()
        self.receipt(directory, [{"path": "entity_admission", "tick": 1, "count": 2},
                                 {"path": "crop_birth", "tick": 2, "count": 0}])
        for preset in report.load_report(directory)["presets"].values():
            self.assertEqual(preset["coverage"], "unevaluated")
            self.assertEqual(preset["paths"]["entity_admission"]["status"], "exercised")
            for path in ("crop_birth", "event_continuation", "ground_settlement"):
                self.assertEqual(preset["paths"][path]["status"], "unevaluated")
        self.change_sidecar(directory, lambda sidecar: sidecar.pop("path_evidence"))
        self.receipt(directory, [])
        self.assertTrue(all(row["status"] == "unevaluated" for row in
                            report.load_report(directory)["presets"]["default"]["paths"].values()))

    def test_changed_work_is_visible_despite_equal_timings(self):
        directory = self.evidence()
        before = report.load_report(directory)
        path = directory / "default.json"
        artifact = json.loads(path.read_text(encoding="utf-8"))
        plants = next(stage for stage in artifact["sim_budget"]["stages"] if stage["name"] == "plants")
        plants["work_trace"][0]["work"], plants["work_trace"][1]["work"] = 2, 1
        write_json(path, artifact)
        after = report.load_report(directory)
        row = report.compare_reports(before, after)["presets"]["default"]["plants"]
        self.assertEqual(row["p99_ratio"], 1)
        self.assertEqual(row["work_total_before"], row["work_total_after"])
        self.assertEqual(row["work"], "different")

    def test_work_items_and_hash_changes_are_reported(self):
        directory = self.evidence()
        path = directory / "default.json"
        artifact = json.loads(path.read_text(encoding="utf-8"))
        artifact["sim_budget"]["work_items"] = {"schema": "luminumbra.sim_work.v1", "counters": [
            {"name": name, "work_trace": [{"tick": tick, "work": 1} for tick in range(1, 5)]}
            for name in capture.WORK_ITEMS]}
        write_json(path, artifact)
        before = report.load_report(directory)
        artifact["sim_budget"]["work_items"]["counters"][0]["work_trace"][0]["work"] = 2
        artifact["world_hash"] = artifact["world_hash_replay"] = "0" * 16
        write_json(path, artifact)
        comparison = report.compare_reports(before, report.load_report(directory))["presets"]["default"]
        self.assertEqual(comparison["work_items"]["work"], "different")
        self.assertEqual(comparison["world_hash"], "different")

    def test_missing_and_malformed_inputs_refuse(self):
        directory = self.evidence()
        for reference in ("missing.json", "../escape.json", "", "C:\\evidence.json"):
            with self.subTest(reference=reference):
                code, out, _ = self.run_tool(str(directory), "--evidence", reference)
                self.assertEqual((code, out), (2, ""))
        malformed = ("", "{", "[]", '{"schema":1,"schema":2}', '{"value":NaN}')
        for text in malformed:
            (directory / "invalid.json").write_text(text, encoding="utf-8")
            self.assertEqual(self.run_tool(str(directory), "--evidence", "invalid.json")[0], 2)
        for mutate in (
                lambda item: item.update(schema="unknown"),
                lambda item: item.update(workload="unknown"),
                lambda item: item.update(host={}),
                lambda item: item.update(build={}),
                lambda item: item.update(path_evidence=None),
                lambda item: item.update(path_evidence=[]),
                lambda item: item.update(path_evidence={"unknown": {}}),
                lambda item: item.update(path_evidence={"default": None}),
                lambda item: item.update(setup={"artifact": "setup.json", "sha256": "0" * 64}),
                lambda item: item.update(setup={"artifact": "missing.json", "sha256": "0" * 64}),
                lambda item: item.update(setup={"artifact": "setup.json", "sha256": True})):
            original = (directory / "regional-admission.json").read_bytes()
            self.change_sidecar(directory, mutate)
            self.assertEqual(self.run_tool(str(directory))[0], 2)
            (directory / "regional-admission.json").write_bytes(original)
        for label, keys in (("build", report.BUILD_KEYS), ("host", report.HOST_KEYS)):
            for key in keys:
                for value in (None, "", " ", True, 1, []):
                    with self.subTest(label=label, key=key, value=value):
                        self.change_sidecar(directory, lambda item: item[label].update({key: value}))
                        self.assertEqual(self.run_tool(str(directory))[0], 2)
                        (directory / "regional-admission.json").write_bytes(original)

    def test_preparation_input_pins_and_read_failures(self):
        directory = self.evidence()
        input_path = directory / "initial-state.bin"
        input_path.write_bytes(b"\x00\x01\x02\xff")
        pin = {"artifact": input_path.name, "sha256": hashlib.sha256(input_path.read_bytes()).hexdigest()}
        path = directory / "setup.json"
        setup = json.loads(path.read_text(encoding="utf-8"))
        setup["inputs"] = [pin]
        setup_pin = write_json(path, setup)
        self.change_sidecar(directory, lambda item: item.update(setup=setup_pin))
        self.assertEqual(self.run_tool(str(directory))[0], 0)
        input_path.write_bytes(b"changed")
        self.assertEqual(self.run_tool(str(directory))[0], 2)
        input_path.unlink()
        self.assertEqual(self.run_tool(str(directory))[0], 2)
        for key, value in (("inputs", None), ("inputs", [pin] * 17), ("inputs", [{}]),
                           ("preparation", ""), ("preparation", True)):
            setup_pin = write_json(path, dict(setup, **{key: value}))
            self.change_sidecar(directory, lambda item: item.update(setup=setup_pin))
            self.assertEqual(self.run_tool(str(directory))[0], 2)
        with mock.patch.object(report, "read_bytes", side_effect=PermissionError("denied")):
            self.assertEqual(self.run_tool(str(directory))[0], 2)
        (directory / "subdirectory").mkdir()
        self.assertEqual(self.run_tool(str(directory), "--evidence", "subdirectory")[0], 2)

    def test_isolated_cli_accepts_crlf_and_shared_loader_keeps_validated_objects(self):
        directory = self.evidence()
        path = directory / "regional-admission.json"
        path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        result = subprocess.run([sys.executable, "-I", "-B", str(Path(report.__file__)), str(directory)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), report.load_report(directory))
        load_capture = report.sim_budget_io.load_capture

        def load_then_remove(*args, **kwargs):
            loaded = load_capture(*args, **kwargs)
            for preset in capture.PRESETS:
                (directory / f"{preset}.json").unlink()
            return loaded

        with mock.patch.object(report.sim_budget_io, "load_capture", side_effect=load_then_remove):
            self.assertEqual(self.run_tool(str(directory))[0], 0)

    def test_stale_unknown_and_duplicate_path_receipts_refuse(self):
        directory = self.evidence()
        invalid = [None, {}, [None], [{}]]
        for key, values in (("path", ["unknown", [], None]), ("tick", [0, 5, True, 1.5]),
                            ("count", [-1, True, 2**64, 1.5])):
            for value in values:
                observation = {"path": "crop_birth", "tick": 1, "count": 1}
                observation[key] = value
                invalid.append([observation])
        invalid.append([{"path": "crop_birth", "tick": 1, "count": 1}] * 2)
        for observations in invalid:
            self.receipt(directory, observations)
            self.assertEqual(self.run_tool(str(directory))[0], 2)
            self.change_sidecar(directory, lambda sidecar: sidecar.pop("path_evidence"))
        for field in ("schema", "capture_sha256", "workload_sha256"):
            self.receipt(directory, [])
            path = directory / "default-paths.json"
            data = json.loads(path.read_text(encoding="utf-8"))
            data[field] = "stale"
            pin = write_json(path, data)
            self.change_sidecar(directory, lambda sidecar: sidecar["path_evidence"].update(default=pin))
            self.assertEqual(self.run_tool(str(directory))[0], 2)
            self.change_sidecar(directory, lambda sidecar: sidecar.pop("path_evidence"))

    def test_workload_setup_cases(self):
        for workload in ("populated", "mixed-cadence-boundary", "growing-frozen-history"):
            directory = self.evidence(workload, workload)
            self.assertEqual(self.run_tool(str(directory))[0], 0)
            path = directory / "setup.json"
            original = json.loads(path.read_text(encoding="utf-8"))
            cases = [(key, value) for key in ("active_regions", "frozen_history_records", "boundary_transitions")
                     for value in (True, -1, 2**64, "1", None)]
            cases += [("active_regions", 0), ("schema", "unknown")]
            cases += [("cadence_ticks", value) for value in (None, [], [True], [0], [1, 1], [2**64])]
            if workload == "mixed-cadence-boundary":
                cases += [("cadence_ticks", [1]), ("boundary_transitions", 0)]
            if workload == "growing-frozen-history":
                cases += [("frozen_history_records", 0)]
            for key, value in cases:
                with self.subTest(workload=workload, key=key, value=value):
                    pin = write_json(path, dict(original, **{key: value}))
                    self.change_sidecar(directory, lambda sidecar: sidecar.update(setup=pin))
                    self.assertEqual(self.run_tool(str(directory))[0], 2)

    def test_receipts_are_bound_to_each_build_despite_identical_artifacts(self):
        directory = self.evidence()
        self.receipt(directory, [{"path": "crop_birth", "tick": 1, "count": 1}])
        path = directory / "capture.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        for key, value in (("revision", "b" * 40), ("binary_sha256", "b" * 64), ("source_dirty", True)):
            write_json(path, dict(manifest, **{key: value}))
            code, out, err = self.run_tool(str(directory))
            self.assertEqual((code, out), (2, ""))
            self.assertIn("capture/workload identity mismatch", err)
        write_json(path, manifest)
        self.assertEqual(self.run_tool(str(directory))[0], 0)

    def test_capture_validation_and_bounds_are_reused(self):
        directory = self.evidence()
        path = directory / "default.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        mutations = [lambda artifact: artifact.update(passed=False),
                       lambda artifact: artifact.update(world_hash_replay="0" * 16),
                       lambda artifact: artifact["sim_budget"].update(work_items=None)]
        for gate in original["effective_simulation"]:
            mutations.append(lambda artifact, name=gate: artifact["effective_simulation"].update({name: False}))
        for mutate in mutations:
            data = copy.deepcopy(original)
            mutate(data)
            write_json(path, data)
            self.assertEqual(self.run_tool(str(directory))[0], 2)
        write_json(path, original)
        with mock.patch.object(report, "CAPTURE_BYTES", 10):
            self.assertEqual(self.run_tool(str(directory))[0], 2)
        with mock.patch.object(report, "EVIDENCE_BYTES", 10):
            self.assertEqual(self.run_tool(str(directory))[0], 2)
        manifest_path = directory / "capture.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for data in (dict(manifest, runs=manifest["runs"] * 2),
                     dict(manifest, runs=manifest["runs"][:-1]),
                     {key: value for key, value in manifest.items() if key != "preset_hash"}):
            write_json(manifest_path, data)
            self.assertEqual(self.run_tool(str(directory))[0], 2)

    def test_registered_in_ctest_and_standalone_ci(self):
        root = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(root / "tools/ci"))
        import ci_paths

        self.assertTrue(ci_paths.needs_python("tools/perf/fixtures/regional-admission-workloads.json"))
        self.assertIn(("tools/perf/test_regional_admission_report.py",), ci_paths.python_test_commands(root))
        cmake = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('add_test(NAME RegionalAdmissionReportContract', cmake)
        self.assertIn('-I -B "${CMAKE_SOURCE_DIR}/tools/perf/test_regional_admission_report.py"', cmake)
        ci = (root / "tools/ci/ci_paths.py").read_text(encoding="utf-8")
        self.assertIn('"tools/perf/test_regional_admission_report.py",', ci)

    def test_workload_recipes_require_each_hot_path(self):
        definitions, _ = report.document(report.DEFINITIONS.parent, report.DEFINITIONS.name)
        common = {"entity_admission", "scent_admission", "plant_environment_sampling",
                  "wind_sampling", "weather_sampling", "aether_sampling", "crop_birth",
                  "event_continuation", "ground_settlement"}
        self.assertEqual(len(definitions["paths"]), len(set(definitions["paths"])))
        self.assertEqual(set(definitions["workloads"]),
                         {"populated", "mixed-cadence-boundary", "growing-frozen-history"})
        for workload in definitions["workloads"].values():
            self.assertTrue(common <= set(workload["required_paths"]))
            self.assertTrue(set(workload["required_paths"]) <= set(definitions["paths"]))
        self.assertTrue({"mixed_cadence", "boundary_admission"} <= set(
            definitions["workloads"]["mixed-cadence-boundary"]["required_paths"]))
        self.assertIn("frozen_history", definitions["workloads"]["growing-frozen-history"]["required_paths"])


if __name__ == "__main__":
    unittest.main()
