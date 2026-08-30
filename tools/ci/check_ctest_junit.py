#!/usr/bin/env python3
"""Fail CI when CTest reports an unexpected skipped or disabled test."""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ALLOWED_SKIPS = {
    "AudioBankIntegrity.LoadedBankFilesExistOnDisk",
    "JobSystemPoolTest.DispatchThroughputBenchmark",
}


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
    for case in cases:
        if case.find("skipped") is None:
            continue
        name = case.get("name", "<unnamed>")
        if name not in ALLOWED_SKIPS:
            unexpected.append(name)

    if unexpected:
        print("Unexpected unevaluated tests:", file=sys.stderr)
        for name in sorted(unexpected):
            print(f"  - {name}", file=sys.stderr)
        return 1

    print(f"CTest inventory and skip audit passed ({len(cases)} tests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
