#!/usr/bin/env python3
"""Fail CI when CTest reports an unexpected skipped or disabled test."""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


# Empty by design. Every test that used to be allow-listed here now genuinely
# runs: the JobSystem throughput benchmark was re-enabled as a real test, and
# AudioBankIntegrity gained a generated fixture so it no longer skips when the
# external sound banks are absent. A skip reaching this gate is now always a
# problem worth failing the lane over.
ALLOWED_SKIPS: set[str] = set()


def skip_is_allowed(case: ET.Element) -> bool:
    name = case.get("name", "<unnamed>")
    return name in ALLOWED_SKIPS


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("--minimum-tests", type=int, default=1)
    args = parser.parse_args()

    if not args.report.is_file():
        print(f"CTest JUnit report is missing: {args.report}", file=sys.stderr)
        return 2

    root = ET.parse(args.report).getroot()
    cases = list(root.iter("testcase"))
    if len(cases) < args.minimum_tests:
        print(
            f"CTest inventory is incomplete: evaluated {len(cases)} tests, "
            f"requires at least {args.minimum_tests}",
            file=sys.stderr,
        )
        return 1

    unexpected: list[str] = []
    approved_skips = 0
    for case in cases:
        if case.find("skipped") is None:
            continue
        if skip_is_allowed(case):
            approved_skips += 1
            continue
        unexpected.append(case.get("name", "<unnamed>"))

    if unexpected:
        print("Unexpected unevaluated tests:", file=sys.stderr)
        for name in sorted(unexpected):
            print(f"  - {name}", file=sys.stderr)
        return 1

    print(
        f"CTest inventory and skip audit passed ({len(cases)} tests; "
        f"{approved_skips} approved skips)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
