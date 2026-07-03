#!/usr/bin/env python3
"""Spec 021 backlog validator (AC-002).

Validates docs/audit/021/backlog.json against the FR-C-003 item schema.
The load-bearing rule: EVERY item must carry a non-empty, non-vague
proving_signal (per test/features/TDD-LOCK.md). An item without one is
invalid and this script exits 1.

Usage: python validate_backlog.py [path-to-backlog.json]
Exit codes: 0 = valid, 1 = one or more violations, 2 = unreadable input.
"""
import json
import re
import sys
from pathlib import Path

REQUIRED_FIELDS = [
    "id", "pillar", "spec", "summary", "evidence", "effort", "risk",
    "dependencies", "status", "proving_signal",
]
EFFORT_ENUM = {"S", "M", "L", "XL"}
RISK_ENUM = {"low", "medium", "high"}
STATUS_ENUM = {"todo", "in-progress", "done"}
PILLAR_ENUM = {
    "shield", "instinct", "atmospheric", "aetheric", "render", "gpu",
    "foliage", "water", "networking", "audio", "ui", "buildtestops",
}
# Vague placeholders that do NOT count as a proving signal.
VAGUE_SIGNALS = {"", "tbd", "todo", "n/a", "none", "tests", "add tests", "add a test"}
# A dependency may be another item id, or a spec reference like
# "014", "017-B", "018-E/F", "016 FR-D", "015 A-T06".
SPEC_DEP_RE = re.compile(r"^\d{3}([-/ ][A-Za-z0-9/.\- ]+)?$")


def validate(path: Path) -> int:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"FATAL: cannot read/parse {path}: {exc}")
        return 2

    items = data.get("items")
    if not isinstance(items, list) or not items:
        print("FAIL: top-level 'items' must be a non-empty array")
        return 1

    errors = []
    warnings = []
    seen_ids = set()
    all_ids = {it.get("id") for it in items if isinstance(it, dict)}

    for idx, item in enumerate(items):
        tag = item.get("id", f"<index {idx}>") if isinstance(item, dict) else f"<index {idx}>"
        if not isinstance(item, dict):
            errors.append(f"{tag}: item is not an object")
            continue
        for field in REQUIRED_FIELDS:
            if field not in item:
                errors.append(f"{tag}: missing required field '{field}'")
        for field in ("id", "pillar", "spec", "summary", "evidence"):
            if field in item and (not isinstance(item[field], str) or not item[field].strip()):
                errors.append(f"{tag}: field '{field}' must be a non-empty string")
        if item.get("id") in seen_ids:
            errors.append(f"{tag}: duplicate id")
        seen_ids.add(item.get("id"))
        if item.get("effort") not in EFFORT_ENUM:
            errors.append(f"{tag}: effort '{item.get('effort')}' not in {sorted(EFFORT_ENUM)}")
        if item.get("risk") not in RISK_ENUM:
            errors.append(f"{tag}: risk '{item.get('risk')}' not in {sorted(RISK_ENUM)}")
        if item.get("status") not in STATUS_ENUM:
            errors.append(f"{tag}: status '{item.get('status')}' not in {sorted(STATUS_ENUM)}")
        if item.get("pillar") not in PILLAR_ENUM:
            errors.append(f"{tag}: pillar '{item.get('pillar')}' not in the 021 pillar set")

        # THE AC-002 GATE: non-empty, non-vague proving_signal.
        signal = item.get("proving_signal")
        if not isinstance(signal, str) or signal.strip().lower() in VAGUE_SIGNALS:
            errors.append(
                f"{tag}: INVALID proving_signal ({signal!r}) — every item MUST carry a "
                "concrete proving signal per test/features/TDD-LOCK.md (FR-C-002)"
            )

        # NFR-002: the ranked order is mechanically derivable — every item carries a rank.
        rank = item.get("rank")
        if not isinstance(rank, int) or rank < 1:
            errors.append(f"{tag}: missing/invalid 'rank' (got {rank!r}); the ranking must be derivable from this file")

        deps = item.get("dependencies")
        if not isinstance(deps, list):
            errors.append(f"{tag}: dependencies must be an array")
        else:
            for dep in deps:
                if not isinstance(dep, str) or not dep.strip():
                    errors.append(f"{tag}: empty dependency entry")
                elif dep not in all_ids and not SPEC_DEP_RE.match(dep):
                    warnings.append(
                        f"{tag}: dependency '{dep}' is neither a known item id nor a spec reference"
                    )

    for warning in warnings:
        print(f"WARN: {warning}")
    if errors:
        for error in errors:
            print(f"FAIL: {error}")
        print(f"\n{len(errors)} violation(s) across {len(items)} items -> INVALID")
        return 1
    print(f"OK: {len(items)} items valid; every item carries a proving_signal (AC-002).")
    return 0


if __name__ == "__main__":
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "backlog.json"
    sys.exit(validate(target))
