#!/usr/bin/env python3
"""Compare ambient determinism receipts across CI lanes.

Each input is a CTest/JUnit XML file written by the ambient determinism receipt test. The receipt
is the single distinct line starting with ``AMBIENT_RECEIPT `` found in a ``system-out`` element, in
an ``ambient_receipt`` property (child element form), or in an ``ambient_receipt`` attribute on a
``testcase`` element (the form GoogleTest writes for RecordProperty). A receipt taken from a
testcase that has a failure, error or skipped child is rejected. Every key in COMPARED_KEYS must be
identical across labels; ``cpu_simd`` is reported in the output but never compared.

Usage:
    compare_ambient_receipts.py LABEL=path.xml [LABEL=path.xml ...] [--expect-labels a,b,c]

Output is a JSON document on stdout. Exit codes: 0 when the receipts agree, 1 when they disagree,
2 on usage or input errors (reported on stderr).
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET


SCHEMA = "luminumbra.ambient_receipt_comparison.v1"
RECEIPT_PREFIX = "AMBIENT_RECEIPT "
RECEIPT_VERSION = "1"
PROPERTY_NAME = "ambient_receipt"
FAILURE_TAGS = ("failure", "error", "skipped")
INTEGER_KEYS = ("seed", "ticks", "ceiling")
HASH_KEYS = (
    "wind_ctor",
    "wind_evolved",
    "weather_ctor",
    "weather_evolved",
    "aether_ctor",
    "aether_evolved",
)
COMPARED_KEYS = ("seed", "ticks", "ceiling") + HASH_KEYS


class InputError(Exception):
    """A usage or input problem that makes the comparison impossible."""


def _receipt_lines(text):
    return {
        line.strip()
        for line in text.splitlines()
        if line.strip().startswith(RECEIPT_PREFIX)
    }


def _receipts_by_source(root):
    """Return (every receipt line, receipt lines taken from a failed testcase)."""
    found = set()
    failed = set()
    for testcase in root.iter("testcase"):
        lines = _receipt_lines(testcase.get(PROPERTY_NAME) or "")
        for element in testcase.iter("property"):
            if element.get("name") == PROPERTY_NAME:
                lines.update(_receipt_lines(element.get("value") or ""))
        for element in testcase.iter("system-out"):
            lines.update(_receipt_lines("".join(element.itertext())))
        found.update(lines)
        if any(child.tag in FAILURE_TAGS for child in testcase):
            failed.update(lines)
    for element in root.iter("system-out"):
        found.update(_receipt_lines("".join(element.itertext())))
    for element in root.iter("property"):
        if element.get("name") == PROPERTY_NAME:
            found.update(_receipt_lines(element.get("value") or ""))
    return found, failed


def _parse_receipt(label, line):
    receipt = {}
    for token in line.split()[1:]:
        key, separator, value = token.partition("=")
        if not separator or not key:
            raise InputError(f"{label}: malformed receipt field {token!r}")
        if key in receipt:
            raise InputError(f"{label}: duplicate receipt field {key!r}")
        receipt[key] = value
    if receipt.get("version") != RECEIPT_VERSION:
        raise InputError(
            f"{label}: unsupported receipt version {receipt.get('version')!r}, "
            f"expected {RECEIPT_VERSION!r}"
        )
    missing = [key for key in COMPARED_KEYS if key not in receipt]
    if missing:
        raise InputError(f"{label}: receipt is missing fields {', '.join(missing)}")
    for key in INTEGER_KEYS:
        if not re.fullmatch(r"[0-9]+", receipt[key]):
            raise InputError(
                f"{label}: field {key!r} must be a base-10 integer, got {receipt[key]!r}"
            )
    for key in HASH_KEYS:
        if not re.fullmatch(r"[0-9a-fA-F]+", receipt[key]):
            raise InputError(
                f"{label}: field {key!r} must be a non-empty hexadecimal hash, "
                f"got {receipt[key]!r}"
            )
    return receipt


def _load_receipt(label, path):
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as exc:
        raise InputError(f"{label}: cannot read {path}: {exc}") from exc
    lines, failed = _receipts_by_source(tree.getroot())
    if not lines:
        raise InputError(f"{label}: no receipt in {path}")
    if len(lines) > 1:
        raise InputError(
            f"{label}: multiple receipts in {path} ({len(lines)} distinct lines)"
        )
    line = next(iter(lines))
    if line in failed:
        raise InputError(
            f"{label}: receipt in {path} comes from a failed, errored or skipped testcase"
        )
    return _parse_receipt(label, line)


def _parse_inputs(arguments):
    pairs = []
    seen = set()
    for argument in arguments:
        label, separator, path = argument.partition("=")
        if not separator or not label or not path:
            raise InputError(f"expected LABEL=path.xml, got {argument!r}")
        if label in seen:
            raise InputError(f"duplicate label {label!r}")
        seen.add(label)
        pairs.append((label, path))
    return pairs


def _parse_expected(value):
    if value is None:
        return None
    labels = [item.strip() for item in value.split(",")]
    if any(not item for item in labels):
        raise InputError(f"empty label in --expect-labels {value!r}")
    if len(set(labels)) != len(labels):
        raise InputError(f"duplicate label in --expect-labels {value!r}")
    return set(labels)


def _mismatches(receipts):
    result = []
    for key in COMPARED_KEYS:
        values = {label: receipt[key] for label, receipt in receipts.items()}
        if len(set(values.values())) > 1:
            result.append({"key": key, "values": values})
    return result


def _build_parser():
    parser = argparse.ArgumentParser(
        prog="compare_ambient_receipts.py",
        description="Compare ambient determinism receipts from CTest JUnit XML files.",
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        metavar="LABEL=path.xml",
        help="a label and the JUnit XML file that carries its receipt",
    )
    parser.add_argument(
        "--expect-labels",
        metavar="a,b,c",
        default=None,
        help="the exact set of labels that must be present",
    )
    return parser


def main(argv=None):
    try:
        args = _build_parser().parse_args(argv)
    except SystemExit as exc:
        return 0 if exc.code == 0 else 2

    try:
        pairs = _parse_inputs(args.inputs)
        expected = _parse_expected(args.expect_labels)
        receipts = {label: _load_receipt(label, path) for label, path in pairs}
        if expected is not None and set(receipts) != expected:
            raise InputError(
                f"labels {sorted(receipts)} do not match expected {sorted(expected)}"
            )
    except InputError as exc:
        print(f"compare_ambient_receipts: error: {exc}", file=sys.stderr)
        return 2

    mismatches = _mismatches(receipts)
    document = {
        "schema": SCHEMA,
        "labels": receipts,
        "mismatches": mismatches,
        "verdict": "disagree" if mismatches else "agree",
    }
    print(json.dumps(document, indent=2))
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
