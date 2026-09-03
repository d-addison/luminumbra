#!/usr/bin/env python3

from __future__ import annotations

import copy
import json
import platform
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import perf


def run_document(samples: list[float], key: str = "same") -> dict:
    document = {
        "schema": perf.SCHEMA,
        "status": "evaluated",
        "enforcement": "gating",
        "reasons": [],
        "workload": {
            "id": "fixture",
            "version": "1",
            "layer": "algorithms",
            "parameters": {"size": 10},
            "evidence_contract": "server-smoke",
            "fixture_hash": "f" * 40,
        },
        "build": {
            "commit": "a" * 40,
            "preset": "perf",
            "compiler": {"id": "fixture", "version": "1", "target": "fixture"},
            "configuration": {
                "build_type": "RelWithDebInfo",
                "effective_flags_sha256": "c" * 64,
                "asan": "OFF",
                "coverage": "OFF",
                "diligent": "OFF",
                "frame_pointers": "ON",
                "tracy": "OFF",
                "warnings_as_errors": "ON",
            },
            "renderer": "none",
            "binary": "fixture",
            "binary_sha256": "b" * 64,
        },
        "platform": {
            "os": "fixture",
            "os_release": "1",
            "architecture": "fixture",
            "cpu": "fixture",
        },
        "sample_policy": {"warmup": 0, "samples": len(samples)},
        "metric_schema": {"wall_time": {"unit": "ms", "direction": "lower"}},
        "comparability_key": "pending",
        "metrics": {"wall_time": perf.summarize(samples, "ms", "lower")},
    }
    document["comparability_key"] = perf.stable_hash(perf.canonical_identity(document))
    if key != "same":
        document["comparability_key"] = key
    return document


def write_build_manifest(path: Path) -> None:
    path.write_text(
        json.dumps(
            {
                "schema": "luminumbra.performance_build.v1",
                "compiler": {
                    "id": "Python",
                    "version": platform.python_version(),
                    "target": platform.machine() or "test",
                },
                "configuration": {
                    "build_type": "test",
                    "effective_flags_sha256": "c" * 64,
                    "asan": "OFF",
                    "coverage": "OFF",
                    "diligent": "OFF",
                    "frame_pointers": "OFF",
                    "tracy": "OFF",
                    "warnings_as_errors": "OFF",
                },
            }
        ),
        encoding="utf-8",
    )


class PerfContractTest(unittest.TestCase):
    def test_reviewed_calibration_matches_relative_policy(self) -> None:
        calibration_path = Path(__file__).with_name("baselines") / "relative-calibration.json"
        calibration = json.loads(calibration_path.read_text(encoding="utf-8"))

        self.assertEqual(calibration["schema"], "luminumbra.relative_performance_calibration.v1")
        self.assertGreaterEqual(
            calibration["workload"]["paired_observations"], perf.MIN_GATING_SAMPLES
        )
        self.assertEqual(calibration["policy"]["minimum_samples"], perf.MIN_GATING_SAMPLES)
        self.assertEqual(calibration["policy"]["effect_floor_percent"], perf.MIN_EFFECT_PERCENT)
        self.assertEqual(calibration["policy"]["p_value_max"], perf.P_VALUE_MAX)
        self.assertEqual(calibration["observed"]["verdict"], "stable")
        self.assertLess(abs(calibration["observed"]["delta_percent"]), perf.MIN_EFFECT_PERCENT)
        for digest in calibration["raw_evidence_sha256"].values():
            self.assertRegex(digest, r"^[0-9a-f]{64}$")

    def test_summary_records_tail_and_dispersion(self) -> None:
        summary = perf.summarize([1.0, 2.0, 3.0, 4.0, 10.0], "ms")
        self.assertEqual(summary["sample_count"], 5)
        self.assertEqual(summary["p50"], 3.0)
        self.assertGreater(summary["p99"], summary["p95"])
        self.assertEqual(summary["max"], 10.0)
        self.assertEqual(summary["mad"], 1.0)

    def test_clear_slowdown_is_a_regression(self) -> None:
        base = run_document([10.0 + (index % 3) * 0.01 for index in range(20)])
        candidate = run_document([12.0 + (index % 3) * 0.01 for index in range(20)])
        comparison = perf.compare_runs(base, candidate, 5.0)
        self.assertEqual(comparison["status"], "evaluated")
        self.assertEqual(comparison["verdict"], "regression")

    def test_report_only_comparison_preserves_regression_without_failing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory) / "base.json"
            candidate = Path(directory) / "candidate.json"
            output = Path(directory) / "comparison.json"
            base.write_text(
                json.dumps(run_document([10.0 + index * 0.01 for index in range(20)])),
                encoding="utf-8",
            )
            candidate.write_text(
                json.dumps(run_document([12.0 + index * 0.01 for index in range(20)])),
                encoding="utf-8",
            )
            status = perf.main(
                [
                    "compare",
                    "--base",
                    str(base),
                    "--candidate",
                    str(candidate),
                    "--report-only",
                    "--output",
                    str(output),
                ]
            )
            self.assertEqual(status, 0)
            comparison = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(comparison["verdict"], "regression")
            self.assertEqual(comparison["policy"]["enforcement"], "observation")

    def test_exact_small_sample_test_detects_separated_distributions(self) -> None:
        self.assertLess(
            perf.mann_whitney_p([float(value) for value in range(10)],
                                [float(value) for value in range(20, 30)]),
            0.01,
        )

    def test_paired_sign_test_detects_consistent_slowdown(self) -> None:
        base = [10.0 + index * 0.01 for index in range(20)]
        candidate = [value * 1.06 for value in base]
        self.assertLessEqual(perf.paired_sign_p(base, candidate), 0.05)

    def test_mismatched_runs_are_unevaluated(self) -> None:
        base = run_document([10.0] * 10)
        candidate = run_document([10.0] * 10, key="different")
        comparison = perf.compare_runs(base, candidate, 10.0)
        self.assertEqual(comparison["status"], "unevaluated")

    def test_missing_candidate_metric_is_unevaluated(self) -> None:
        base = run_document([10.0] * 10)
        candidate = run_document([10.0] * 10)
        candidate["metrics"] = {}
        self.assertEqual(perf.compare_runs(base, candidate, 10.0)["status"], "unevaluated")

    def test_underpowered_comparison_is_unevaluated(self) -> None:
        base = run_document([10.0])
        candidate = run_document([20.0])
        comparison = perf.compare_runs(base, candidate, 5.0)
        self.assertEqual(comparison["status"], "evaluated")
        self.assertEqual(comparison["verdict"], "warning")

    def test_equivalent_observation_fragments_combine_into_gating_run(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / f"fragment-{index}.json" for index in range(2)]
            for index, path in enumerate(paths):
                document = run_document([10.0 + index] * 10)
                document["enforcement"] = "observation"
                document["comparability_key"] = perf.stable_hash(
                    perf.canonical_identity(document)
                )
                path.write_text(json.dumps(document), encoding="utf-8")
            output = Path(directory) / "combined.json"
            status = perf.main(
                [
                    "combine",
                    "--runs",
                    *(str(path) for path in paths),
                    "--output",
                    str(output),
                ]
            )
            self.assertEqual(status, 0)
            combined = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(combined["enforcement"], "gating")
            self.assertEqual(combined["metrics"]["wall_time"]["sample_count"], 20)
            self.assertEqual(perf.validate_run(combined), [])

    def test_mismatched_fragments_are_unevaluated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / f"fragment-{index}.json" for index in range(2)]
            for index, path in enumerate(paths):
                document = run_document([10.0] * 5)
                document["enforcement"] = "observation"
                document["workload"]["parameters"]["size"] = 10 + index
                document["comparability_key"] = perf.stable_hash(
                    perf.canonical_identity(document)
                )
                path.write_text(json.dumps(document), encoding="utf-8")
            status = perf.main(
                [
                    "combine",
                    "--runs",
                    *(str(path) for path in paths),
                    "--output",
                    str(Path(directory) / "combined.json"),
                ]
            )
            self.assertEqual(status, perf.UNEVALUATED)

    def test_underpowered_fragments_are_unevaluated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fragment = Path(directory) / "fragment.json"
            document = run_document([10.0] * 5)
            document["enforcement"] = "observation"
            document["comparability_key"] = perf.stable_hash(
                perf.canonical_identity(document)
            )
            fragment.write_text(json.dumps(document), encoding="utf-8")
            status = perf.main(
                [
                    "combine",
                    "--runs",
                    str(fragment),
                    "--output",
                    str(Path(directory) / "combined.json"),
                ]
            )
            self.assertEqual(status, perf.UNEVALUATED)

    def test_non_finite_limit_is_unevaluated(self) -> None:
        base = run_document([10.0] * 10)
        candidate = run_document([20.0] * 10)
        self.assertEqual(perf.compare_runs(base, candidate, float("nan"))["status"], "unevaluated")

    def test_higher_is_better_metric_uses_the_correct_direction(self) -> None:
        base = run_document([100.0 + index * 0.01 for index in range(20)])
        candidate = run_document([80.0 + index * 0.01 for index in range(20)])
        for document in (base, candidate):
            samples = document["metrics"]["wall_time"]["samples"]
            document["metrics"]["wall_time"] = perf.summarize(samples, "items/s", "higher")
            document["metric_schema"]["wall_time"] = {
                "unit": "items/s",
                "direction": "higher",
            }
            document["comparability_key"] = perf.stable_hash(
                perf.canonical_identity(document)
            )
        comparison = perf.compare_runs(base, candidate, 10.0)
        self.assertEqual(comparison["verdict"], "regression")

    def test_binary_hash_is_evidence_not_a_comparability_input(self) -> None:
        base = run_document([10.0] * 10)
        candidate = run_document([10.0] * 10)
        base["build"]["binary_sha256"] = "base"
        candidate["build"]["binary_sha256"] = "candidate"
        self.assertEqual(perf.compare_runs(base, candidate, 10.0)["status"], "evaluated")

    def test_validation_rejects_inconsistent_summary(self) -> None:
        document = run_document([1.0, 2.0, 3.0])
        broken = copy.deepcopy(document)
        broken["metrics"]["wall_time"]["p50"] = 99.0
        self.assertTrue(perf.validate_run(broken))

    def test_validation_rejects_tampered_comparability_key(self) -> None:
        document = run_document([1.0, 2.0, 3.0])
        document["comparability_key"] = "0" * 64
        self.assertIn(
            "comparability_key does not match the result identity",
            perf.validate_run(document),
        )

    def test_validation_rejects_evaluated_result_without_metrics(self) -> None:
        document = run_document([1.0, 2.0, 3.0])
        document["metrics"] = {}
        self.assertIn("evaluated result has no metrics", perf.validate_run(document))

    def test_validation_rejects_gpu_result_without_hardware_identity(self) -> None:
        document = run_document([1.0, 2.0, 3.0])
        document["workload"]["layer"] = "rendering-gpu"
        document["platform"]["gpu"] = "unknown"
        document["platform"]["driver"] = "unknown"
        self.assertIn(
            "evaluated GPU result has no GPU or driver identity",
            perf.validate_run(document),
        )

    def test_runner_writes_an_evaluated_document(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run.json"
            manifest = Path(directory) / "build.json"
            write_build_manifest(manifest)
            completed = subprocess.run(
                [
                    sys.executable,
                    str(Path(perf.__file__)),
                    "run",
                    "--workload",
                    "python-noop",
                    "--layer",
                    "tooling",
                    "--build-manifest",
                    str(manifest),
                    "--commit",
                    "a" * 40,
                    "--warmup",
                    "0",
                    "--samples",
                    "3",
                    "--output",
                    str(output),
                    "--",
                    sys.executable,
                    "-c",
                    "pass",
                ],
                check=False,
            )
            self.assertEqual(completed.returncode, 0)
            document = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(document["status"], "evaluated")
            self.assertEqual(document["metrics"]["wall_time"]["sample_count"], 3)
            self.assertNotIn("hostname", json.dumps(document).lower())
            self.assertEqual(perf.validate_run(document), [])

    def test_evidence_metric_is_sampled_from_the_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run.json"
            manifest = Path(directory) / "build.json"
            evidence = Path(directory) / "evidence.json"
            write_build_manifest(manifest)
            script = (
                "import json,sys;"
                "json.dump({'water_phase_ms':{'total':{'p95':3.5}}},"
                f"open({str(evidence)!r},'w'))"
            )
            status = perf.main(
                [
                    "run",
                    "--workload",
                    "water-fixture",
                    "--layer",
                    "simulation",
                    "--build-manifest",
                    str(manifest),
                    "--commit",
                    "a" * 40,
                    "--warmup",
                    "0",
                    "--samples",
                    "2",
                    "--evidence",
                    str(evidence),
                    "--evidence-metric",
                    "water_phase_p95_ms",
                    "--output",
                    str(output),
                    "--",
                    sys.executable,
                    "-c",
                    script,
                ]
            )
            self.assertEqual(status, 0)
            document = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(document["status"], "evaluated")
            metric = document["metrics"]["water_phase_p95_ms"]
            self.assertEqual(metric["sample_count"], 2)
            self.assertEqual(metric["p50"], 3.5)
            self.assertEqual(
                document["metric_schema"]["water_phase_p95_ms"],
                {"unit": "ms", "direction": "lower"},
            )
            self.assertEqual(perf.validate_run(document), [])

    def test_missing_evidence_metric_fails_the_run(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run.json"
            manifest = Path(directory) / "build.json"
            evidence = Path(directory) / "evidence.json"
            write_build_manifest(manifest)
            script = f"import json;json.dump({{}},open({str(evidence)!r},'w'))"
            status = perf.main(
                [
                    "run",
                    "--workload",
                    "water-fixture",
                    "--layer",
                    "simulation",
                    "--build-manifest",
                    str(manifest),
                    "--commit",
                    "a" * 40,
                    "--warmup",
                    "0",
                    "--samples",
                    "1",
                    "--evidence",
                    str(evidence),
                    "--evidence-metric",
                    "water_phase_p95_ms",
                    "--output",
                    str(output),
                    "--",
                    sys.executable,
                    "-c",
                    script,
                ]
            )
            self.assertEqual(status, 1)
            document = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(document["status"], "failed")

    def test_gpu_run_without_identity_is_unevaluated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run.json"
            manifest = Path(directory) / "build.json"
            write_build_manifest(manifest)
            status = perf.main(
                [
                    "run",
                    "--workload",
                    "gpu-fixture",
                    "--layer",
                    "rendering-gpu",
                    "--build-manifest",
                    str(manifest),
                    "--commit",
                    "a" * 40,
                    "--samples",
                    "1",
                    "--output",
                    str(output),
                    "--",
                    sys.executable,
                    "-c",
                    "pass",
                ]
            )
            self.assertEqual(status, perf.UNEVALUATED)
            self.assertEqual(json.loads(output.read_text())["status"], "unevaluated")


if __name__ == "__main__":
    unittest.main()
