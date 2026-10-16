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
from unittest import mock

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


def far_capture():
    rows = [dict(frame=frame, requested_mode="ladder", effective_mode="ladder", refusal="None",
                 submission_refusal="None", suppressed=False, coverage_settled=True,
                 content_identity="18446744073709551615",
                 streaming=dict(generation_pending=0, meshing_pending=0, terrain_uploads_pending=0,
                                generation_outstanding=False, meshing_outstanding=False),
                 settings=dict(gns_compiled=True, active_regions=True,
                               water_cells_per_side=16, grass_density_scale=1.6))
            for frame in range(2, 6)]
    data = dict(schema="luminumbra.render_benchmark.v3", frames=4, warmup_frames=2,
                far_world=dict(frames=rows))
    update_far_summary(data)
    return data


def update_far_summary(data):
    rows = data['far_world']['frames']
    summary = {key: {} for key in ('requested_mode', 'effective_mode', 'refusal', 'submission_refusal')}
    for row in rows:
        for key in summary:
            summary[key][row[key]] = summary[key].get(row[key], 0) + 1
    summary['suppressed_frames'] = sum(row['suppressed'] for row in rows)
    summary['unsettled_frames'] = sum(not row['coverage_settled'] or any(row['streaming'].values()) for row in rows)
    data['far_world']['summary'] = summary


class FarModeReportingTest(unittest.TestCase):
    def test_ladder_positive_and_rejection_controls(self):
        valid = far_capture()
        perf.validate_far_world_reporting(valid, ladder_baseline=True)
        controls = [dict(streaming=dict(generation_pending=1, meshing_pending=0, terrain_uploads_pending=0)),
                    dict(streaming=dict(generation_pending=0, meshing_pending=1, terrain_uploads_pending=0)),
                    dict(streaming=dict(generation_pending=0, meshing_pending=0, terrain_uploads_pending=1)),
                    dict(effective_mode='legacy'), dict(requested_mode='legacy'),
                    dict(refusal='ImmutableAuthorityUnavailable'), dict(suppressed=True),
                    dict(suppressed=True, submission_refusal='LiveOwnershipIncomplete'),
                    dict(suppressed=True, submission_refusal='LogicalStoreAdmission'),
                    dict(suppressed=True, submission_refusal='BackendUnavailable'),
                    dict(suppressed=True, submission_refusal='DrawRejected'),
                    dict(suppressed=True, submission_refusal='IsolationSuppressed'),
                    dict(streaming=dict(generation_outstanding=True)),
                    dict(streaming=dict(meshing_outstanding=True)),
                    dict(coverage_settled=False)]
        for index in range(4):
            for mutation in controls:
                with self.subTest(index=index, mutation=mutation):
                    data = copy.deepcopy(valid)
                    row = data['far_world']['frames'][index]
                    if 'streaming' in mutation:
                        row['streaming'].update(mutation['streaming'])
                    else:
                        row.update(mutation)
                    update_far_summary(data)
                    # A diagnostic capture may faithfully report a refused frame.
                    perf.validate_far_world_reporting(data)
                    with self.assertRaisesRegex(ValueError, 'Ladder baseline refused'):
                        perf.validate_far_world_reporting(data, ladder_baseline=True)
                    data['far_world']['frames'][index] = copy.deepcopy(valid['far_world']['frames'][index])
                    update_far_summary(data)
                    perf.validate_far_world_reporting(data, ladder_baseline=True)

    def test_missing_malformed_and_forged_evidence(self):
        controls = [lambda d: d['far_world']['frames'][0].pop('streaming'),
                    lambda d: d['far_world']['frames'][0]['streaming'].update(generation_pending=False),
                    lambda d: d['far_world']['frames'][0]['streaming'].pop('generation_outstanding'),
                    lambda d: d['far_world']['frames'][0]['streaming'].pop('meshing_outstanding'),
                    lambda d: d['far_world']['frames'][0]['streaming'].update(generation_outstanding=1),
                    lambda d: d['far_world']['frames'][0]['streaming'].update(meshing_outstanding=0),
                    lambda d: d['far_world']['frames'][0]['settings'].pop('grass_density_scale'),
                    lambda d: d.pop('far_world'),
                    lambda d: d['far_world']['frames'].pop(),
                    lambda d: d['far_world']['frames'][2].update(frame=99),
                    lambda d: d['far_world']['frames'][0].update(coverage_settled=1),
                    lambda d: d['far_world']['frames'][0].update(suppressed=0),
                    lambda d: d['far_world']['frames'][0].update(effective_mode='Ladder'),
                    lambda d: d['far_world']['frames'][0].update(refusal=[]),
                    lambda d: d['far_world']['frames'][0].update(submission_refusal='DrawRejected'),
                    lambda d: d['far_world']['frames'][0].update(content_identity='18446744073709551616'),
                    lambda d: d['far_world']['frames'][0]['settings'].update(gns_compiled=1),
                    lambda d: d['far_world']['frames'][0]['settings'].update(water_cells_per_side=0),
                    lambda d: d['far_world']['frames'][0]['settings'].update(grass_density_scale=float('nan')),
                    lambda d: d['far_world']['summary']['effective_mode'].update(ladder=3),
                    lambda d: d['far_world']['summary'].update(suppressed_frames=False)]
        for mutation in controls:
            data = far_capture()
            mutation(data)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                perf.validate_far_world_reporting(data, ladder_baseline=True)
        perf.validate_far_world_reporting({'frames': 4})

    def test_perf_ladder_cli_rejection_and_repair(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.json'
            # Isolated Python needs an explicit trusted local module path.
            bootstrap = ("import runpy,sys; sys.path.insert(0,sys.argv[1]); "
                         "sys.argv=sys.argv[2:]; runpy.run_path(sys.argv[0],run_name='__main__')")
            command = [sys.executable, '-I', '-B', '-c', bootstrap,
                       str(Path(perf.__file__).parent), str(Path(perf.__file__)),
                       'validate', str(path), '--ladder-baseline']
            for accepted in (False, True):
                data = far_capture()
                data['far_world']['frames'][1]['coverage_settled'] = accepted
                update_far_summary(data)
                path.write_text(json.dumps(data), encoding='utf-8')
                result = subprocess.run(command, capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 0 if accepted else 1, result.stderr)


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


def rebaseline_document(base: dict, candidate: dict, reason: str = "Fixture data changed") -> dict:
    return {
        "schema": perf.REBASELINE_SCHEMA,
        "retired_comparability_key": base["comparability_key"],
        "replacement_comparability_key": candidate["comparability_key"],
        "reason": reason,
    }


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

    def test_matching_rebaseline_waives_comparability_mismatch(self) -> None:
        base = run_document([10.0] * 20)
        candidate = run_document([10.0] * 20)
        candidate["workload"]["fixture_hash"] = "e" * 40
        candidate["comparability_key"] = perf.stable_hash(
            perf.canonical_identity(candidate)
        )

        comparison = perf.compare_runs(
            base,
            candidate,
            5.0,
            rebaseline=rebaseline_document(base, candidate, "New world fixture data"),
        )

        self.assertEqual(comparison["status"], "evaluated")
        self.assertEqual(comparison["verdict"], "rebaseline")
        self.assertTrue(comparison["rebaseline"]["applied"])
        self.assertEqual(comparison["rebaseline"]["reason"], "New world fixture data")
        self.assertIn("explicitly re-baselined", comparison["reasons"][0])

    def test_nonmatching_rebaseline_does_not_waive_mismatch(self) -> None:
        base = run_document([10.0] * 20)
        candidate = run_document([10.0] * 20)
        candidate["workload"]["fixture_hash"] = "e" * 40
        candidate["comparability_key"] = perf.stable_hash(
            perf.canonical_identity(candidate)
        )
        declaration = rebaseline_document(base, candidate)
        declaration["retired_comparability_key"] = "0" * 64

        comparison = perf.compare_runs(
            base, candidate, 5.0, rebaseline=declaration
        )

        self.assertEqual(comparison["status"], "unevaluated")
        self.assertEqual(comparison["reasons"], ["comparability keys differ"])
        self.assertFalse(comparison["rebaseline"]["applied"])
        self.assertIn("does not match", comparison["rebaseline"]["warning"])

    def test_rebaseline_declaration_cannot_waive_a_regression(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base_document = run_document(
                [10.0 + (index % 3) * 0.01 for index in range(20)]
            )
            candidate_document = run_document(
                [12.0 + (index % 3) * 0.01 for index in range(20)]
            )
            base = Path(directory) / "base.json"
            candidate = Path(directory) / "candidate.json"
            declaration = Path(directory) / "rebaseline.json"
            base.write_text(json.dumps(base_document), encoding="utf-8")
            candidate.write_text(json.dumps(candidate_document), encoding="utf-8")
            declaration.write_text(
                json.dumps(
                    {
                        "schema": perf.REBASELINE_SCHEMA,
                        "retired_comparability_key": "0" * 64,
                        "replacement_comparability_key": "1" * 64,
                        "reason": "A prior fixture transition",
                    }
                ),
                encoding="utf-8",
            )

            with mock.patch.object(perf, "REBASELINE_PATH", declaration):
                status = perf.main(
                    ["compare", "--base", str(base), "--candidate", str(candidate)]
                )

        self.assertEqual(status, 1)

    def test_malformed_rebaseline_declaration_is_an_error(self) -> None:
        with self.assertRaisesRegex(ValueError, "re-baseline declaration fields"):
            perf.validate_rebaseline(
                {
                    "schema": perf.REBASELINE_SCHEMA,
                    "retired_comparability_key": "0" * 64,
                    "reason": "Missing replacement key",
                }
            )

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
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "unevaluated")


if __name__ == "__main__":
    unittest.main()
