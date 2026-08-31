#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


VALIDATOR = Path(__file__).with_name("validate_performance_measurement.py")
TEST_NAME = "WorldgenPreviewTest.PerFrameRenderProducesComparableObservation"


class PerformanceMeasurementValidatorTest(unittest.TestCase):
    def run_validator(
        self, *, junit_pass: bool = True, absolute_threshold: bool = False
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "report.xml"
            measurement = root / "measurement.json"
            output = root / "result.json"
            failure = "" if junit_pass else '<failure message="measurement failed" />'
            report.write_text(
                f'<testsuite><testcase name="{TEST_NAME}" time="0.1">'
                f"{failure}</testcase></testsuite>",
                encoding="utf-8",
            )
            samples = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
            document = {
                "schema": "luminumbra.performance_measurement.v3",
                "status": "evaluated",
                "enforcement": "observation",
                "test": TEST_NAME,
                "metric": "preview_frame_wall_ms",
                "unit": "ms",
                "direction": "lower",
                "sample_count": len(samples),
                "samples": samples,
                "p50": 4.0,
                "p95": 8.0,
                "p99": 8.0,
                "mad": 2.0,
                "worst": max(samples),
            }
            if absolute_threshold:
                document.update({"threshold": 9.0, "passed": True})
            measurement.write_text(json.dumps(document), encoding="utf-8")
            return subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    str(report),
                    str(measurement),
                    str(output),
                    "--test",
                    TEST_NAME,
                    "--preset",
                    "perf",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

    def test_valid_observation_returns_success(self) -> None:
        completed = self.run_validator()
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_failed_measurement_test_is_rejected(self) -> None:
        self.assertEqual(self.run_validator(junit_pass=False).returncode, 1)

    def test_absolute_machine_threshold_is_rejected(self) -> None:
        self.assertEqual(self.run_validator(absolute_threshold=True).returncode, 1)


if __name__ == "__main__":
    unittest.main()
