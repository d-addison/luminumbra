#!/usr/bin/env python3
"""Report/capture contracts, including an optional real benchmark integration."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import active_region_scale as scale


def identity():
    return {"revision": "a" * 40, "source_dirty": False, "binary_sha256": "b" * 64,
            "fixture_hash": "c" * 64, "recipe": scale.RECIPE}


def sample(records=8, chunks=4):
    ledger = dict.fromkeys(scale.LEDGER_COUNTS, 0)
    ledger.update(records=records, populated_records=(records + 1) // 2,
                  empty_unowned_records=records // 2, encoded_bytes=1024,
                  record_value_bytes=records * 104, digest="1234567890abcdef", elapsed_ms=1.)
    saved = dict.fromkeys(scale.SAVE_COUNTS, 0)
    saved.update(chunks=chunks, payload_value_bytes=chunks * 19664,
                 ledger_records=records, ledger_encoded_bytes=1024, elapsed_ms=2.)
    return {"schema": "luminumbra.active_region_scale_samples.v1",
            "records_requested": records, "chunks_requested": chunks,
            "scheduler": {phase: copy.deepcopy(ledger) for phase in scale.SCHEDULER},
            "saves": {phase: copy.deepcopy(saved) for phase in scale.SAVES}}


def runs():
    return [{"identity": identity(), "records": size, "repeat": repeat, "sample": sample(size)}
            for size in (8, 32) for repeat in range(2)]


class ActiveRegionScaleReportContract(unittest.TestCase):
    def test_benchmark_capture_is_registered(self):
        root = Path(__file__).resolve().parents[2]
        cmake = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('if(TARGET luminumbra_active_regions_bench)', cmake)
        self.assertIn('add_test(NAME ActiveRegionScaleProducerContract', cmake)
        self.assertIn('"${CMAKE_SOURCE_DIR}/tools/perf/test_active_region_scale.py"\n'
                      '            --binary "$<TARGET_FILE:luminumbra_active_regions_bench>")', cmake)
        tools = (root / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('if(LUMINUMBRA_BUILD_TOOLS OR BUILD_TESTING)\n'
                      '    add_executable(luminumbra_active_regions_bench active_regions_bench.cpp)', tools)

    def report(self, rows):
        return scale.build_report(rows, [8, 32], 4, 2)

    def test_complete_report_is_order_independent_and_preserves_counts(self):
        rows = runs()
        rows[1]["sample"]["scheduler"]["cold"]["elapsed_ms"] = 3.
        rows[1]["sample"]["saves"]["sparse_edit"]["clean_siblings_serialized"] = 1
        report = self.report(rows)
        self.assertEqual(report, self.report(list(reversed(rows))))
        self.assertEqual(len(report["summaries"]), 18)
        self.assertEqual(report["summaries"][0]["elapsed_ms"],
                         {"p50": 2., "p95": 2.9, "p99": 2.98, "maximum": 3.})
        self.assertEqual(report["summaries"][0]["observations"]["records"], [8, 8])
        self.assertEqual(report["summaries"][8]["observations"]["clean_siblings_serialized"], [0, 1])
        json.dumps(report, allow_nan=False)

    def test_identity_mismatch_refuses_every_identity_field(self):
        for key in scale.IDENTITY_KEYS:
            rows = runs()
            replacement = {"revision": "d" * 40, "source_dirty": True,
                           "binary_sha256": "d" * 64, "fixture_hash": "d" * 64,
                           "recipe": "unsupported"}[key]
            rows[-1]["identity"][key] = replacement
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "identity"):
                self.report(rows)

    def test_missing_and_duplicate_samples_refuse(self):
        rows = runs()
        for invalid in ([], rows[:-1], rows + rows[:1], rows[:3] + rows[:1]):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                self.report(invalid)
        for group, phases in (("scheduler", scale.SCHEDULER), ("saves", scale.SAVES)):
            for phase in phases:
                rows = runs()
                del rows[0]["sample"][group][phase]
                with self.subTest(phase=phase), self.assertRaisesRegex(ValueError, "sample"):
                    self.report(rows)

    def test_scheduler_counts_and_digests_must_reproduce_but_times_are_observational(self):
        for key in (*scale.LEDGER_COUNTS, "digest"):
            rows = runs()
            row = rows[1]["sample"]["scheduler"]["cold"]
            row[key] = "a" * 16 if key == "digest" else row[key] + 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.report(rows)

    def test_malformed_inputs_refuse(self):
        mutations = (("elapsed_ms", True), ("elapsed_ms", float("nan")),
                     ("elapsed_ms", float("inf")), ("elapsed_ms", -1),
                     ("elapsed_ms", 10**1000), ("records", False), ("records", -1),
                     ("records", 2**64), ("digest", "wrong"), ("populated_records", 0))
        for key, value in mutations:
            rows = runs()
            rows[0]["sample"]["scheduler"]["warm"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.report(rows)
        for invalid in (None, {}, [], True):
            rows = runs()
            rows[0]["sample"] = invalid
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                self.report(rows)
        rows = runs()
        rows[0]["sample"]["saves"]["first"]["chunks"] = 0
        with self.assertRaisesRegex(ValueError, "payload"):
            self.report(rows)
        for key in scale.IDENTITY_KEYS:
            rows = runs()
            del rows[0]["identity"][key]
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "identity"):
                self.report(rows)

    def test_invalid_matrix_refuses_before_creating_evidence(self):
        matrices = (([], 4, 2), ([0], 4, 2), ([True], 4, 2),
                    ([scale.MAX_RECORDS + 1], 4, 2), ([8, 8], 4, 2),
                    ([8], 0, 2), ([8], scale.MAX_CHUNKS + 1, 2),
                    ([8], 4, 0), ([8], 4, 33), (list(range(1, 17)), 4, 32))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            for sizes, chunks, repeats in matrices:
                with self.subTest(sizes=sizes, chunks=chunks, repeats=repeats), \
                        mock.patch.object(scale, "measurement_identity") as measure, \
                        self.assertRaisesRegex(ValueError, "matrix"):
                    scale.capture(path / "binary", path, path / "evidence", sizes, chunks, repeats)
                measure.assert_not_called()
                self.assertFalse((path / "evidence").exists())

    def test_capture_commands_and_final_identity_check(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = root / "evidence"
            completed = SimpleNamespace(stdout=json.dumps(sample()) + "\r\n")
            with mock.patch.object(scale, "measurement_identity", return_value=identity()), \
                    mock.patch.object(scale.subprocess, "run", return_value=completed) as launch:
                report = scale.capture(root / "bench", root / "source", evidence, [8], 4, 2)
            self.assertEqual(launch.call_count, 2)
            self.assertEqual(launch.call_args.args[0],
                             [str(root / "bench"), "--root", str(root / "source"), "--work-dir",
                              str(evidence / "fixture-8-1"), "--records", "8", "--chunks", "4"])
            self.assertTrue(launch.call_args.kwargs["check"])
            self.assertEqual(json.loads((evidence / "report.json").read_text()), report)
            self.assertEqual(len(list(evidence.glob("sample-*.json"))), 2)
            with mock.patch.object(scale, "measurement_identity", return_value=identity()), \
                    mock.patch.object(scale.subprocess, "run") as launch, \
                    self.assertRaises(FileExistsError):
                scale.capture(root / "bench", root / "source", evidence, [8], 4, 2)
            launch.assert_not_called()

    def test_failed_partial_and_changed_capture_never_publishes_report(self):
        failures = (subprocess.CalledProcessError(1, ["bench"]),
                    SimpleNamespace(stdout="{"), SimpleNamespace(stdout="x" * 65537),
                    SimpleNamespace(stdout='{"schema":1,"schema":2}'),
                    SimpleNamespace(stdout=json.dumps({})),
                    SimpleNamespace(stdout=json.dumps(sample())))
        for failure in failures:
            with self.subTest(failure=type(failure)), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                changed = identity()
                changed["fixture_hash"] = "d" * 64
                result = {"side_effect": failure} if isinstance(failure, Exception) else {
                    "return_value": failure}
                with mock.patch.object(scale, "measurement_identity", side_effect=[identity(), changed]), \
                        mock.patch.object(scale.subprocess, "run", **result), \
                        self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    scale.capture(root / "bench", root / "source", root / "evidence", [8], 4, 1)
                self.assertFalse((root / "evidence/report.json").exists())

    def test_evidence_inside_source_tree_refuses_before_identity_or_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with mock.patch.object(scale, "measurement_identity") as measure, \
                    mock.patch.object(scale.subprocess, "run") as launch:
                for evidence in (root, root / "evidence", root / "dir/../evidence"):
                    with self.subTest(evidence=evidence), self.assertRaisesRegex(ValueError, "outside"):
                        scale.capture(root / "bench", root, evidence, [8], 4, 1)
            measure.assert_not_called()
            launch.assert_not_called()
            self.assertFalse((root / "evidence").exists())

    def test_partial_report_write_and_failed_rename_never_publish_success(self):
        original_dump = json.dump
        for stage in ("write", "rename"):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                def dump(data, output, **options):
                    if Path(output.name).name == ".report.pending" and stage == "write":
                        output.write("{")
                        raise OSError("synthetic partial write")
                    original_dump(data, output, **options)
                completed = SimpleNamespace(stdout=json.dumps(sample()))
                with mock.patch.object(scale, "measurement_identity", return_value=identity()), \
                        mock.patch.object(scale.subprocess, "run", return_value=completed), \
                        mock.patch.object(scale.json, "dump", side_effect=dump), \
                        mock.patch.object(Path, "replace", side_effect=OSError("synthetic rename failure")), \
                        self.assertRaises(OSError):
                    scale.capture(root / "bench", root / "source", root / "evidence", [8], 4, 1)
                self.assertTrue((root / "evidence/.report.pending").exists())
                self.assertFalse((root / "evidence/report.json").exists())

    def test_binary_hash_reads_regular_file_and_refuses_missing_or_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "binary"
            binary.write_bytes(b"abc")
            self.assertEqual(scale.file_digest(binary),
                             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
            for invalid in (root / "missing", root):
                with self.subTest(invalid=invalid), self.assertRaises((OSError, ValueError)):
                    scale.file_digest(invalid)


def benchmark_capture(binary):
    """Cross the real producer boundary and retain every fixed phase through capture."""
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory() as temporary:
        evidence = Path(temporary) / "evidence"
        report = scale.capture(binary.resolve(), root, evidence, [1, 8], 1, 2)
        case = unittest.TestCase()
        case.assertEqual(json.loads((evidence / "report.json").read_text(encoding="utf-8")), report)
        case.assertEqual(len(report["summaries"]), 2 * (len(scale.SCHEDULER) + len(scale.SAVES)))
        for records in (1, 8):
            for repeat in range(2):
                run = json.loads((evidence / f"sample-{records}-{repeat}.json").read_text(encoding="utf-8"))
                scale.validate_sample(run["sample"], records, 1)
                case.assertEqual(run["identity"], report["identity"])
                case.assertTrue((evidence / f"fixture-{records}-{repeat}").is_dir())


if __name__ == "__main__":
    if "--binary" in sys.argv[1:]:
        parser = argparse.ArgumentParser(description=benchmark_capture.__doc__)
        parser.add_argument("--binary", type=Path, required=True)
        benchmark_capture(parser.parse_args().binary)
    else:
        unittest.main()
