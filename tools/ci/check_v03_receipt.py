#!/usr/bin/env python3
"""Evaluate a v0.3 acceptance receipt and print a verdict.

Usage: check_v03_receipt.py RECEIPT.json [--verdict-out PATH]

Input schema "luminumbra.v03_acceptance.v1": {schema, revision (optional str),
sections: {<name>: {status, evidence, detail?, skipped_tests?, required_tests?,
discovered_tests?}}, evidence_root?}. Evidence references must resolve to regular
files strictly within an existing directory evidence_root (relative to the
receipt directory, default "."). References to the root itself are rejected.
The root must also resolve within the receipt directory and contain no ".."
components using either separator. Traversal and symlink escapes are rejected.
Passing net_soak sections require nonempty required_tests
and discovered_tests lists; skipped_tests must be a nonnegative integer.
Each required section yields one
check whose id is the section name. Sections not in REQUIRED_SECTIONS yield an "unknown_section:<name>"
check that is always unevaluated, so they never make the overall verdict pass.

Exit codes: 0 pass, 1 fail, 2 invalid input, 3 unevaluated.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path, PureWindowsPath

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


def _check_evidence(name: str, evidence: str, root: Path) -> dict:
    # Check both separators so receipts cannot hide Windows traversal on POSIX.
    if ".." in evidence.replace("\\", "/").split("/"):
        return _result(name, FAIL, f"evidence traversal: {evidence}")
    reference = Path(evidence)
    if PureWindowsPath(evidence).is_absolute() and not reference.is_absolute():
        return _result(name, FAIL, f"evidence outside root: {evidence}")
    try:
        artifact = (root / reference).resolve()
        artifact.relative_to(root)
    except ValueError:
        return _result(name, FAIL, f"evidence outside root: {evidence}")
    except (OSError, RuntimeError) as error:
        return _result(name, FAIL, f"cannot resolve evidence {evidence}: {error}")
    if artifact == root:
        return _result(name, FAIL, f"evidence resolves to root itself: {evidence}")
    try:
        if not artifact.exists():
            return _result(name, FAIL, f"missing evidence artifact: {evidence}")
        if not artifact.is_file():
            return _result(name, FAIL, f"evidence is not a regular file: {evidence}")
    except OSError as error:
        return _result(name, FAIL, f"cannot inspect evidence {evidence}: {error}")
    return _result(name, PASS, f"evidence: {evidence}")


def _check_section(name: str, section, evidence_root: Path) -> dict:
    if not isinstance(section, dict):
        return _result(name, FAIL, "malformed section")
    if section.get("status") == "skipped":
        return _result(name, FAIL, "unexpected skip")
    if "skipped_tests" in section:
        skipped = section["skipped_tests"]
        if not _is_int(skipped) or skipped < 0:
            return _result(name, FAIL, "malformed skipped_tests")
        if skipped > 0:
            return _result(name, FAIL, f"unexpected skipped tests: {skipped}")
    if name == "net_soak" and section.get("status") == "pass":
        for field in ("required_tests", "discovered_tests", "skipped_tests"):
            if field not in section:
                return _result(name, FAIL, f"missing {field}")
        if not section["required_tests"]:
            return _result(name, FAIL, "malformed required_tests")
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
        return _check_evidence(name, evidence, evidence_root)
    return _result(name, FAIL, f"unknown status: {status!r}")


def _overall(checks: list) -> str:
    verdicts = {check["verdict"] for check in checks}
    if FAIL in verdicts:
        return FAIL
    if UNEVALUATED in verdicts:
        return UNEVALUATED
    return PASS


def evaluate(receipt, receipt_dir=None) -> dict:
    """Return the verdict dict for a parsed receipt.

    Raises InputError when the receipt is not an object with the right schema
    and a sections object, or evidence_root is malformed. receipt_dir defaults
    to the current directory for programmatic callers; the CLI uses the receipt
    file's directory regardless of the working directory.
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
    root = receipt.get("evidence_root", ".")
    if not isinstance(root, str) or not root:
        raise InputError("malformed receipt: evidence_root must be a nonempty string")
    if Path(root).is_absolute() or PureWindowsPath(root).anchor:
        raise InputError("malformed receipt: evidence_root must be relative")
    if ".." in root.replace("\\", "/").split("/"):
        raise InputError("malformed receipt: evidence_root must not contain '..' components")
    try:
        receipt_directory = Path(receipt_dir or Path.cwd()).resolve()
        evidence_root = (receipt_directory / root).resolve()
    except (OSError, RuntimeError, ValueError) as error:
        raise InputError(f"cannot resolve evidence_root: {error}") from error
    try:
        evidence_root.relative_to(receipt_directory)
    except ValueError as error:
        raise InputError("malformed receipt: evidence_root outside receipt directory") from error
    try:
        if not evidence_root.is_dir():
            raise InputError("malformed receipt: evidence_root must be an existing directory")
    except OSError as error:
        raise InputError(f"cannot inspect evidence_root: {error}") from error
    checks = []
    for name in REQUIRED_SECTIONS:
        if name not in sections:
            checks.append(_result(name, UNEVALUATED, "section absent"))
        else:
            checks.append(_check_section(name, sections[name], evidence_root))
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
        verdict = evaluate(load_receipt(args.receipt), Path(args.receipt).absolute().parent)
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
