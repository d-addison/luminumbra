#!/usr/bin/env python3
"""Report regional admission measurements from validated captures; never launch an engine."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

# Standalone invocation supports the same isolated mode as the registered tests.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_sim_budget
from fixture_paths import read_bytes, reference_parts
from propose_sim_budgets import format_json
from resolved_fixture import unique_object
import sim_budget_compare
import sim_budget_io


DEFINITIONS = Path(__file__).with_name("fixtures") / "regional-admission-workloads.json"
# Diagnostic input bounds only: three presets, 64 MiB per capture JSON, and
# 1 MiB per sidecar/setup/receipt. No simulation operation limit is changed.
CAPTURE_BYTES = 64 * 1024 * 1024
EVIDENCE_BYTES = 1024 * 1024
BUILD_KEYS = ("compiler", "build_type", "cmake_options", "instrumentation")
HOST_KEYS = ("os", "cpu", "gpu", "driver", "power_profile", "cpu_affinity", "background_services")


def digest(value: Any) -> str:
    """Canonical diagnostic identity; object order and JSON whitespace are irrelevant."""
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True, allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def document(root: Path, reference: str, expected_hash: str | None = None) -> tuple[dict, str]:
    path = root.joinpath(*reference_parts(reference))
    raw = read_bytes(root, path, limit=EVIDENCE_BYTES)
    raw_hash = hashlib.sha256(raw).hexdigest()
    if expected_hash is not None and raw_hash != expected_hash:
        raise ValueError("evidence sha256 mismatch")
    data = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=unique_object)
    if not isinstance(data, dict):
        raise ValueError("evidence must be a JSON object")
    # Reject non-finite values even in otherwise uninterpreted setup metadata.
    digest(data)
    return data, raw_hash


def validate_pin(pin: Any) -> None:
    if (not isinstance(pin, dict) or set(pin) != {"artifact", "sha256"}
            or not isinstance(pin["sha256"], str)
            or sim_budget_io.SHA256_HEX.fullmatch(pin["sha256"]) is None):
        raise ValueError("evidence needs an artifact and sha256 pin")


def pinned_document(root: Path, pin: Any) -> tuple[dict, str]:
    validate_pin(pin)
    return document(root, pin["artifact"], pin["sha256"])


def settings(data: Any, keys: tuple[str, ...], label: str) -> dict:
    if (not isinstance(data, dict) or set(data) != set(keys)
            or any(not isinstance(data[key], str) or not data[key].strip() for key in keys)):
        raise ValueError(f"incomplete {label} settings")
    return data


def setup_identity(root: Path, sidecar: dict, workload: str) -> tuple[dict, str]:
    setup, setup_hash = pinned_document(root, sidecar.get("setup"))
    if setup.get("schema") != "luminumbra.regional_admission_setup.v1":
        raise ValueError("unsupported setup schema")
    if not isinstance(setup.get("preparation"), str) or not setup["preparation"].strip():
        raise ValueError("setup requires a preparation procedure")
    inputs = setup.get("inputs")
    # At most sixteen additional inputs, read sequentially through verified
    # descriptors with the same 64 MiB per-file diagnostic bound as captures.
    if not isinstance(inputs, list) or len(inputs) > 16:
        raise ValueError("setup requires at most sixteen pinned additional inputs")
    for pin in inputs:
        validate_pin(pin)
        raw = read_bytes(root, root.joinpath(*reference_parts(pin["artifact"])), limit=CAPTURE_BYTES)
        if hashlib.sha256(raw).hexdigest() != pin["sha256"]:
            raise ValueError("setup input sha256 mismatch")
    for name in ("active_regions", "frozen_history_records", "boundary_transitions"):
        if not capture_sim_budget.integer(setup.get(name)):
            raise ValueError(f"invalid setup {name}")
    cadences = setup.get("cadence_ticks")
    if (setup["active_regions"] == 0 or not isinstance(cadences, list) or not cadences
            or any(not capture_sim_budget.integer(value) or value == 0 for value in cadences)
            or len(set(cadences)) != len(cadences)):
        raise ValueError("invalid setup regions or cadences")
    if workload == "mixed-cadence-boundary" and (
            len(cadences) < 2 or setup["boundary_transitions"] == 0):
        raise ValueError("mixed workload requires distinct cadences and boundary transitions")
    if workload == "growing-frozen-history" and setup["frozen_history_records"] == 0:
        raise ValueError("history workload requires frozen history")
    return setup, setup_hash


def path_coverage(root: Path, pin: Any, identity: str, capture_hash: str, artifact: dict,
                  paths: list[str]) -> dict:
    coverage = {name: {"status": "unevaluated", "observations": 0} for name in paths}
    if pin is None:
        return coverage
    receipt, receipt_hash = pinned_document(root, pin)
    if (receipt.get("schema") != "luminumbra.regional_admission_paths.v1"
            or receipt.get("workload_sha256") != identity
            or receipt.get("capture_sha256") != capture_hash):
        raise ValueError("path receipt schema or capture/workload identity mismatch")
    observations = receipt.get("observations")
    if not isinstance(observations, list):
        raise ValueError("path receipt lacks observations")
    seen = set()
    for observation in observations:
        if not isinstance(observation, dict) or set(observation) != {"path", "tick", "count"}:
            raise ValueError("malformed path observation")
        name, tick, count = (observation[key] for key in ("path", "tick", "count"))
        if (not isinstance(name, str) or name not in coverage
                or not capture_sim_budget.integer(tick) or not 1 <= tick <= artifact["ticks_requested"]
                or not capture_sim_budget.integer(count) or (name, tick) in seen):
            raise ValueError("unknown, duplicate or out-of-range path observation")
        seen.add((name, tick))
        coverage[name]["observations"] += count
    for row in coverage.values():
        row["receipt_sha256"] = receipt_hash
        if row["observations"] > 0:
            row["status"] = "exercised"
    return coverage


def load_report(directory: Path, evidence: str = "regional-admission.json") -> dict:
    capture = sim_budget_io.load_capture(directory, include_artifacts=True,
                                         limit=CAPTURE_BYTES, max_runs=3)
    manifest = capture["manifest"]
    if (set(capture["presets"]) != set(capture_sim_budget.PRESETS)
            or "preset_hash" not in manifest):
        raise ValueError("report requires all shipping presets and preset_hash")
    sidecar, _ = document(directory, evidence)
    definitions, _ = document(DEFINITIONS.parent, DEFINITIONS.name)
    if (sidecar.get("schema") != "luminumbra.regional_admission_evidence.v1"
            or definitions.get("schema") != "luminumbra.regional_admission_workloads.v1"):
        raise ValueError("unsupported regional admission schema")
    workload = sidecar.get("workload")
    if not isinstance(workload, str) or workload not in definitions["workloads"]:
        raise ValueError("unknown regional admission workload")
    identity = {key: manifest[key] for key in sim_budget_compare.WORKLOAD_KEYS}
    for key in ("seed", "ticks"):
        identity[key] = sim_budget_io.validate_workload_integer(identity[key], key)
    setup, setup_hash = setup_identity(directory, sidecar, workload)
    identity.update({
        "workload": workload, "definition_sha256": digest(definitions),
        "setup_sha256": setup_hash,
        "build_settings": settings(sidecar.get("build"), BUILD_KEYS, "build"),
        "host_settings": settings(sidecar.get("host"), HOST_KEYS, "host"),
    })
    workload_hash = digest(identity)
    receipts = sidecar.get("path_evidence", {})
    if not isinstance(receipts, dict) or set(receipts) - set(capture["presets"]):
        raise ValueError("unknown preset in path evidence")
    build = {key: manifest[key] for key in ("revision", "source_dirty", "binary_sha256")}
    presets = {}
    for preset, data in capture["presets"].items():
        artifact = data["artifact"]
        artifact_hash = digest(artifact)
        capture_hash = digest({"artifact_sha256": artifact_hash, "build": build})
        if preset in receipts and receipts[preset] is None:
            raise ValueError("present path evidence must have an artifact and sha256 pin")
        coverage = path_coverage(directory, receipts.get(preset), workload_hash, capture_hash,
                                 artifact, definitions["paths"])
        presets[preset] = {
            "capture_sha256": capture_hash, "artifact_sha256": artifact_hash,
            "world_hash": artifact["world_hash"],
            "world_hash_replay": artifact["world_hash_replay"],
            "stages": artifact["sim_budget"]["stages"],
            "work_items": artifact["sim_budget"].get("work_items"),
            "paths": coverage,
            "coverage": "complete" if all(coverage[name]["status"] == "exercised"
                for name in definitions["workloads"][workload]["required_paths"]) else "unevaluated",
        }
    return {
        "schema": "luminumbra.regional_admission_report.v1",
        "identity": identity, "workload_sha256": workload_hash,
        "setup": setup,
        "build": build,
        "timings": "observational only; no performance verdict",
        "operation_limits": "unchanged; use check_sim_budgets.py --require-complete separately",
        "presets": presets,
    }


def compare_reports(before: dict, after: dict) -> dict:
    if before["identity"] != after["identity"]:
        raise ValueError("reports differ in workload, setup, build settings or host settings")

    def comparable(report: dict) -> dict:
        return {"manifest": report["identity"], "presets": {
            name: {"stages": sim_budget_io.summarize_stages(data["stages"])}
            for name, data in report["presets"].items()}}

    comparison, _ = sim_budget_compare.build_comparison(comparable(before), comparable(after), set())
    comparison["schema"] = "luminumbra.regional_admission_compare.v1"
    for preset, rows in comparison["presets"].items():
        old, new = before["presets"][preset], after["presets"][preset]
        rows["work_items"] = {
            "work": "unevaluated" if old["work_items"] is None or new["work_items"] is None
                    else "same" if old["work_items"] == new["work_items"] else "different"}
        rows["world_hash"] = "same" if old["world_hash"] == new["world_hash"] else "different"
    return comparison


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("--evidence", default="regional-admission.json",
                        help="root-relative evidence sidecar within each capture directory")
    parser.add_argument("--compare", type=Path, help="second capture with the same workload")
    args = parser.parse_args(argv)
    try:
        before = load_report(args.capture_dir, args.evidence)
        result = before
        if args.compare is not None:
            after = load_report(args.compare, args.evidence)
            result = {"before": before, "after": after, "comparison": compare_reports(before, after)}
        sys.stdout.write(format_json(result))
    except (OSError, ValueError, TypeError, KeyError, AttributeError, RecursionError) as exc:
        print(f"Regional admission report refused: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
