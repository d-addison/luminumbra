#!/usr/bin/env python3
"""Check measured residency bytes against a sim-budget profile (stdlib only).

Exit codes: 0 all profile classes pass, 1 any class fails, 3 any class is unevaluated
(no failures), 2 usage or invalid input.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

PROFILE_SCHEMA = "luminumbra.sim_budget_profile.v1"
VERDICT_SCHEMA = "luminumbra.residency_verdict.v1"
EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_UNEVALUATED = 3

# Mirrors the limits in simulation/SimBudgetProfile.h.
MAX_PROFILE_BYTES = 64 * 1024
MAX_PROFILE_STAGES = 64
MAX_PROFILE_RESIDENCY_CLASSES = 32
MAX_UINT64 = 2**64 - 1
MAX_UINT32 = 2**32 - 1
MAX_REDUCED_SHIFT = 16
PROFILE_KEYS = frozenset({"schema", "region_scheduler", "stages", "residency"})
SCHEDULER_KEYS = frozenset({"work_limit", "hold_ticks", "reduced_shift"})
NAME_PATTERN = re.compile(r"[a-z0-9_.-]{1,64}")


class InvalidInput(Exception):
    pass


def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise InvalidInput(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def reject_constant(name: str) -> Any:
    raise InvalidInput(f"non-finite constant {name} is not allowed")


def load_json(path: Path, max_bytes: int | None = None) -> Any:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise InvalidInput(f"cannot read {path}: {exc}") from exc
    if max_bytes is not None and len(raw) > max_bytes:
        raise InvalidInput(f"{path} is larger than {max_bytes} bytes")
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise InvalidInput(f"{path} is not valid UTF-8: {exc}") from exc
    try:
        return json.loads(text, object_pairs_hook=reject_duplicates, parse_constant=reject_constant)
    except json.JSONDecodeError as exc:
        raise InvalidInput(f"{path} is not valid JSON: {exc}") from exc
    except RecursionError as exc:
        raise InvalidInput(f"{path} is nested too deeply") from exc


def in_range(value: Any, low: int, high: int) -> bool:
    # bool is a subclass of int; JSON true/false must not pass as numbers.
    return type(value) is int and low <= value <= high


def uint64(value: Any) -> bool:
    return in_range(value, 0, MAX_UINT64)


def validate_named_caps(section: Any, name: str, max_entries: int) -> dict[str, int]:
    if not isinstance(section, dict):
        raise InvalidInput(f"profile {name} must be an object")
    if len(section) > max_entries:
        raise InvalidInput(f"profile {name} has more than {max_entries} entries")
    for key, cap in section.items():
        if not NAME_PATTERN.fullmatch(key):
            raise InvalidInput(f"profile {name} name {key!r} must match [a-z0-9_.-]{{1,64}}")
        if not uint64(cap):
            raise InvalidInput(f"profile {name} {key!r} must be an integer in 0..2^64-1")
    return dict(section)


def validate_profile(data: Any) -> dict[str, int]:
    """Applies the same strict contract as ParseSimBudgetProfile; returns the residency caps."""
    if not isinstance(data, dict):
        raise InvalidInput("profile must be a JSON object")
    if data.get("schema") != PROFILE_SCHEMA:
        raise InvalidInput(f"profile schema must be {PROFILE_SCHEMA!r}")
    for key in data:
        if key not in PROFILE_KEYS:
            raise InvalidInput(f"unknown profile key {key!r}")
    if "region_scheduler" in data:
        scheduler = data["region_scheduler"]
        if not isinstance(scheduler, dict):
            raise InvalidInput("profile region_scheduler must be an object")
        for key in scheduler:
            if key not in SCHEDULER_KEYS:
                raise InvalidInput(f"unknown region_scheduler key {key!r}")
        if "work_limit" in scheduler and not uint64(scheduler["work_limit"]):
            raise InvalidInput("region_scheduler work_limit must be an integer in 0..2^64-1")
        if "hold_ticks" in scheduler and not in_range(scheduler["hold_ticks"], 1, MAX_UINT32):
            raise InvalidInput("region_scheduler hold_ticks must be an integer in 1..4294967295")
        if "reduced_shift" in scheduler and not in_range(
                scheduler["reduced_shift"], 1, MAX_REDUCED_SHIFT):
            raise InvalidInput("region_scheduler reduced_shift must be an integer in 1..16")
    if "stages" in data:
        validate_named_caps(data["stages"], "stages", MAX_PROFILE_STAGES)
    if "residency" in data:
        return validate_named_caps(data["residency"], "residency", MAX_PROFILE_RESIDENCY_CLASSES)
    return {}


def load_profile(path: Path) -> dict[str, int]:
    return validate_profile(load_json(path, MAX_PROFILE_BYTES))


def load_rows(path: Path) -> dict[str, dict[str, Any]]:
    data = load_json(path)
    if not isinstance(data, dict):
        raise InvalidInput("artifact must be a JSON object")
    rows = data.get("residency")
    if not isinstance(rows, list):
        return {}
    by_class: dict[str, dict[str, Any]] = {}
    for index, row in enumerate(rows):
        where = f"artifact residency[{index}]"
        if not isinstance(row, dict):
            raise InvalidInput(f"{where} must be an object")
        cls = row.get("class")
        if not isinstance(cls, str) or not cls:
            raise InvalidInput(f"{where} class must be a non-empty string")
        if cls in by_class:
            raise InvalidInput(f"duplicate residency row for class {cls!r}")
        for key in ("resident_bytes", "peak_bytes", "count"):
            if not uint64(row.get(key)):
                raise InvalidInput(f"{where} {key} must be an integer in 0..2^64-1")
        if "budget_bytes" not in row:
            raise InvalidInput(f"{where} budget_bytes is required (use null when unset)")
        budget = row["budget_bytes"]
        if budget is not None and not uint64(budget):
            raise InvalidInput(f"{where} budget_bytes must be an integer in 0..2^64-1 or null")
        for key in ("bytes_cpu", "bytes_gpu"):
            if key in row and not uint64(row[key]):
                raise InvalidInput(f"{where} {key} must be an integer in 0..2^64-1")
        if row["peak_bytes"] < row["resident_bytes"]:
            raise InvalidInput(f"{where} peak_bytes is below resident_bytes")
        if "bytes_cpu" in row and "bytes_gpu" in row:
            if row["resident_bytes"] != row["bytes_cpu"] + row["bytes_gpu"]:
                raise InvalidInput(f"{where} resident_bytes is not bytes_cpu + bytes_gpu")
        by_class[cls] = row
    return by_class


def evaluate(caps: dict[str, int], rows: dict[str, dict[str, Any]]) -> dict[str, Any]:
    classes: list[dict[str, Any]] = []
    counts = {"pass": 0, "fail": 0, "unevaluated": 0}
    for cls in sorted(caps):
        cap = caps[cls]
        row = rows.get(cls)
        if row is None:
            verdict, reason = "unevaluated", "not-instrumented"
            peak, budget = None, None
        else:
            peak, budget = row["peak_bytes"], row["budget_bytes"]
            if peak > cap:
                verdict, reason = "fail", "over-cap"
            elif budget != cap:
                # null means unlimited in the artifact, which is not the profile cap.
                verdict, reason = "fail", "budget_mismatch"
            else:
                verdict, reason = "pass", "within-cap"
        counts[verdict] += 1
        classes.append({"class": cls, "verdict": verdict, "reason": reason,
                        "peak_bytes": peak, "cap_bytes": cap, "budget_bytes": budget})
    unknown = sorted(name for name in rows if name not in caps)
    return {"schema": VERDICT_SCHEMA, "classes": classes, "unknown_classes": unknown,
            "summary": counts}


def write_atomic(path: Path, text: str) -> None:
    fd, temp_name = tempfile.mkstemp(dir=str(path.parent), prefix=path.name + ".",
                                     suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        os.replace(temp_name, path)
    except BaseException:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--artifact", type=Path, required=True,
                        help="measured JSON with a top-level residency array")
    parser.add_argument("--profile", type=Path, required=True,
                        help="luminumbra.sim_budget_profile.v1 JSON")
    parser.add_argument("--verdict-out", type=Path, default=None,
                        help="also write the verdict JSON here (atomic replace)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        caps = load_profile(args.profile)
        rows = load_rows(args.artifact)
        verdict = evaluate(caps, rows)
        text = json.dumps(verdict, sort_keys=True, separators=(",", ":"), allow_nan=False)
        if args.verdict_out is not None:
            write_atomic(args.verdict_out, text + "\n")
    except (InvalidInput, OSError, ValueError) as exc:
        sys.stderr.write(f"check_residency: {exc}\n")
        return EXIT_INVALID
    print(text, flush=True)
    summary = verdict["summary"]
    if summary["fail"]:
        return EXIT_FAIL
    if summary["unevaluated"]:
        return EXIT_UNEVALUATED
    return EXIT_PASS


if __name__ == "__main__":
    raise SystemExit(main())
