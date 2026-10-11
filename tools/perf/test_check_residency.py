#!/usr/bin/env python3
"""Residency checker contract tests; runs the CLI in a subprocess from any working directory."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "check_residency.py"
sys.path.insert(0, str(SCRIPT.parent))

PROFILE_SCHEMA = "luminumbra.sim_budget_profile.v1"


def row(cls: str, **overrides: object) -> dict:
    data = {"class": cls, "resident_bytes": 50, "peak_bytes": 50, "budget_bytes": 100,
            "count": 1}
    data.update(overrides)
    return data


class ResidencyCheckerTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.dir = Path(self._temp.name)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write(self, name: str, text: str) -> Path:
        path = self.dir / name
        path.write_text(text, encoding="utf-8")
        return path

    def profile(self, residency: dict | None = None, raw: str | None = None) -> Path:
        if raw is not None:
            return self.write("profile.json", raw)
        if residency is None:
            residency = {"a": 100}
        body = {"schema": PROFILE_SCHEMA, "residency": residency}
        return self.write("profile.json", json.dumps(body))

    def artifact(self, rows: object = None, raw: str | None = None) -> Path:
        if raw is not None:
            return self.write("artifact.json", raw)
        body: dict = {"schema": "luminumbra.server_tick.v1"}
        if rows is not None:
            body["residency"] = rows
        return self.write("artifact.json", json.dumps(body))

    def run_cli(self, profile: Path, artifact: Path, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--artifact", str(artifact), "--profile", str(profile),
             *extra],
            capture_output=True, text=True, cwd=str(self.dir), check=False)

    def verdict(self, result: subprocess.CompletedProcess) -> dict:
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout)

    def assert_invalid(self, profile: Path, artifact: Path) -> None:
        result = self.run_cli(profile, artifact)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertTrue(result.stderr.startswith("check_residency: "))

    def test_pass(self) -> None:
        result = self.run_cli(self.profile(), self.artifact([row("a")]))
        self.assertEqual(result.returncode, 0, result.stderr)
        verdict = self.verdict(result)
        self.assertEqual(verdict["schema"], "luminumbra.residency_verdict.v1")
        self.assertEqual(verdict["classes"][0]["verdict"], "pass")
        self.assertEqual(verdict["summary"], {"pass": 1, "fail": 0, "unevaluated": 0})

    def test_over_cap_fails(self) -> None:
        result = self.run_cli(self.profile(), self.artifact([row("a", peak_bytes=101,
                                                              resident_bytes=101)]))
        self.assertEqual(result.returncode, 1, result.stderr)
        entry = self.verdict(result)["classes"][0]
        self.assertEqual((entry["verdict"], entry["reason"]), ("fail", "over-cap"))

    def test_budget_mismatch_fails(self) -> None:
        result = self.run_cli(self.profile(), self.artifact([row("a", budget_bytes=99)]))
        self.assertEqual(result.returncode, 1, result.stderr)
        entry = self.verdict(result)["classes"][0]
        self.assertEqual((entry["verdict"], entry["reason"]), ("fail", "budget_mismatch"))

    def test_budget_equal_to_cap_passes(self) -> None:
        result = self.run_cli(self.profile(), self.artifact([row("a", budget_bytes=100)]))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.verdict(result)["classes"][0]["verdict"], "pass")

    def test_null_budget_is_budget_mismatch(self) -> None:
        result = self.run_cli(self.profile(), self.artifact([row("a", budget_bytes=None)]))
        self.assertEqual(result.returncode, 1, result.stderr)
        entry = self.verdict(result)["classes"][0]
        self.assertEqual((entry["verdict"], entry["reason"]), ("fail", "budget_mismatch"))

    def test_misspelt_profile_key_invalid(self) -> None:
        self.assert_invalid(self.profile(raw=(
            '{"schema": "%s", "residncy": {"a": 1}}' % PROFILE_SCHEMA)), self.artifact([row("a")]))

    def test_unknown_region_scheduler_key_invalid(self) -> None:
        self.assert_invalid(self.profile(raw=(
            '{"schema": "%s", "region_scheduler": {"speed": 1}}' % PROFILE_SCHEMA)),
            self.artifact([row("a")]))

    def test_bad_scheduler_ranges_invalid(self) -> None:
        for body in ('"hold_ticks": 0', '"hold_ticks": 4294967296', '"reduced_shift": 0',
                     '"reduced_shift": 17', '"work_limit": -1', '"hold_ticks": true'):
            with self.subTest(body=body):
                self.assert_invalid(self.profile(raw=(
                    '{"schema": "%s", "region_scheduler": {%s}}' % (PROFILE_SCHEMA, body))),
                    self.artifact([row("a")]))

    def test_bad_profile_name_invalid(self) -> None:
        self.assert_invalid(self.profile({"Upper": 1}), self.artifact([row("Upper")]))

    def test_too_many_residency_classes_invalid(self) -> None:
        caps = {f"c{i}": 1 for i in range(33)}
        self.assert_invalid(self.profile(caps), self.artifact([row("c0")]))

    def test_oversize_profile_invalid(self) -> None:
        head = '{"schema": "%s", "residency": {}}' % PROFILE_SCHEMA
        raw = head + " " * (64 * 1024 + 1 - len(head))
        self.assert_invalid(self.profile(raw=raw), self.artifact([row("a")]))

    def test_count_above_uint64_invalid(self) -> None:
        self.assert_invalid(self.profile(), self.artifact([row("a", count=2**64)]))

    def test_max_uint64_count_accepted(self) -> None:
        result = self.run_cli(self.profile(), self.artifact([row("a", count=2**64 - 1)]))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_bytes_cpu_checked_without_gpu(self) -> None:
        self.assert_invalid(self.profile(), self.artifact([row("a", bytes_cpu="bad")]))

    def test_missing_row_is_unevaluated(self) -> None:
        result = self.run_cli(self.profile({"a": 100, "b": 5}), self.artifact([row("a")]))
        self.assertEqual(result.returncode, 3, result.stderr)
        verdict = self.verdict(result)
        entry = verdict["classes"][1]
        self.assertEqual((entry["class"], entry["verdict"], entry["reason"]),
                         ("b", "unevaluated", "not-instrumented"))
        self.assertEqual(verdict["summary"]["unevaluated"], 1)

    def test_fail_beats_unevaluated(self) -> None:
        result = self.run_cli(self.profile({"a": 10, "b": 5}),
                              self.artifact([row("a", peak_bytes=20, resident_bytes=20)]))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.verdict(result)["summary"],
                         {"pass": 0, "fail": 1, "unevaluated": 1})

    def test_absent_residency_array_leaves_all_unevaluated(self) -> None:
        result = self.run_cli(self.profile({"a": 100}), self.artifact())
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(self.verdict(result)["classes"][0]["verdict"], "unevaluated")

    def test_non_list_residency_leaves_all_unevaluated(self) -> None:
        result = self.run_cli(self.profile({"a": 100}), self.artifact({"a": row("a")}))
        self.assertEqual(result.returncode, 3, result.stderr)

    def test_duplicate_rows_invalid(self) -> None:
        self.assert_invalid(self.profile(), self.artifact([row("a"), row("a")]))

    def test_cpu_gpu_sum_mismatch_invalid(self) -> None:
        self.assert_invalid(self.profile(), self.artifact(
            [row("a", bytes_cpu=20, bytes_gpu=20)]))

    def test_cpu_gpu_sum_match_accepted(self) -> None:
        result = self.run_cli(self.profile(), self.artifact(
            [row("a", bytes_cpu=20, bytes_gpu=30, resident_bytes=50)]))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_peak_below_resident_invalid(self) -> None:
        self.assert_invalid(self.profile(), self.artifact(
            [row("a", resident_bytes=60, peak_bytes=50)]))

    def test_duplicate_json_key_invalid_in_artifact(self) -> None:
        self.assert_invalid(self.profile(), self.artifact(raw=(
            '{"residency": [], "residency": []}')))

    def test_duplicate_json_key_invalid_in_profile(self) -> None:
        raw = '{"schema": "%s", "schema": "%s", "residency": {}}' % (PROFILE_SCHEMA, PROFILE_SCHEMA)
        self.assert_invalid(self.profile(raw=raw), self.artifact([row("a")]))

    def test_non_finite_constant_invalid(self) -> None:
        self.assert_invalid(self.profile(), self.artifact(raw='{"residency": [NaN]}'))

    def test_bool_is_not_an_integer(self) -> None:
        self.assert_invalid(self.profile(), self.artifact([row("a", resident_bytes=True)]))
        self.assert_invalid(self.profile({"a": True}), self.artifact([row("a")]))

    def test_unknown_class_listed_not_failing(self) -> None:
        extra = row("x", peak_bytes=900, resident_bytes=900)
        result = self.run_cli(self.profile(), self.artifact([row("a"), extra]))
        self.assertEqual(result.returncode, 0, result.stderr)
        verdict = self.verdict(result)
        self.assertEqual(verdict["unknown_classes"], ["x"])
        self.assertEqual(verdict["summary"]["fail"], 0)

    def test_verdict_out_is_written(self) -> None:
        out = self.dir / "verdict.json"
        result = self.run_cli(self.profile(), self.artifact([row("a")]), "--verdict-out", str(out))
        self.assertEqual(result.returncode, 0, result.stderr)
        written = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(written, json.loads(result.stdout))
        self.assertEqual(sorted(p.name for p in self.dir.iterdir() if p.name.endswith(".tmp")), [])

    def test_missing_file_is_invalid_input(self) -> None:
        self.assert_invalid(self.dir / "missing.json", self.artifact([row("a")]))

    def test_disallowed_abbreviation_is_usage_error(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--art", "x", "--profile", "y"],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
