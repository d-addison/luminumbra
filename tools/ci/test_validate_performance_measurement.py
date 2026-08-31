#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


VALIDATOR = Path(__file__).with_name("validate_performance_measurement.py")
TEST_NAME = "WorldgenPreviewTest.PerFrameRenderHoldsPreviewBudget"


class PerformanceMeasurementValidatorTest(unittest.TestCase):
    def run_validator(self, measured_pass: bool) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "report.xml"
            measurement = root / "measurement.json"
            output = root / "result.json"
            failure = "" if measured_pass else '<failure message="budget exceeded" />'
            report.write_text(
                f'<testsuite><testcase name="{TEST_NAME}" time="0.1">'
                f"{failure}</testcase></testsuite>",
                encoding="utf-8",
            )
            samples = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
            measurement.write_text(
                json.dumps(
                    {
                        "schema": "luminumbra.performance_measurement.v2",
                        "status": "evaluated",
                        "test": TEST_NAME,
                        "metric": "preview_frame_wall_ms",
                        "unit": "ms",
                        "sample_count": len(samples),
                        "samples": samples,
                        "p50": 4.0,
                        "p95": 8.0,
                        "p99": 8.0,
                        "mad": 2.0,
                        "worst": max(samples),
                        "threshold": 9.0 if measured_pass else 8.0,
                        "passed": measured_pass,
                    }
                ),
                encoding="utf-8",
            )
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

    def test_passing_measurement_returns_success(self) -> None:
        self.assertEqual(self.run_validator(True).returncode, 0)

    def test_regression_returns_failure(self) -> None:
        self.assertEqual(self.run_validator(False).returncode, 1)


if __name__ == "__main__":
    unittest.main()
