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
Passing audio_off sections require runtime_snapshots with ordinary, no_audio and
no_audio_reversed runs, each {exit_code, timed_out, state}. The state must contain
the producer's luminumbra.audio_acceptance.v1 observations; see audio-release-policy.md.
Each required section yields one
check whose id is the section name. Sections not in REQUIRED_SECTIONS yield an "unknown_section:<name>"
check that is always unevaluated, so they never make the overall verdict pass.

Exit codes: 0 pass, 1 fail, 2 invalid input, 3 unevaluated.
"""

from __future__ import annotations

import argparse
import json
import math
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


class AudioEvidenceError(Exception):
    """A runtime observation cannot qualify amended audio acceptance."""


def _audio_match(actual, expected, path: str) -> None:
    # Only traverse the fixed v1 contract, never arbitrary receipt subtrees.
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            raise AudioEvidenceError(f"{path}: expected object")
        for key, value in expected.items():
            if key not in actual:
                raise AudioEvidenceError(f"{path}.{key}: missing key")
            _audio_match(actual[key], value, f"{path}.{key}")
    elif type(actual) is not type(expected) or actual != expected:
        raise AudioEvidenceError(f"{path}: expected {expected!r} ({type(expected).__name__})")


def _audio_volumes(value, path: str) -> dict:
    if not isinstance(value, dict):
        raise AudioEvidenceError(f"{path}: expected observed volumes object")
    for channel in ("master", "music", "sfx"):
        if channel not in value:
            raise AudioEvidenceError(f"{path}.{channel}: missing key")
        level = value[channel]
        if (type(level) not in (int, float) or not 0 < level <= 1
                or not math.isfinite(level)):
            raise AudioEvidenceError(f"{path}.{channel}: expected finite nonzero level in (0, 1]")
    return {channel: value[channel] for channel in ("master", "music", "sfx")}


def _audio_telemetry(value, path: str, ordinary: bool) -> None:
    _audio_match(value, {"opt_in_only": True}, path)
    for key in ("opt_in_requested", "enabled", "retained_requests"):
        if key not in value:
            raise AudioEvidenceError(f"{path}.{key}: missing key")
    for key in ("opt_in_requested", "enabled"):
        if type(value[key]) is not bool:
            raise AudioEvidenceError(f"{path}.{key}: expected boolean")
    retained = value["retained_requests"]
    if not _is_int(retained) or retained < 0:
        raise AudioEvidenceError(f"{path}.retained_requests: expected nonnegative integer")
    opted_in = value["opt_in_requested"]
    if ordinary and opted_in:
        raise AudioEvidenceError(f"{path}.opt_in_requested: ordinary startup must not opt in")
    if not opted_in and (value["enabled"] or retained != 0):
        raise AudioEvidenceError(f"{path}: telemetry or retained requests without opt-in")


def _check_audio(section: dict) -> None:
    snapshots = section.get("runtime_snapshots")
    if not isinstance(snapshots, dict):
        raise AudioEvidenceError(
            "missing or malformed runtime_snapshots; stale default-off-only evidence cannot qualify")
    saved = None
    # Three sessions: ordinary startup and explicit suppression in both flag orders.
    for role in ("ordinary", "no_audio", "no_audio_reversed"):
        if role not in snapshots:
            raise AudioEvidenceError(f"runtime_snapshots.{role}: missing key")
        run = snapshots[role]
        path = f"runtime_snapshots.{role}"
        _audio_match(run, {"exit_code": 0, "timed_out": False, "state": {
            "schema": "luminumbra.runtime_state.v1", "audio_acceptance": {}}}, path)
        audio = run["state"]["audio_acceptance"]
        path += ".state.audio_acceptance"
        playback = role == "ordinary"
        _audio_match(audio, {
            "schema": "luminumbra.audio_acceptance.v1",
            "resolution": {"no_audio_requested": not playback,
                           "enable_audio_requested": not playback,
                           "requested_mode": "device" if playback else "null",
                           "resolved_mode": "device" if playback else "null",
                           "reason": "default" if playback else "flag"},
            "backend": {"playback_enabled": playback, "engine_initialized": playback,
                        "device_started": playback,
                        "routing": {key: playback for key in ("sfx", "ambient", "events", "ui")}},
            "saved_volumes": {"unchanged": True},
            "controls": {"observed": True, "present": 3,
                         "disabled_count": 0 if playback else 3, "match_playback": True},
            "telemetry": {},
            "no_audio_precedence": None if playback else True,
            "controls_disabled": not playback,
        }, path)
        volumes = audio["saved_volumes"]
        for key in ("loaded", "in_effect", "playback_gains"):
            if key not in volumes:
                raise AudioEvidenceError(f"{path}.saved_volumes.{key}: missing key")
        loaded = _audio_volumes(volumes["loaded"], f"{path}.saved_volumes.loaded")
        current = _audio_volumes(volumes["in_effect"], f"{path}.saved_volumes.in_effect")
        if loaded != current or (saved is not None and loaded != saved):
            raise AudioEvidenceError(f"{path}.saved_volumes: saved volumes changed")
        saved = loaded
        if playback:
            gains = _audio_volumes(volumes["playback_gains"], f"{path}.saved_volumes.playback_gains")
            if gains != loaded:
                raise AudioEvidenceError(f"{path}.saved_volumes.playback_gains: differs from saved volumes")
        elif volumes["playback_gains"] is not None:
            raise AudioEvidenceError(f"{path}.saved_volumes.playback_gains: expected explicit null")
        _audio_telemetry(audio["telemetry"], f"{path}.telemetry", playback)


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
        detail = section.get("detail")
        reason = f": {detail}" if isinstance(detail, str) and detail else ""
        return _result(name, UNEVALUATED, "section reported unevaluated" + reason)
    if status == "pass":
        if evidence is not None and not isinstance(evidence, str):
            return _result(name, FAIL, "malformed evidence")
        if not evidence:
            return _result(name, UNEVALUATED, "pass without evidence")
        result = _check_evidence(name, evidence, evidence_root)
        if result["verdict"] == PASS and name == "audio_off":
            try:
                _check_audio(section)
            except AudioEvidenceError as error:
                return _result(name, FAIL, str(error))
        return result
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
