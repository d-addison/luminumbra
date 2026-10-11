#!/usr/bin/env python3
"""Evaluate a saved-world acceptance receipt and print a verdict.

Usage: check_acceptance_receipt.py RECEIPT.json [--min-switches N] [--verdict-out PATH]

Input schema "luminumbra.saved_world_acceptance.v1". Every check yields "pass",
"fail" or "unevaluated"; absence of evidence is never a pass. Overall verdict is
"fail" if any check failed, else "unevaluated" if any check is unevaluated, else
"pass".

Exit codes: 0 pass, 1 fail, 2 invalid input, 3 unevaluated.
"""

from __future__ import annotations

import argparse
import json
import re
import sys

INPUT_SCHEMA = "luminumbra.saved_world_acceptance.v1"
VERDICT_SCHEMA = "luminumbra.saved_world_acceptance_verdict.v1"
SHUTDOWN_SCHEMA = "luminumbra.shutdown.v1"
DEFAULT_MIN_SWITCHES = 6
REQUIRED_KINDS = ("create_edit_save", "restart_select_load", "second_seed", "switch_back")
REQUIRED_LABELS = ("before_edit", "after_save", "after_reload")
WORLD_INFO = "world_info.json"

PASS = "pass"
FAIL = "fail"
UNEVALUATED = "unevaluated"

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_UNEVALUATED = 3
EXIT_CODES = {PASS: EXIT_PASS, FAIL: EXIT_FAIL, UNEVALUATED: EXIT_UNEVALUATED}

_HEX64 = re.compile(r"[0-9a-f]{64}")
_TOP_LEVEL_TYPES = {
    "package": dict,
    "fixture": dict,
    "sessions": list,
    "save_receipts": list,
    "reload_changes": dict,
    "switches": list,
    "refusals": list,
}


class InputError(Exception):
    """Invalid input: unreadable file, not JSON, wrong schema or top-level types."""


def _is_hex64(value) -> bool:
    return isinstance(value, str) and _HEX64.fullmatch(value) is not None


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_nonempty_str(value) -> bool:
    return isinstance(value, str) and value != ""


def _result(check_id: str, verdict: str, detail: str) -> dict:
    return {"id": check_id, "verdict": verdict, "detail": detail}


def _missing(check_id: str, field: str) -> dict:
    return _result(check_id, UNEVALUATED, f"missing {field}")


def _malformed(check_id: str, field: str) -> dict:
    return _result(check_id, FAIL, f"malformed {field}")


def _package_ok(package: dict) -> bool:
    return (
        _is_hex64(package.get("exe_sha256"))
        and _is_nonempty_str(package.get("exe_name"))
        and _is_nonempty_str(package.get("version_stamp"))
    )


def _check_package(receipt: dict) -> dict:
    check_id = "package_identity"
    if "package" not in receipt:
        return _missing(check_id, "package")
    if not _package_ok(receipt["package"]):
        return _malformed(check_id, "package")
    return _result(check_id, PASS, "package identity recorded")


def _check_fixture(receipt: dict) -> dict:
    check_id = "fixture_identity"
    if "fixture" not in receipt:
        return _missing(check_id, "fixture")
    fixture = receipt["fixture"]
    if not _is_hex64(fixture.get("tree_digest")) or not (
        _is_int(fixture.get("file_count")) and fixture["file_count"] > 0
    ):
        return _malformed(check_id, "fixture")
    return _result(check_id, PASS, "fixture identity recorded")


def _session_ok(session) -> bool:
    if not isinstance(session, dict):
        return False
    if not isinstance(session.get("name"), str) or not isinstance(session.get("kind"), str):
        return False
    if not _is_int(session.get("exit_code")) or not isinstance(session.get("timed_out"), bool):
        return False
    shutdown = session.get("shutdown")
    if shutdown is None:
        return True
    if not isinstance(shutdown, dict) or not isinstance(shutdown.get("schema"), str):
        return False
    if "complete" in shutdown and not isinstance(shutdown["complete"], bool):
        return False
    if "jobs_drained" in shutdown and not isinstance(shutdown["jobs_drained"], bool):
        return False
    return True


def _check_sessions_present(receipt: dict) -> dict:
    check_id = "sessions_present"
    sessions = receipt.get("sessions")
    if not sessions:
        return _missing(check_id, "sessions")
    if not all(isinstance(s, dict) and isinstance(s.get("kind"), str) for s in sessions):
        return _malformed(check_id, "sessions")
    kinds = [session["kind"] for session in sessions]
    absent = [kind for kind in REQUIRED_KINDS if kind not in kinds]
    if absent:
        return _result(check_id, FAIL, "missing required session kinds: " + ", ".join(absent))
    positions = [kinds.index(kind) for kind in REQUIRED_KINDS]
    if positions != sorted(positions):
        return _result(check_id, FAIL, "required session kinds out of order")
    return _result(check_id, PASS, "required session kinds present in order")


def _check_sessions_clean(receipt: dict) -> dict:
    check_id = "sessions_clean"
    sessions = receipt.get("sessions")
    if not sessions:
        return _missing(check_id, "sessions")
    if not all(_session_ok(s) for s in sessions):
        return _malformed(check_id, "sessions")
    failed = []
    unevaluated = []
    for session in sessions:
        name = session["name"]
        shutdown = session.get("shutdown")
        reasons = []
        if session["timed_out"]:
            reasons.append("timed out")
        if session["exit_code"] != 0:
            reasons.append(f"exit code {session['exit_code']}")
        if shutdown is not None:
            if shutdown["schema"] != SHUTDOWN_SCHEMA:
                reasons.append(f"shutdown schema {shutdown['schema']!r}")
            if shutdown.get("jobs_drained") is not True:
                reasons.append("jobs not drained")
            if "complete" in shutdown and shutdown["complete"] is not True:
                reasons.append("shutdown not complete")
        if reasons:
            failed.append(f"{name} ({', '.join(reasons)})")
        elif shutdown is None:
            unevaluated.append(name)
    if failed:
        return _result(check_id, FAIL, "failed sessions: " + "; ".join(failed))
    if unevaluated:
        return _result(
            check_id, UNEVALUATED, "no shutdown record for sessions: " + ", ".join(unevaluated)
        )
    return _result(check_id, PASS, f"{len(sessions)} sessions clean")


def _save_entry_ok(entry) -> bool:
    return (
        isinstance(entry, dict)
        and isinstance(entry.get("label"), str)
        and _is_hex64(entry.get("tree_digest"))
        and _is_int(entry.get("file_count"))
    )


def _changes_ok(changes: dict) -> bool:
    for key in ("added", "removed", "changed"):
        value = changes.get(key)
        if not isinstance(value, list) or not all(isinstance(path, str) for path in value):
            return False
    return True


def _check_save_tree_changes(receipt: dict) -> dict:
    check_id = "save_tree_changes"
    if "save_receipts" not in receipt:
        return _missing(check_id, "save_receipts")
    entries = receipt["save_receipts"]
    if not all(_save_entry_ok(entry) for entry in entries):
        return _malformed(check_id, "save_receipts")
    by_label: dict = {}
    for entry in entries:
        if entry["label"] in by_label:
            return _malformed(check_id, "save_receipts")
        by_label[entry["label"]] = entry
    absent = [label for label in REQUIRED_LABELS if label not in by_label]
    if absent:
        return _result(check_id, UNEVALUATED, "missing save_receipts labels: " + ", ".join(absent))
    before = by_label["before_edit"]["tree_digest"]
    after_save = by_label["after_save"]["tree_digest"]
    after_reload = by_label["after_reload"]["tree_digest"]
    if before == after_save:
        return _result(check_id, FAIL, "edit and save did not change the tree")
    if "reload_changes" not in receipt:
        return _missing(check_id, "reload_changes")
    changes = receipt["reload_changes"]
    if not _changes_ok(changes):
        return _malformed(check_id, "reload_changes")
    if after_save != after_reload:
        problems = []
        if changes["added"]:
            problems.append("added: " + ", ".join(changes["added"]))
        if changes["removed"]:
            problems.append("removed: " + ", ".join(changes["removed"]))
        unexpected = [path for path in changes["changed"] if path != WORLD_INFO]
        if unexpected:
            problems.append("changed: " + ", ".join(unexpected))
        if problems:
            return _result(check_id, FAIL, "reload changed unexpected paths; " + "; ".join(problems))
        return _result(check_id, PASS, "reload differs only in world_info.json")
    if any(changes[key] for key in ("added", "removed", "changed")):
        return _result(check_id, FAIL, "reload_changes not empty but tree digests match")
    return _result(check_id, PASS, "reload leaves the tree unchanged")


def _switch_ok(switch) -> bool:
    if not isinstance(switch, dict):
        return False
    if not _is_int(switch.get("index")) or not isinstance(switch.get("world"), str):
        return False
    capture = switch.get("capture_sha256")
    if capture is not None and not _is_hex64(capture):
        return False
    probe = switch.get("g_buffer_probe")
    return probe is None or isinstance(probe, str)


def _check_switch_count(receipt: dict, min_switches: int) -> dict:
    check_id = "switch_count"
    if "switches" not in receipt:
        return _missing(check_id, "switches")
    switches = receipt["switches"]
    if not all(_switch_ok(switch) for switch in switches):
        return _malformed(check_id, "switches")
    if len(switches) < min_switches:
        return _result(
            check_id, FAIL, f"{len(switches)} switches, need at least {min_switches}"
        )
    return _result(check_id, PASS, f"{len(switches)} switches")


def _check_switch_captures(receipt: dict) -> dict:
    check_id = "switch_captures_identical"
    switches = receipt.get("switches")
    if not switches:
        return _missing(check_id, "switches")
    if not all(_switch_ok(switch) for switch in switches):
        return _malformed(check_id, "switches")
    null_indices = [str(s["index"]) for s in switches if s.get("capture_sha256") is None]
    if null_indices:
        return _result(
            check_id, UNEVALUATED, "capture_sha256 missing for switch index " + ", ".join(null_indices)
        )
    captures: dict = {}
    for switch in switches:
        captures.setdefault(switch["world"], set()).add(switch.get("capture_sha256"))
    divergent = sorted(world for world, values in captures.items() if len(values) > 1)
    if divergent:
        return _result(
            check_id, FAIL, "captures differ within world: " + ", ".join(divergent)
        )
    return _result(check_id, PASS, f"captures identical within each of {len(captures)} worlds")


def _refusal_ok(refusal) -> bool:
    return (
        isinstance(refusal, dict)
        and isinstance(refusal.get("variant"), str)
        and _is_int(refusal.get("exit_code"))
        and isinstance(refusal.get("diagnostic"), bool)
    )


def _check_refusals(receipt: dict) -> dict:
    check_id = "refusal_matrix"
    refusals = receipt.get("refusals")
    if not refusals:
        return _missing(check_id, "refusals")
    if not all(_refusal_ok(refusal) for refusal in refusals):
        return _malformed(check_id, "refusals")
    bad = [
        refusal["variant"]
        for refusal in refusals
        if refusal["exit_code"] == 0 or refusal["diagnostic"] is not True
    ]
    if bad:
        return _result(check_id, FAIL, "refusals not clean: " + ", ".join(bad))
    return _result(check_id, PASS, f"{len(refusals)} refusals exit nonzero with diagnostics")


def _top_level_problem(receipt) -> str | None:
    if not isinstance(receipt, dict):
        return "malformed receipt: top level is not a JSON object"
    if receipt.get("schema") != INPUT_SCHEMA:
        return f"unsupported receipt schema: {receipt.get('schema')!r}"
    for field, expected in _TOP_LEVEL_TYPES.items():
        if field in receipt and not isinstance(receipt[field], expected):
            return f"malformed receipt: {field} must be a {expected.__name__}"
    return None


def _overall(checks: list) -> str:
    verdicts = {check["verdict"] for check in checks}
    if FAIL in verdicts:
        return FAIL
    if UNEVALUATED in verdicts:
        return UNEVALUATED
    return PASS


def evaluate(receipt: dict, min_switches: int = DEFAULT_MIN_SWITCHES) -> dict:
    """Return the verdict dict for a parsed receipt.

    Raises InputError when the top level is not a valid receipt object.
    """
    problem = _top_level_problem(receipt)
    if problem is not None:
        raise InputError(problem)
    checks = [
        _check_package(receipt),
        _check_fixture(receipt),
        _check_sessions_present(receipt),
        _check_sessions_clean(receipt),
        _check_save_tree_changes(receipt),
        _check_switch_count(receipt, min_switches),
        _check_switch_captures(receipt),
        _check_refusals(receipt),
    ]
    return {
        "schema": VERDICT_SCHEMA,
        "verdict": _overall(checks),
        "min_switches": min_switches,
        "checks": checks,
    }


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
    parser = argparse.ArgumentParser(description="Check a saved-world acceptance receipt.")
    parser.add_argument("receipt")
    parser.add_argument("--min-switches", type=int, default=DEFAULT_MIN_SWITCHES)
    parser.add_argument("--verdict-out")
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return error.code if isinstance(error.code, int) else EXIT_INVALID
    if args.min_switches < 0:
        print("error: --min-switches must be >= 0", file=sys.stderr)
        return EXIT_INVALID
    try:
        verdict = evaluate(load_receipt(args.receipt), args.min_switches)
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
