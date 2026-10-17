"""Shared resolved-input identity for server comparison and simulation budgets."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any

from fixture_hash import SERVER_RENDER_ONLY_FILES, canonical_file_digest


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
    """Hash headless content, selected presets and their resolved external inputs.

    Match TerrainPresetLoader's data-root resolution. Biome tables are the only
    authored external-file reference in that loader; structure pools and species
    definitions are inline documents beneath data/common. Follow directory links
    too, because the engine's directory iterators consume their contents.
    """
    inputs: dict[str, str] = {}

    def add_file(label: str, path: Path) -> None:
        inputs[label] = canonical_file_digest(path) if path.is_file() else "missing"

    def add_common(directory: Path, relative: Path, ancestors: frozenset[Path]) -> None:
        resolved = directory.resolve()
        if resolved in ancestors:
            raise ValueError("cyclic simulation fixture directory")
        for child in sorted(directory.iterdir()):
            name = relative / child.name
            if child.is_dir():
                add_common(child, name, ancestors | {resolved})
            elif child.is_file() and name.as_posix() not in SERVER_RENDER_ONLY_FILES:
                add_file("data/common/" + name.as_posix(), child)

    common = root / "data/common"
    if common.is_dir():
        add_common(common, Path(), frozenset())
    for preset in PRESETS:
        path = root / "worlds/atlas/presets" / f"{preset}.json"
        document = json.loads(path.read_text(encoding="utf-8-sig"),
                              object_pairs_hook=unique_object)
        add_file(f"preset/{preset}", path)
        biomes = document.get("generation_params", {}).get("biomes", {})
        # The native loader treats a non-object optional block as absent.
        if not isinstance(biomes, dict):
            continue
        table = biomes.get("table", "")
        if not isinstance(table, str):
            raise ValueError("preset biome table must be a path string")
        if table:
            # Absolute and parent-relative references have the same resolution
            # as the native loader. Identity uses content, never the clone path.
            data_root = path.parent.parent.parent.parent / "data"
            # Cancel path components before probing, even when they do not exist.
            table_path = Path(os.path.normpath(data_root / table))
            add_file(f"preset/{preset}/biomes", table_path)
    marker = root / CONTROLLED_MARKER
    if marker.exists():
        # Marker presence enables runtime overrides even with identical data.
        # Leave it absent from shipping inputs to preserve their existing hashes.
        inputs["configuration"] = marker.read_text(encoding="utf-8")
    encoded = json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(b"luminumbra.sim_budget.inputs.v2\0" + encoded).hexdigest()
