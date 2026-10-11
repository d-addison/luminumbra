#!/usr/bin/env python3
"""Check measured residency bytes against a sim-budget profile (stdlib only).

Exit codes: 0 all profile classes pass, 1 any class fails, 3 any class is unevaluated
(no failures), 2 usage or invalid input.
"""

from __future__ import annotations

import argparse
import json
import os
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


def load_json(path: Path) -> Any:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeDecodeError) as exc:
        raise InvalidInput(f"cannot read {path}: {exc}") from exc
    try:
        return json.loads(text, object_pairs_hook=reject_duplicates, parse_constant=reject_constant)
    except json.JSONDecodeError as exc:
        raise InvalidInput(f"{path} is not valid JSON: {exc}") from exc
    except RecursionError as exc:
        raise InvalidInput(f"{path} is nested too deeply") from exc


def non_negative_int(value: Any) -> bool:
    # bool is a subclass of int; JSON true/false must not pass as byte counts.
    return type(value) is int and value >= 0


def load_profile(path: Path) -> dict[str, int]:
    data = load_json(path)
    if not isinstance(data, dict):
        raise InvalidInput("profile must be a JSON object")
    if data.get("schema") != PROFILE_SCHEMA:
        raise InvalidInput(f"profile schema must be {PROFILE_SCHEMA!r}")
    residency = data.get("residency", {})
    if not isinstance(residency, dict):
        raise InvalidInput("profile residency must be an object")
    caps: dict[str, int] = {}
    for name, cap in residency.items():
        if not non_negative_int(cap):
            raise InvalidInput(f"profile residency {name!r} must be a non-negative integer")
        caps[name] = cap
    return caps


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
            if not non_negative_int(row.get(key)):
                raise InvalidInput(f"{where} {key} must be a non-negative integer")
        if "budget_bytes" not in row:
            raise InvalidInput(f"{where} budget_bytes is required (use null when unset)")
        budget = row["budget_bytes"]
        if budget is not None and not non_negative_int(budget):
            raise InvalidInput(f"{where} budget_bytes must be a non-negative integer or null")
        if row["peak_bytes"] < row["resident_bytes"]:
            raise InvalidInput(f"{where} peak_bytes is below resident_bytes")
        if "bytes_cpu" in row and "bytes_gpu" in row:
            if not non_negative_int(row["bytes_cpu"]) or not non_negative_int(row["bytes_gpu"]):
                raise InvalidInput(f"{where} bytes_cpu and bytes_gpu must be non-negative integers")
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
            entry = {"peak_bytes": None, "budget_bytes": None}
        else:
            peak, budget = row["peak_bytes"], row["budget_bytes"]
            entry = {"peak_bytes": peak, "budget_bytes": budget}
            if peak > cap:
                verdict, reason = "fail", "over-cap"
            elif budget is not None and budget != cap:
                verdict, reason = "fail", "budget_mismatch"
            else:
                verdict, reason = "pass", "within-cap"
        counts[verdict] += 1
        classes.append({"class": cls, "verdict": verdict, "reason": reason,
                        "peak_bytes": entry["peak_bytes"], "cap_bytes": cap,
                        "budget_bytes": entry["budget_bytes"]})
    unknown = sorted(name for name in rows if name not in caps)
    return {"schema": VERDICT_SCHEMA, "classes": classes, "unknown_classes": unknown,
            "summary": counts}


def write_atomic(path: Path, text: str) -> None:
    fd, temp_name = tempfile.mkstemp(dir=str(path.parent or Path(".")), prefix=path.name + ".",
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
