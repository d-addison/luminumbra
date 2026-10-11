#!/usr/bin/env python3
"""Read sim budget capture directories and derive integer per-tick work figures."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

import capture_sim_budget


CAPTURE_SCHEMA = "luminumbra.sim_budget_capture.v1"
MANIFEST_KEYS = (
    "revision", "source_dirty", "binary_sha256", "fixture_hash",
    "seed", "ticks", "surface_radius", "collision_radius",
)
SHA256_HEX = re.compile(r"[0-9a-f]{64}")


def read_json(path: Path) -> Any:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise ValueError(f"cannot read {path.name}: {exc.strerror or exc}") from exc
    try:
        return json.loads(text)
    except ValueError as exc:
        raise ValueError(f"invalid JSON in {path.name}: {exc}") from exc


def summarize_stages(stages: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    summary: dict[str, dict[str, Any]] = {}
    for stage in stages:
        samples = stage["samples"]
        trace = [sample["work"] for sample in stage["work_trace"]] if samples > 0 else []
        duration = stage.get("duration_ms")
        p99 = float(duration["p99"]) if isinstance(duration, dict) else None
        summary[stage["name"]] = {
            "samples": samples, "trace": trace,
            "work_total": stage["work_total"], "p99_ms": p99,
        }
    return summary


def validate_manifest(manifest: dict[str, Any]) -> None:
    """Check the manifest keys capture_sim_budget.py writes; refuse null or mistyped values."""
    revision = manifest["revision"]
    if not isinstance(revision, str) or not revision:
        raise ValueError("capture.json has an invalid revision")
    if type(manifest["source_dirty"]) is not bool:
        raise ValueError("capture.json has an invalid source_dirty flag")
    digest = manifest["binary_sha256"]
    if not isinstance(digest, str) or SHA256_HEX.fullmatch(digest) is None:
        raise ValueError("capture.json has an invalid binary_sha256")
    fixture = manifest["fixture_hash"]
    if not isinstance(fixture, str) or not fixture:
        raise ValueError("capture.json has an invalid fixture_hash")
    for key in ("surface_radius", "collision_radius"):
        value = manifest[key]
        if type(value) is not int or value <= 0:
            raise ValueError(f"capture.json has an invalid {key}")


def load_capture(directory: Path) -> dict[str, Any]:
    manifest = read_json(directory / "capture.json")
    if not isinstance(manifest, dict) or manifest.get("schema") != CAPTURE_SCHEMA:
        raise ValueError("capture.json has a missing or unsupported schema")
    absent = [key for key in MANIFEST_KEYS if key not in manifest]
    if absent:
        raise ValueError(f"capture.json lacks {', '.join(absent)}")
    ticks = manifest["ticks"]
    if type(ticks) is not int or ticks <= 0 or not isinstance(manifest["seed"], str):
        raise ValueError("capture.json has an invalid seed or tick count")
    validate_manifest(manifest)
    runs = manifest.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("capture.json lists no runs")
    presets: dict[str, dict[str, Any]] = {}
    for run in runs:
        if (not isinstance(run, dict) or not isinstance(run.get("preset"), str)
                or not isinstance(run.get("artifact"), str)):
            raise ValueError("capture.json has a malformed run entry")
        preset, artifact = run["preset"], run["artifact"]
        if preset in presets:
            raise ValueError(f"duplicate preset in capture.json: {preset}")
        data = read_json(directory / artifact)
        if not isinstance(data, dict):
            raise ValueError(f"{artifact} is not a JSON object")
        try:
            capture_sim_budget.validate_artifact(data, preset, manifest["seed"], ticks)
        except (TypeError, KeyError, AttributeError) as exc:
            raise ValueError(f"{artifact} is malformed: {exc}") from exc
        presets[preset] = {"stages": summarize_stages(data["sim_budget"]["stages"])}
    return {"manifest": manifest, "presets": presets}


def per_tick_totals(stages: dict[str, dict[str, Any]]) -> list[int]:
    active = [stage["trace"] for stage in stages.values() if stage["samples"] > 0]
    if not active:
        return []
    length = len(active[0])
    if any(len(trace) != length for trace in active):
        raise ValueError("stage traces have different lengths")
    return [sum(values) for values in zip(*active)]


def headroom(value: int, num: int, den: int) -> int:
    if type(value) is not int or type(num) is not int or type(den) is not int:
        raise ValueError("headroom arithmetic is integer only")
    if num <= 0 or den <= 0:
        raise ValueError("headroom ratio must be positive")
    return (value * num + den - 1) // den


def stage_max(stage: dict[str, Any]) -> int:
    return max(stage["trace"], default=0)
