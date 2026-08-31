#!/usr/bin/env python3
"""Validate performance JUnit and raw samples, then emit a comparable CI result."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def fail(message: str) -> int:
    print(f"Performance measurement is invalid: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("measurement", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--test", required=True)
    parser.add_argument("--preset", default="release")
    args = parser.parse_args()

    if not args.report.is_file() or not args.measurement.is_file():
        return fail("JUnit or raw measurement evidence is missing")

    try:
        root = ET.parse(args.report).getroot()
        measurement = json.loads(args.measurement.read_text(encoding="utf-8"))
    except (ET.ParseError, OSError, ValueError) as exc:
        return fail(f"evidence cannot be parsed: {exc}")

    cases = [case for case in root.iter("testcase") if case.get("name") == args.test]
    if len(cases) != 1:
        return fail(f"expected one {args.test!r} result, found {len(cases)}")
    case = cases[0]
    if case.find("skipped") is not None:
        return fail("the benchmark was skipped")

    if measurement.get("schema") != "luminumbra.performance_measurement.v3":
        return fail("the raw measurement schema is not supported")
    if measurement.get("status") != "evaluated" or measurement.get("test") != args.test:
        return fail("the raw measurement does not describe the evaluated test")
    if (
        measurement.get("enforcement") != "observation"
        or measurement.get("metric") != "preview_frame_wall_ms"
        or measurement.get("unit") != "ms"
        or measurement.get("direction") != "lower"
    ):
        return fail("the raw measurement metric schema is invalid")
    if "threshold" in measurement or "passed" in measurement:
        return fail("absolute machine thresholds are not permitted")

    samples = measurement.get("samples")
    if not isinstance(samples, list) or len(samples) < 8:
        return fail("at least eight raw samples are required")
    if any(
        not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0
        for value in samples
    ):
        return fail("raw samples contain an invalid value")
    if measurement.get("sample_count") != len(samples):
        return fail("sample count does not match the raw samples")

    worst = measurement.get("worst")
    if not isinstance(worst, (int, float)) or not math.isfinite(worst):
        return fail("worst is absent")
    if not math.isclose(worst, max(samples), rel_tol=1e-12, abs_tol=1e-12):
        return fail("worst does not match the raw samples")

    ordered = sorted(float(value) for value in samples)
    def percentile(fraction: float) -> float:
        rank = math.ceil(fraction * len(ordered))
        return ordered[min(len(ordered) - 1, max(1, rank) - 1)]

    expected = {name: percentile(fraction) for name, fraction in (
        ("p50", 0.50), ("p95", 0.95), ("p99", 0.99)
    )}
    deviations = sorted(abs(value - expected["p50"]) for value in ordered)
    expected["mad"] = deviations[(len(deviations) - 1) // 2]
    for name, value in expected.items():
        actual = measurement.get(name)
        if not isinstance(actual, (int, float)) or not math.isclose(
            actual, value, rel_tol=1e-12, abs_tol=1e-12
        ):
            return fail(f"{name} does not match the raw samples")

    junit_pass = case.find("failure") is None and case.find("error") is None
    if not junit_pass:
        return fail("the measurement test did not complete successfully")

    result = {
        "schema": "luminumbra.performance_result.v3",
        "status": "evaluated",
        "verdict": "observation",
        "enforcement": "observation",
        "measurement": measurement,
        "duration_seconds": float(case.get("time", "0")),
        "comparability": {
            "runner_os": os.environ.get("RUNNER_OS", "local"),
            "runner_arch": os.environ.get("RUNNER_ARCH", "unknown"),
            "runner_image": os.environ.get("ImageOS", "unknown"),
            "commit": os.environ.get("GITHUB_SHA", "local"),
            "graphics": "software-opengl",
            "build_preset": args.preset,
        },
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Performance observation validated: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
