#!/usr/bin/env python3
"""Evaluate a saved-world acceptance receipt and print a verdict.

Usage: check_acceptance_receipt.py RECEIPT.json [--min-switches N] [--verdict-out PATH]
                                   [--refusal-variants a,b,c]

Input schema "luminumbra.saved_world_acceptance.v1". Every check yields "pass",
"fail" or "unevaluated"; absence of evidence is never a pass. Overall verdict is
"fail" if any check failed, else "unevaluated" if any check is unevaluated, else
"pass".

Exit codes: 0 pass, 1 fail, 2 invalid input, 3 unevaluated.
"""

from __future__ import annotations

import argparse
import json
from pathlib import PurePosixPath, PureWindowsPath
import re
import sys

INPUT_SCHEMA = "luminumbra.saved_world_acceptance.v1"
VERDICT_SCHEMA = "luminumbra.saved_world_acceptance_verdict.v1"
SHUTDOWN_SCHEMA = "luminumbra.shutdown.v1"
PHASE_SCHEMA = "luminumbra.persistence_runtime_roundtrip_phase.v1"
REFUSAL_EXIT_CODE = 2
DEFAULT_MIN_SWITCHES = 6
REQUIRED_KINDS = ("create_edit_save", "restart_select_load", "second_seed", "switch_back")
# before_edit and fixture_after_loads snapshot the FIXTURE world (before any session
# and after all loads); after_save and after_reload snapshot the PRIMARY created world
# (after create/edit/save, and after the restart and switching sequence).
REQUIRED_LABELS = ("before_edit", "fixture_after_loads", "after_save", "after_reload")
REQUIRED_REFUSAL_VARIANTS = (
    "obsolete-metadata",
    "future-metadata",
    "corrupt-metadata",
    "missing-metadata",
    "missing-preset",
    "truncated-region",
)
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
# WorldPersistenceRoundtrip.cpp Checksum emits a zero-padded 64-bit hex value.
_WORLD_HASH = re.compile(r"[0-9a-f]{16}")
_TOP_LEVEL_TYPES = {
    "package": dict,
    "fixture": dict,
    "sessions": list,
    "save_receipts": list,
    "reload_changes": dict,
    "fixture_changes": dict,
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
    completion_missing = []
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
        elif "complete" not in shutdown:
            # The client omits this field when shutdown progress was not enabled.
            # A drained queue alone does not prove that shutdown completed.
            completion_missing.append(name)
    if failed:
        return _result(check_id, FAIL, "failed sessions: " + "; ".join(failed))
    missing_details = []
    if unevaluated:
        missing_details.append("no shutdown record for sessions: " + ", ".join(unevaluated))
    if completion_missing:
        missing_details.append("shutdown completion missing for sessions: " + ", ".join(completion_missing))
    if missing_details:
        return _result(check_id, UNEVALUATED, "; ".join(missing_details))
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


def _judge_pair(name: str, before: str, after: str, changes: dict) -> tuple:
    """Apply the change rule to one before/after pair. Returns (ok, detail)."""
    total = sum(len(changes[key]) for key in ("added", "removed", "changed"))
    if before == after:
        if total:
            return False, f"{name}: changes listed but tree digests match"
        return True, f"{name}: tree unchanged"
    if total == 0:
        return False, f"{name}: digest differs without change evidence"
    problems = []
    if changes["added"]:
        problems.append("added: " + ", ".join(changes["added"]))
    if changes["removed"]:
        problems.append("removed: " + ", ".join(changes["removed"]))
    unexpected = [path for path in changes["changed"] if path != WORLD_INFO]
    if unexpected:
        problems.append("changed: " + ", ".join(unexpected))
    if problems:
        return False, f"{name}: unexpected paths; " + "; ".join(problems)
    return True, f"{name}: differs only in world_info.json"


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
    for field in ("reload_changes", "fixture_changes"):
        if field not in receipt:
            return _missing(check_id, field)
    for field in ("reload_changes", "fixture_changes"):
        if not _changes_ok(receipt[field]):
            return _malformed(check_id, field)
    if by_label["after_save"]["file_count"] < 1:
        return _result(check_id, FAIL, "primary world has no files after save")
    primary_ok, primary_detail = _judge_pair(
        "primary",
        by_label["after_save"]["tree_digest"],
        by_label["after_reload"]["tree_digest"],
        receipt["reload_changes"],
    )
    fixture_ok, fixture_detail = _judge_pair(
        "fixture",
        by_label["before_edit"]["tree_digest"],
        by_label["fixture_after_loads"]["tree_digest"],
        receipt["fixture_changes"],
    )
    detail = primary_detail + "; " + fixture_detail
    if not (primary_ok and fixture_ok):
        return _result(check_id, FAIL, detail)
    return _result(check_id, PASS, detail)


def _switch_ok(switch) -> bool:
    if not isinstance(switch, dict):
        return False
    if not _is_int(switch.get("index")) or not _is_nonempty_str(switch.get("world")):
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
    if len({switch["world"] for switch in switches}) < 2:
        return _result(check_id, FAIL, "switches require at least two distinct world IDs")
    if not any(a["world"] != b["world"] for a, b in zip(switches, switches[1:])):
        return _result(check_id, FAIL, "no consecutive entries switch world IDs")
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


def _check_switch_probes(receipt: dict) -> dict:
    check_id = "switch_probes"
    switches = receipt.get("switches")
    if not switches:
        return _missing(check_id, "switches")
    if not all(_switch_ok(switch) for switch in switches):
        return _malformed(check_id, "switches")
    null_indices = [str(s["index"]) for s in switches if not isinstance(s.get("g_buffer_probe"), str)]
    if null_indices:
        return _result(
            check_id, UNEVALUATED, "g_buffer_probe missing for switch index " + ", ".join(null_indices)
        )
    empty_indices = [str(s["index"]) for s in switches if s["g_buffer_probe"] == ""]
    if empty_indices:
        return _result(
            check_id, FAIL, "empty g_buffer_probe for switch index " + ", ".join(empty_indices)
        )
    return _result(check_id, PASS, f"g_buffer_probe recorded for {len(switches)} switches")


def _check_edit_roundtrip(receipt: dict) -> dict:
    check_id = "edit_roundtrip"
    trip = receipt.get("edit_roundtrip")
    if trip is None:
        return _missing(check_id, "edit_roundtrip")
    if (
        not isinstance(trip, dict)
        or not _is_int(trip.get("edited_chunk_count"))
    ):
        return _malformed(check_id, "edit_roundtrip")
    for phase in ("save", "load"):
        if trip.get(f"{phase}_phase_schema") != PHASE_SCHEMA:
            return _result(check_id, FAIL, f"invalid {phase} phase artifact schema")
        value = trip.get(f"{phase}_world_hash")
        if not isinstance(value, str) or _WORLD_HASH.fullmatch(value) is None:
            return _result(
                check_id, FAIL, f"{phase}_world_hash must be 16-character lowercase hex"
            )
    if trip["edited_chunk_count"] < 1:
        return _result(check_id, FAIL, "save phase recorded no edited chunks")
    if trip["save_world_hash"] != trip["load_world_hash"]:
        return _result(
            check_id, FAIL, "edited chunk hash after reload differs from the hash at save"
        )
    return _result(
        check_id,
        PASS,
        f"edited chunk hash identical after reload ({trip['edited_chunk_count']} chunks)",
    )


OBSOLETE_WORLD_MESSAGE = (
    "This world predates the v0.3.0 format and cannot be opened. Create a new world; "
    "migration is not supported."
)
MISSING_METADATA_MESSAGE = "World unavailable: its save directory or metadata is missing or unreadable."
_REFUSAL_HASHES = ("fixture_manifest_sha256", "expected_tree_digest", "before_tree_digest", "after_tree_digest")
_REFUSAL_BINDING = (*_REFUSAL_HASHES, "expected_reason", "observed_reason", "jobs_drained", "shutdown",
                    "staged_world_dir", "world_type", "mutation_path", "atlas_fallback_absent")


def _refusal_ok(refusal) -> bool:
    if not (isinstance(refusal, dict) and isinstance(refusal.get("variant"), str)
            and _is_int(refusal.get("exit_code")) and isinstance(refusal.get("diagnostic"), bool)
            and ("timed_out" not in refusal or isinstance(refusal["timed_out"], bool))):
        return False
    for name in _REFUSAL_HASHES:
        if name in refusal and not _is_hex64(refusal[name]):
            return False
    for name in ("observed_reason", "staged_world_dir", "world_type", "mutation_path"):
        if name in refusal and refusal[name] is not None and not isinstance(refusal[name], str):
            return False
    for name in ("jobs_drained", "atlas_fallback_absent"):
        if name in refusal and refusal[name] is not None and not isinstance(refusal[name], bool):
            return False
    world_text = refusal.get("staged_world_dir")
    if world_text is not None:
        try:
            world = _runtime_path(world_text)
        except ValueError:
            return False
        if world.name != refusal["variant"] or world.parent.name != "saves" or world.parent.parent.name != "worlds":
            return False
    world_type = refusal.get("world_type")
    if world_type is not None and (not world_type or world_type in (".", "..") or re.search(r"[/\\:]", world_type)):
        return False
    mutation = refusal.get("mutation_path")
    if mutation is not None and refusal["variant"] in REQUIRED_REFUSAL_VARIANTS:
        if refusal["variant"] == "truncated-region":
            if not re.fullmatch(r"chunks/region/r\.-?[0-9]+\.-?[0-9]+\.lmr", mutation):
                return False
        elif mutation != ("preset.json" if refusal["variant"] == "missing-preset" else WORLD_INFO):
            return False
    reason = refusal.get("expected_reason")
    if reason is not None:
        if not isinstance(reason, dict) or reason.get("kind") != refusal["variant"]:
            return False
        versioned = refusal["variant"] in ("obsolete-metadata", "future-metadata")
        if set(reason) != ({"kind", "version"} if versioned else {"kind"}):
            return False
        if versioned and (not _is_int(reason["version"]) or reason["version"] < 1):
            return False
        if refusal["variant"] == "obsolete-metadata" and reason["version"] != 1:
            return False
        if refusal["variant"] == "future-metadata" and reason["version"] <= 1:
            return False
    shutdown = refusal.get("shutdown")
    if shutdown is not None:
        if not isinstance(shutdown, dict) or (shutdown.get("schema") is not None and not isinstance(shutdown["schema"], str)):
            return False
        for name in ("complete", "jobs_drained"):
            if name in shutdown and shutdown[name] is not None and not isinstance(shutdown[name], bool):
                return False
    return True


def _runtime_path(text):
    if not _is_nonempty_str(text):
        raise ValueError("missing runtime path")
    path = PureWindowsPath(text) if PureWindowsPath(text).drive else PurePosixPath(text)
    if not path.is_absolute() or ".." in path.parts:
        raise ValueError("runtime path must be absolute without parent traversal")
    return path


def _same_runtime_path(observed, expected) -> bool:
    try:
        actual = _runtime_path(observed)
    except ValueError:
        return False
    return type(actual) is type(expected) and actual == expected


def _reason_matches(refusal) -> bool:
    variant, observed = refusal["variant"], refusal["observed_reason"]
    if not _is_nonempty_str(observed):
        return False
    if variant == "obsolete-metadata":
        return observed == OBSOLETE_WORLD_MESSAGE
    if variant == "future-metadata":
        return observed == "unsupported future world metadata container version " + str(refusal["expected_reason"]["version"])
    if variant == "corrupt-metadata":
        # The producer writes exactly '{', so catalog validation must fail at
        # the next object-key token, rather than at a size/type/spawn check.
        return re.fullmatch(
            r"Corrupt world metadata: \[json\.exception\.parse_error\.101\] "
            r"parse error at line 1, column 2: syntax error while parsing object key - "
            r"unexpected end of input; expected string literal", observed) is not None
    if variant == "missing-metadata":
        return observed == MISSING_METADATA_MESSAGE
    try:
        world = _runtime_path(refusal["staged_world_dir"])
        if world.name != variant or world.parent.name != "saves" or world.parent.parent.name != "worlds":
            return False
        if variant == "missing-preset":
            world_type = refusal["world_type"]
            if not _is_nonempty_str(world_type) or world_type in (".", "..") or re.search(r"[/\\:]", world_type):
                return False
            prefix = "failed to open world preset: "
            expected = world.parent.parent / "atlas" / "presets" / (world_type + ".json")
        elif variant == "truncated-region":
            relative = refusal["mutation_path"]
            if not isinstance(relative, str) or not re.fullmatch(r"chunks/region/r\.-?[0-9]+\.-?[0-9]+\.lmr", relative):
                return False
            prefix = "region file record payload is truncated: "
            expected = world.joinpath(*relative.split("/"))
        else:
            return False
        return observed.startswith(prefix) and _same_runtime_path(observed[len(prefix):], expected)
    except (KeyError, ValueError, TypeError):
        return False


def _check_refusals(receipt: dict, required: tuple) -> dict:
    check_id = "refusal_matrix"
    refusals = receipt.get("refusals")
    if not refusals:
        return _missing(check_id, "refusals")
    if not all(_refusal_ok(refusal) for refusal in refusals):
        return _malformed(check_id, "refusals")
    variants = [refusal["variant"] for refusal in refusals]
    if len(set(variants)) != len(variants):
        return _result(check_id, FAIL, "duplicate refusal variants")
    hashes = {refusal["fixture_manifest_sha256"] for refusal in refusals if "fixture_manifest_sha256" in refusal}
    if len(hashes) > 1:
        return _result(check_id, FAIL, "mixed refusal fixture manifests")
    known = [variant for variant in variants if variant in REQUIRED_REFUSAL_VARIANTS]
    if known != [variant for variant in REQUIRED_REFUSAL_VARIANTS if variant in known]:
        return _result(check_id, FAIL, "refusal variants out of registry order")
    failed, pending = [], []
    missing = [variant for variant in required if variant not in variants]
    if missing:
        pending.append("missing variants: " + ", ".join(missing))
    untimed = [refusal["variant"] for refusal in refusals if "timed_out" not in refusal]
    if untimed:
        pending.append("timed_out absent for: " + ", ".join(untimed))
    for refusal in refusals:
        variant = refusal["variant"]
        reasons = []
        if refusal["exit_code"] != REFUSAL_EXIT_CODE:
            reasons.append(f"exit code {refusal['exit_code']}")
        if refusal["diagnostic"] is not True:
            reasons.append("no diagnostic")
        if refusal.get("timed_out") is True:
            reasons.append("timed out")
        absent = [name for name in _REFUSAL_BINDING if name not in refusal or
                  (refusal[name] is None and name != "atlas_fallback_absent")]
        if absent:
            pending.append(variant + " evidence absent: " + ", ".join(absent))
        if variant not in REQUIRED_REFUSAL_VARIANTS:
            pending.append("unknown native refusal contract: " + variant)
        if ("before_tree_digest" in refusal and "after_tree_digest" in refusal
                and refusal["before_tree_digest"] != refusal["after_tree_digest"]):
            reasons.append("before/after staged bytes differ")
        if "expected_tree_digest" in refusal:
            for name in ("before_tree_digest", "after_tree_digest"):
                if name in refusal and refusal[name] != refusal["expected_tree_digest"]:
                    reasons.append(name + " differs from staged manifest inventory")
        if refusal.get("jobs_drained") is False:
            reasons.append("jobs not drained")
        shutdown = refusal.get("shutdown")
        if shutdown is not None:
            if (shutdown.get("schema") is not None and shutdown["schema"] != SHUTDOWN_SCHEMA) or shutdown.get("complete") is False or shutdown.get("jobs_drained") is False:
                reasons.append("shutdown not complete and drained")
            if any(shutdown.get(name) is None for name in ("schema", "complete", "jobs_drained")):
                pending.append(variant + " complete shutdown evidence absent")
            if shutdown.get("jobs_drained") is not None and refusal.get("jobs_drained") is not None and shutdown["jobs_drained"] != refusal["jobs_drained"]:
                reasons.append("jobs_drained differs from shutdown record")
        if variant == "missing-preset":
            if refusal.get("atlas_fallback_absent") is False:
                reasons.append("atlas preset fallback was present")
            elif refusal.get("atlas_fallback_absent") is None:
                pending.append(variant + " atlas fallback absence not observed")
        elif "atlas_fallback_absent" in refusal and refusal["atlas_fallback_absent"] is not None:
            reasons.append("unexpected atlas fallback condition")
        if (variant in REQUIRED_REFUSAL_VARIANTS and refusal.get("expected_reason") is not None
                and refusal.get("observed_reason") is not None
                and (variant not in ("missing-preset", "truncated-region") or
                     all(refusal.get(name) is not None for name in ("staged_world_dir", "world_type", "mutation_path")))
                and not _reason_matches(refusal)):
            reasons.append("wrong World open refused reason")
        if reasons:
            failed.append(f"{variant} ({', '.join(reasons)})")
    if failed:
        return _result(check_id, FAIL, "; ".join(["refusals not clean: " + "; ".join(failed), *pending]))
    if pending:
        return _result(check_id, UNEVALUATED, "; ".join(pending))
    return _result(check_id, PASS, f"{len(refusals)} manifest-bound refusals exit 2 with exact reasons, unchanged bytes and drained shutdown")


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


def evaluate(
    receipt: dict,
    min_switches: int = DEFAULT_MIN_SWITCHES,
    refusal_variants: tuple = REQUIRED_REFUSAL_VARIANTS,
) -> dict:
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
        _check_edit_roundtrip(receipt),
        _check_switch_count(receipt, min_switches),
        _check_switch_captures(receipt),
        _check_switch_probes(receipt),
        _check_refusals(receipt, refusal_variants),
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
    parser.add_argument(
        "--refusal-variants",
        help="comma-separated required refusal variants (default: "
        + ",".join(REQUIRED_REFUSAL_VARIANTS) + ")",
    )
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return error.code if isinstance(error.code, int) else EXIT_INVALID
    if args.min_switches < 0:
        print("error: --min-switches must be >= 0", file=sys.stderr)
        return EXIT_INVALID
    refusal_variants = REQUIRED_REFUSAL_VARIANTS
    if args.refusal_variants is not None:
        refusal_variants = tuple(v.strip() for v in args.refusal_variants.split(",") if v.strip())
        if not refusal_variants:
            print("error: --refusal-variants must name at least one variant", file=sys.stderr)
            return EXIT_INVALID
    try:
        verdict = evaluate(load_receipt(args.receipt), args.min_switches, refusal_variants)
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
