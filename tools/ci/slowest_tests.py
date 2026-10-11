#!/usr/bin/env python3
"""Report the slowest CTest cases from a JUnit report.

Designed for CI steps that run under ``if: always()``: it never fails the lane.
A missing or unparseable report is reported on stderr and the script exits 0.
"""

from __future__ import annotations

import argparse
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import List, NamedTuple, Optional


class TestTiming(NamedTuple):
    name: str
    seconds: float


class Summary(NamedTuple):
    timings: List[TestTiming]
    skipped: int
    failed: int


def _seconds(raw: Optional[str]) -> float:
    if raw is None:
        return 0.0
    try:
        value = float(raw)
    except ValueError:
        return 0.0
    if not math.isfinite(value) or value < 0.0:
        return 0.0
    return value


def parse_report(path: Path) -> Summary:
    root = ET.parse(path).getroot()
    timings: List[TestTiming] = []
    skipped = 0
    failed = 0
    for case in root.iter("testcase"):
        timings.append(TestTiming(case.get("name", "<unnamed>"), _seconds(case.get("time"))))
        if case.find("skipped") is not None:
            skipped += 1
        if case.find("failure") is not None or case.find("error") is not None:
            failed += 1
    # Slowest first; name breaks ties so the output is deterministic.
    timings.sort(key=lambda t: (-t.seconds, t.name))
    return Summary(timings, skipped, failed)


def _escape_cell(text: str) -> str:
    return text.replace("\\", "\\\\").replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def render_markdown(summary: Summary, count: int) -> str:
    shown = summary.timings[:count]
    total_seconds = sum(t.seconds for t in summary.timings)
    lines = [
        f"### Slowest {len(shown)} tests",
        "",
        "| Rank | Seconds | Test |",
        "| ---: | ---: | --- |",
    ]
    for rank, timing in enumerate(shown, start=1):
        lines.append(f"| {rank} | {timing.seconds:.1f} | {_escape_cell(timing.name)} |")
    lines.append("")
    lines.append(
        f"Total: {len(summary.timings)} tests, {total_seconds:.1f} s; "
        f"{summary.skipped} skipped, {summary.failed} failed"
    )
    return "\n".join(lines) + "\n"


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("report", type=Path)
    parser.add_argument("--count", type=int, default=15)
    parser.add_argument("--summary-file", type=Path, default=None)
    args = parser.parse_args(argv)

    if not args.report.is_file():
        print(f"slowest_tests: report is missing: {args.report}", file=sys.stderr)
        return 0

    try:
        summary = parse_report(args.report)
    except (ET.ParseError, OSError, LookupError, ValueError) as error:
        print(f"slowest_tests: could not parse {args.report}: {error}", file=sys.stderr)
        return 0

    markdown = render_markdown(summary, max(0, args.count))
    try:
        reconfigure = getattr(sys.stdout, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(errors="replace")
        sys.stdout.write(markdown)
        sys.stdout.flush()
    except (OSError, ValueError, UnicodeError) as error:
        print(f"slowest_tests: could not print report: {error}", file=sys.stderr)

    if args.summary_file is not None:
        try:
            with args.summary_file.open("a", encoding="utf-8") as handle:
                handle.write("\n" + markdown)
        except OSError as error:
            print(f"slowest_tests: could not write summary: {error}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
