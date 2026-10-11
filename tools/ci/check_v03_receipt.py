#!/usr/bin/env python3
"""Evaluate a v0.3 acceptance receipt and print a verdict.

Usage: check_v03_receipt.py RECEIPT.json [--verdict-out PATH]

Input schema "luminumbra.v03_acceptance.v1": {schema, revision (optional str),
sections: {<name>: {status, evidence, detail?, skipped_tests?, required_tests?,
discovered_tests?}}}. Each required section yields one check whose id is the
section name. Sections not in REQUIRED_SECTIONS yield an "unknown_section:<name>"
check that is always unevaluated, so they never make the overall verdict pass.

Exit codes: 0 pass, 1 fail, 2 invalid input, 3 unevaluated.
"""

from __future__ import annotations

import argparse
import json
import sys

INPUT_SCHEMA = "luminumbra.v03_acceptance.v1"
VERDICT_SCHEMA = "luminumbra.v03_acceptance_verdict.v1"
REQUIRED_SECTIONS = (
    "render_qualification",
    "saved_world_lifecycle",
    "noui_load",
    "serial_load_20",
    "net_soak",
    "perf_pointer",
    "audio_off",
)

PASS = "pass"
FAIL = "fail"
UNEVALUATED = "unevaluated"

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_UNEVALUATED = 3
EXIT_CODES = {PASS: EXIT_PASS, FAIL: EXIT_FAIL, UNEVALUATED: EXIT_UNEVALUATED}


class InputError(Exception):
    """Invalid input: unreadable file, not JSON, wrong schema or sections shape."""


def _result(check_id: str, verdict: str, detail: str) -> dict:
    return {"id": check_id, "verdict": verdict, "detail": detail}


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_str_list(value) -> bool:
    return isinstance(value, list) and all(isinstance(item, str) for item in value)


def _check_section(name: str, section) -> dict:
    if not isinstance(section, dict):
        return _result(name, FAIL, "malformed section")
    if section.get("status") == "skipped":
        return _result(name, FAIL, "unexpected skip")
    if "skipped_tests" in section:
        skipped = section["skipped_tests"]
        if not _is_int(skipped):
            return _result(name, FAIL, "malformed skipped_tests")
        if skipped > 0:
            return _result(name, FAIL, f"unexpected skipped tests: {skipped}")
    if "required_tests" in section:
        required = section["required_tests"]
        discovered = section.get("discovered_tests", [])
        if not _is_str_list(required) or not _is_str_list(discovered):
            return _result(name, FAIL, "malformed required_tests")
        absent = [test for test in required if test not in discovered]
        if absent:
            return _result(name, FAIL, "missing named discovery: " + ", ".join(absent))
    status = section.get("status")
    evidence = section.get("evidence")
    if status == "fail":
        return _result(name, FAIL, "section reported fail")
    if status == "unevaluated":
        return _result(name, UNEVALUATED, "section reported unevaluated")
    if status == "pass":
        if evidence is not None and not isinstance(evidence, str):
            return _result(name, FAIL, "malformed evidence")
        if not evidence:
            return _result(name, UNEVALUATED, "pass without evidence")
        return _result(name, PASS, f"evidence: {evidence}")
    return _result(name, FAIL, f"unknown status: {status!r}")


def _overall(checks: list) -> str:
    verdicts = {check["verdict"] for check in checks}
    if FAIL in verdicts:
        return FAIL
    if UNEVALUATED in verdicts:
        return UNEVALUATED
    return PASS


def evaluate(receipt) -> dict:
    """Return the verdict dict for a parsed receipt.

    Raises InputError when the receipt is not an object with the right schema
    and a sections object.
    """
    if not isinstance(receipt, dict):
        raise InputError("malformed receipt: top level is not a JSON object")
    if receipt.get("schema") != INPUT_SCHEMA:
        raise InputError(f"unsupported receipt schema: {receipt.get('schema')!r}")
    if "revision" in receipt and not isinstance(receipt["revision"], str):
        raise InputError("malformed receipt: revision must be a string")
    sections = receipt.get("sections")
    if not isinstance(sections, dict):
        raise InputError("malformed receipt: sections must be an object")
    checks = []
    for name in REQUIRED_SECTIONS:
        if name not in sections:
            checks.append(_result(name, UNEVALUATED, "section absent"))
        else:
            checks.append(_check_section(name, sections[name]))
    for name in sorted(sections):
        if name not in REQUIRED_SECTIONS:
            checks.append(
                _result(
                    f"unknown_section:{name}",
                    UNEVALUATED,
                    "unknown section; not counted as evidence",
                )
            )
    return {"schema": VERDICT_SCHEMA, "verdict": _overall(checks), "checks": checks}


def load_receipt(path) -> object:
    """Read and parse the receipt file; structural checks happen in evaluate."""
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise InputError(f"cannot read receipt {path}: {error}") from error
    except ValueError as error:
        raise InputError(f"not valid JSON: {path}: {error}") from error


def _write_verdict(path, verdict: dict) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(verdict, handle, indent=2, sort_keys=True)
        handle.write("\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Check a v0.3 acceptance receipt.")
    parser.add_argument("receipt")
    parser.add_argument("--verdict-out")
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return error.code if isinstance(error.code, int) else EXIT_INVALID
    try:
        verdict = evaluate(load_receipt(args.receipt))
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_INVALID
    if args.verdict_out is not None:
        try:
            _write_verdict(args.verdict_out, verdict)
        except OSError as error:
            print(f"error: cannot write verdict {args.verdict_out}: {error}", file=sys.stderr)
            return EXIT_INVALID
    for check in verdict["checks"]:
        print(f"{check['verdict']} {check['id']}: {check['detail']}")
    print(f"overall: {verdict['verdict']}")
    return EXIT_CODES[verdict["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
