#!/usr/bin/env python3
"""Contracts for the smoke report and probe launch wrapper."""

import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import check_worldgen_probe_smoke as checker


def valid_report():
    def stats(samples):
        return dict(samples=samples, min_ms=0.0, median_ms=1.0, max_ms=2.0,
                    total_ms=2.0 * samples)

    phases = {name: stats(2 if name in checker.PHASES[:3] or name == "shaped_heights" else 1024)
              for name in checker.PHASES}
    phases["polygonise"]["mesh_triangles"] = 1
    for name in checker.PHASES[3:]:
        phases[name]["grid"] = "32x32"
    phases["height_query_cold"]["first_call_ms"] = 0.0
    phases["shaped_heights"].update(positions=1024, reps=2)
    libm = dict(calls=1000, functions={
        name: dict(calls=1000, total_ms=0.0, ns_per_call=0.0)
        for name in checker.LIBM_FUNCTIONS})
    return dict(schema=checker.SCHEMA, mode="smoke", compiler="fixture compiler",
                cpu="unknown", preset="default.json", hardware_concurrency=0,
                default_worker_count=1, seed=424242, requested_chunks=2,
                selected_chunks=2, initial_chunk_list_size=10,
                spawn=dict(x=8.0, y=-2.0, z=8.0), phases=phases,
                scaling=[dict(requested_workers=requested, worker_count=1,
                              chunks=2, wall_ms=0.0)
                         for requested in (1, 2, 4, "default")],
                libm=libm, libm_after_generation=copy.deepcopy(libm))


class SmokeReportTests(unittest.TestCase):
    def test_valid_report_allows_zero_timing_and_different_actual_workers(self):
        checker.validate_report(valid_report())

    def test_reduced_chunk_coverage_is_rejected(self):
        report = valid_report()
        report["selected_chunks"] = 1
        for phase in checker.PHASES[:3]:
            report["phases"][phase].update(samples=1, total_ms=2.0)
        for entry in report["scaling"]:
            entry["chunks"] = 1
        with self.assertRaises(ValueError) as error:
            checker.validate_report(report)
        self.assertEqual(str(error.exception), "selected_chunks must match requested_chunks")

    def test_zero_mesh_triangles_are_rejected(self):
        report = valid_report()
        report["phases"]["polygonise"]["mesh_triangles"] = 0
        with self.assertRaises(ValueError) as error:
            checker.validate_report(report)
        self.assertEqual(str(error.exception), "polygonise.mesh_triangles must be an integer >= 1")

    def test_missing_essential_fields(self):
        for field in valid_report():
            with self.subTest(field=field):
                report = valid_report()
                del report[field]
                with self.assertRaises(ValueError):
                    checker.validate_report(report)

    def test_schema_mode_and_types(self):
        for field, value in (("schema", "other"), ("mode", "full"),
                             ("compiler", ""), ("cpu", 3), ("preset", None),
                             ("seed", True), ("requested_chunks", 2.0),
                             ("selected_chunks", 0), ("hardware_concurrency", -1),
                             ("default_worker_count", False), ("phases", []),
                             ("spawn", []), ("scaling", {})):
            with self.subTest(field=field, value=value):
                report = valid_report()
                report[field] = value
                with self.assertRaises(ValueError):
                    checker.validate_report(report)

    def test_all_phases_and_sample_counts_required(self):
        for phase in checker.PHASES:
            for failure in ("missing", "samples", "timing", "order"):
                with self.subTest(phase=phase, failure=failure):
                    report = valid_report()
                    if failure == "missing":
                        del report["phases"][phase]
                    elif failure == "samples":
                        report["phases"][phase]["samples"] += 1
                    elif failure == "timing":
                        del report["phases"][phase]["median_ms"]
                    else:
                        report["phases"][phase]["min_ms"] = 5
                    with self.assertRaises(ValueError):
                        checker.validate_report(report)

    def test_invalid_numeric_values(self):
        for value in (-1, True, "0", None, float("nan"), float("inf"), 10 ** 400):
            with self.subTest(value=value):
                report = valid_report()
                report["phases"]["generate_step1"]["total_ms"] = value
                with self.assertRaises(ValueError):
                    checker.validate_report(report)

    def test_auxiliary_phase_fields(self):
        for phase, field, value in (("polygonise", "mesh_triangles", False),
                                    ("height_query_cold", "first_call_ms", -1),
                                    ("height_query_warm", "grid", "16x16"),
                                    ("shaped_heights", "positions", 1),
                                    ("shaped_heights", "reps", 8)):
            with self.subTest(phase=phase, field=field):
                report = valid_report()
                report["phases"][phase][field] = value
                with self.assertRaises(ValueError):
                    checker.validate_report(report)

    def test_scaling_fields(self):
        for field, value in (("requested_workers", True), ("worker_count", 0),
                             ("chunks", 1), ("wall_ms", float("inf"))):
            with self.subTest(field=field):
                report = valid_report()
                report["scaling"][0][field] = value
                with self.assertRaises(ValueError):
                    checker.validate_report(report)
        report = valid_report()
        report["scaling"].pop()
        with self.assertRaises(ValueError):
            checker.validate_report(report)

    def test_both_libm_observations(self):
        for observation in ("libm", "libm_after_generation"):
            for failure in ("function", "calls", "function_calls", "timing", "type"):
                with self.subTest(observation=observation, failure=failure):
                    report = valid_report()
                    suite = report[observation]
                    if failure == "function":
                        del suite["functions"]["sinf"]
                    elif failure == "calls":
                        suite["calls"] = 1
                    elif failure == "function_calls":
                        suite["functions"]["sinf"]["calls"] = 1
                    elif failure == "timing":
                        suite["functions"]["sinf"]["ns_per_call"] = -1
                    else:
                        suite["functions"]["sinf"] = []
                    with self.assertRaises(ValueError):
                        checker.validate_report(report)

    def test_strict_json_and_missing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            with self.assertRaises(OSError):
                checker.validate_output(path)
            for content in ("{", "null", '{"schema": "a", "schema": "b"}',
                            '{"timing": NaN}', '{"timing": Infinity}',
                            json.dumps(valid_report()).replace('"total_ms": 4.0', '"total_ms": 1e999', 1)):
                with self.subTest(content=content[:40]):
                    path.write_text(content, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        checker.validate_output(path)

    def test_wrapper_removes_stale_output_and_passes_original_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            path.write_text("stale", encoding="utf-8")

            def probe(command, check):
                self.assertFalse(path.exists())
                self.assertEqual(command, ["fixture-probe", "--smoke", "--json", str(path)])
                self.assertFalse(check)
                path.write_text(json.dumps(valid_report()), encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(checker.subprocess, "run", side_effect=probe):
                self.assertEqual(checker.main(["--probe", "fixture-probe", "--out", str(path)]), 0)

    def test_wrapper_propagates_failure_before_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            path.write_text(json.dumps(valid_report()), encoding="utf-8")
            with mock.patch.object(checker.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)), \
                    mock.patch.object(checker, "validate_output") as validate:
                self.assertEqual(checker.main(["--probe", "fixture-probe", "--out", str(path)]), 7)
                validate.assert_not_called()
                self.assertFalse(path.exists())

    def test_wrapper_rejects_success_without_fresh_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            path.write_text(json.dumps(valid_report()), encoding="utf-8")
            with mock.patch.object(checker.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
                self.assertEqual(checker.main(["--probe", "fixture-probe", "--out", str(path)]), 1)


if __name__ == "__main__":
    unittest.main()
