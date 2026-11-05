"""Shared resolved-input identity for server comparison and simulation budgets."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

from fixture_hash import fixture_hash
from fixture_paths import path_exists, read_text, resolve_reference


PRESETS = ("default", "mountains", "archipelago")
CONTROLLED_MARKER = ".perf-controlled"


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def resolved_fixture_hash(root: Path) -> str:
    """Bind canonical headless data, presets, dependencies and runtime overrides."""
    data = resolve_reference(root, "data").resolve()
    presets = resolve_reference(root, "worlds/atlas/presets")
    inputs = {
        "common": fixture_hash(resolve_reference(root, "data/common"), scope="headless-server"),
        "presets": fixture_hash(presets, reference_root=data),
    }
    # Selected presets must load successfully before any executable is launched.
    for preset in PRESETS:
        path = resolve_reference(presets, f"{preset}.json")
        document = json.loads(read_text(presets, path, encoding="utf-8-sig"),
                              object_pairs_hook=unique_object)
        if not isinstance(document, dict):
            raise ValueError("preset must be a JSON object")
        generation = document.get("generation_params")
        biomes = generation.get("biomes") if isinstance(generation, dict) else None
        # The native loader treats a non-object optional block as absent.
        if isinstance(biomes, dict) and not isinstance(biomes.get("table", ""), str):
            raise ValueError("preset biome table must be a path string")
    marker = resolve_reference(root, CONTROLLED_MARKER)
    if path_exists(marker):
        inputs["configuration"] = read_text(root.resolve(), marker)
    encoded = json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(b"luminumbra.sim_budget.inputs.v2\0" + encoded).hexdigest()
