#!/usr/bin/env python3
"""Fail CI when CTest reports an unexpected skipped or disabled test."""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ALLOWED_SKIPS = {
    "AudioBankIntegrity.LoadedBankFilesExistOnDisk",
}


def skip_reasons(case: ET.Element) -> list[str]:
    """Return the explicit GoogleTest reasons recorded for a skipped case."""
    output = case.findtext("system-out") or ""
    lines = output.splitlines()
    reasons: list[str] = []
    for index, line in enumerate(lines[:-1]):
        if line.rstrip().endswith(": Skipped"):
            reason = lines[index + 1].strip()
            if reason:
                reasons.append(reason)
    return reasons


def skip_is_allowed(
    case: ET.Element, allowed_reason_patterns: list[re.Pattern[str]]
) -> bool:
    name = case.get("name", "<unnamed>")
    if name in ALLOWED_SKIPS:
        return True

    reasons = skip_reasons(case)
    return bool(reasons) and all(
        any(pattern.fullmatch(reason) for pattern in allowed_reason_patterns)
        for reason in reasons
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("--minimum-tests", type=int, default=1)
    parser.add_argument(
        "--allow-skip-reason-regex",
        action="append",
        default=[],
        metavar="REGEX",
        help=(
            "accept a skipped test only when every explicit GoogleTest skip reason "
            "fully matches REGEX; may be repeated"
        ),
    )
    args = parser.parse_args()

    try:
        allowed_reason_patterns = [
            re.compile(pattern) for pattern in args.allow_skip_reason_regex
        ]
    except re.error as error:
        parser.error(f"invalid skip-reason regex: {error}")

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
        if skip_is_allowed(case, allowed_reason_patterns):
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
