#!/usr/bin/env python3
"""Fail CI when CTest reports an unexpected skipped or disabled test."""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ALLOWED_SKIPS = {"JobSystemPoolTest.DispatchThroughputBenchmark"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    args = parser.parse_args()

    if not args.report.is_file():
        print(f"CTest JUnit report is missing: {args.report}", file=sys.stderr)
        return 2

    root = ET.parse(args.report).getroot()
    unexpected: list[str] = []
    for case in root.iter("testcase"):
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

    print("CTest skip audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
