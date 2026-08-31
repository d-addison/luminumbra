"""Blender-independent logic for deterministic geometry-nodes baking."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


ParameterValue = bool | int | float | str | list[bool | int | float | str]


class BakeConfigurationError(ValueError):
    """Raised when bake arguments or parameter data violate the contract."""


@dataclass(frozen=True)
class BakeRequest:
    """Validated inputs needed by the Blender-facing bake layer."""

    object_name: str
    output_path: Path
    manifest_path: Path
    frame: int
    seed: int
    parameters: dict[str, ParameterValue]


def arguments_after_separator(argv: Sequence[str]) -> list[str]:
    """Return arguments following Blender's ``--`` script separator."""

    try:
        separator = argv.index("--")
    except ValueError:
        return []
    return list(argv[separator + 1 :])


def _reject_json_constant(value: str) -> None:
    raise BakeConfigurationError(f"non-finite JSON number is not allowed: {value}")


def _validate_parameter_value(name: str, value: Any) -> ParameterValue:
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        if not -(2**31) <= value < 2**31:
            raise BakeConfigurationError(f"parameter {name!r} is outside the signed integer range")
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise BakeConfigurationError(f"parameter {name!r} must be finite")
        return value
    if isinstance(value, str):
        if "\x00" in value:
            raise BakeConfigurationError(f"parameter {name!r} contains a null character")
        return value
    if isinstance(value, list):
        if not 2 <= len(value) <= 4:
            raise BakeConfigurationError(
                f"parameter {name!r} arrays must contain two to four scalar values"
            )
        validated = [_validate_parameter_value(name, item) for item in value]
        if any(isinstance(item, (str, list)) for item in validated):
            raise BakeConfigurationError(f"parameter {name!r} arrays must contain numbers or booleans")
        return validated
    raise BakeConfigurationError(
        f"parameter {name!r} must be a boolean, integer, finite number, string, or short scalar array"
    )


def validate_parameters(parameters: Mapping[str, Any]) -> dict[str, ParameterValue]:
    """Validate and canonically order a geometry-nodes parameter mapping."""

    validated: dict[str, ParameterValue] = {}
    folded_names: dict[str, str] = {}
    for raw_name, value in parameters.items():
        if not isinstance(raw_name, str):
            raise BakeConfigurationError("parameter names must be strings")
        name = raw_name.strip()
        if not name or any(ord(character) < 32 for character in name):
            raise BakeConfigurationError("parameter names must be non-empty printable strings")
        folded = name.casefold()
        if folded in folded_names:
            raise BakeConfigurationError(
                f"parameter names {folded_names[folded]!r} and {name!r} differ only by case"
            )
        folded_names[folded] = name
        validated[name] = _validate_parameter_value(name, value)
    return {name: validated[name] for name in sorted(validated, key=lambda item: (item.casefold(), item))}


def parse_parameters_json(text: str) -> dict[str, ParameterValue]:
    """Parse a JSON object containing node-group input values."""

    try:
        value = json.loads(text, parse_constant=_reject_json_constant)
    except (json.JSONDecodeError, TypeError) as error:
        raise BakeConfigurationError(f"invalid parameter JSON: {error}") from error
    if not isinstance(value, dict):
        raise BakeConfigurationError("parameter JSON must contain an object")
    return validate_parameters(value)


def merge_parameters(*parameter_sets: Mapping[str, Any]) -> dict[str, ParameterValue]:
    """Merge parameter sets with later sets overriding names case-insensitively."""

    merged: dict[str, Any] = {}
    names_by_fold: dict[str, str] = {}
    for parameter_set in parameter_sets:
        for raw_name, value in parameter_set.items():
            if not isinstance(raw_name, str):
                raise BakeConfigurationError("parameter names must be strings")
            folded = raw_name.strip().casefold()
            previous = names_by_fold.get(folded)
            if previous is not None:
                del merged[previous]
            merged[raw_name] = value
            names_by_fold[folded] = raw_name
    return validate_parameters(merged)


def _parameter_source(text_or_path: str) -> dict[str, ParameterValue]:
    candidate = text_or_path[1:] if text_or_path.startswith("@") else text_or_path
    path = Path(candidate)
    should_read = text_or_path.startswith("@") or (
        not text_or_path.lstrip().startswith("{") and path.is_file()
    )
    if should_read:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            raise BakeConfigurationError(f"cannot read parameter file {path}: {error}") from error
        return parse_parameters_json(text)
    return parse_parameters_json(text_or_path)


def _single_parameter(item: str) -> dict[str, ParameterValue]:
    if "=" not in item:
        raise BakeConfigurationError(f"parameter override must use NAME=JSON_VALUE: {item!r}")
    name, raw_value = item.split("=", 1)
    try:
        value = json.loads(raw_value, parse_constant=_reject_json_constant)
    except (json.JSONDecodeError, TypeError) as error:
        raise BakeConfigurationError(f"invalid JSON value for parameter {name!r}: {error}") from error
    return validate_parameters({name: value})


def _portable_stem(value: str) -> str:
    ascii_value = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode("ascii")
    stem = re.sub(r"[^A-Za-z0-9_-]+", "-", ascii_value).strip("-_").lower()
    return stem or "bake"


def default_output_name(object_name: str, seed: int) -> str:
    """Build a portable deterministic GLB name for an object and seed."""

    seed_text = f"neg{abs(seed)}" if seed < 0 else str(seed)
    return f"{_portable_stem(object_name)}-seed-{seed_text}.glb"


def normalize_output_path(output: str | os.PathLike[str] | None, object_name: str, seed: int) -> Path:
    """Normalize an explicit output path or construct the deterministic default."""

    if output is None:
        return Path(default_output_name(object_name, seed))
    path = Path(output)
    if path.name in ("", ".", ".."):
        raise BakeConfigurationError("output must name a GLB file")
    if path.suffix and path.suffix.lower() != ".glb":
        raise BakeConfigurationError("output path must end in .glb")
    if not path.suffix:
        path = path.with_suffix(".glb")
    elif path.suffix != ".glb":
        path = path.with_suffix(".glb")
    return path


def default_manifest_path(output_path: Path) -> Path:
    """Return the result-manifest path paired with an output GLB."""

    return output_path.with_name(f"{output_path.stem}.bake.json")


def build_export_settings(output_path: str | os.PathLike[str]) -> dict[str, Any]:
    """Construct the pinned restricted-GLB exporter arguments."""

    return {
        "filepath": os.fspath(output_path),
        "check_existing": False,
        "use_selection": True,
        "export_format": "GLB",
        "export_yup": True,
        "export_texcoords": True,
        "export_normals": True,
        "export_skins": True,
        "export_influence_nb": 4,
        "export_all_influences": False,
        "export_animations": True,
        "export_force_sampling": True,
        "export_morph": False,
        "export_morph_animation": False,
        "export_cameras": False,
        "export_lights": False,
        "export_draco_mesh_compression_enable": False,
        "export_meshopt_compression_enable": False,
        "export_use_gltfpack": False,
        "export_apply": True,
        "export_extras": False,
    }


def build_result_manifest(
    *,
    source_blend: str,
    object_name: str,
    output_path: str,
    frame: int,
    seed: int,
    parameters: Mapping[str, Any],
    blender_version: str,
    vertex_count: int,
    triangle_count: int,
    material_count: int,
    content_sha256: str,
    validation: Mapping[str, Any],
) -> dict[str, Any]:
    """Construct a deterministic, JSON-compatible bake result manifest."""

    canonical_parameters = validate_parameters(parameters)
    if len(content_sha256) != 64 or any(character not in "0123456789abcdef" for character in content_sha256):
        raise BakeConfigurationError("content hash must be a lowercase SHA-256 digest")
    for label, count in (
        ("vertex", vertex_count),
        ("triangle", triangle_count),
        ("material", material_count),
    ):
        if not isinstance(count, int) or isinstance(count, bool) or count < 0:
            raise BakeConfigurationError(f"{label} count must be a non-negative integer")
    return {
        "schema": "geonodes-bake-result",
        "source_blend": source_blend,
        "object": object_name,
        "output": output_path,
        "frame": frame,
        "seed": seed,
        "parameters": canonical_parameters,
        "blender_version": blender_version,
        "export_profile": "restricted-glb",
        "geometry": {
            "vertices": vertex_count,
            "triangles": triangle_count,
            "materials": material_count,
        },
        "sha256": content_sha256,
        "validation": dict(validation),
    }


def manifest_json(manifest: Mapping[str, Any]) -> str:
    """Serialize a manifest with stable key ordering and a trailing newline."""

    return json.dumps(manifest, indent=2, sort_keys=True, allow_nan=False) + "\n"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="geonodes_bake.py",
        description="Evaluate a named object's geometry-nodes stack and export a restricted GLB.",
    )
    parser.add_argument("--object", dest="object_name", required=True, help="object to evaluate")
    parser.add_argument("--output", help="output GLB path; defaults to an object-and-seed name")
    parser.add_argument("--manifest", help="result JSON path; defaults beside the output GLB")
    parser.add_argument("--frame", type=int, default=1, help="explicit evaluation frame")
    parser.add_argument("--seed", type=int, help="integer seed, required here or in parameter JSON")
    parser.add_argument(
        "--params-json",
        action="append",
        default=[],
        metavar="JSON_OR_FILE",
        help="JSON object, file path, or @file containing node input values",
    )
    parser.add_argument(
        "--param",
        action="append",
        default=[],
        metavar="NAME=JSON_VALUE",
        help="repeatable node input override",
    )
    return parser


def parse_cli_args(argv: Iterable[str]) -> BakeRequest:
    """Parse Blender-script arguments into a validated bake request."""

    namespace = _parser().parse_args(list(argv))
    if not namespace.object_name.strip():
        raise BakeConfigurationError("object name must not be empty")

    parameter_sets: list[Mapping[str, Any]] = []
    parameter_sets.extend(_parameter_source(source) for source in namespace.params_json)
    parameter_sets.extend(_single_parameter(item) for item in namespace.param)
    parameters = merge_parameters(*parameter_sets)

    seed_key = next((name for name in parameters if name.casefold() == "seed"), None)
    json_seed = parameters.get(seed_key) if seed_key is not None else None
    if json_seed is not None and (not isinstance(json_seed, int) or isinstance(json_seed, bool)):
        raise BakeConfigurationError("seed parameter must be an integer")
    if namespace.seed is None and json_seed is None:
        raise BakeConfigurationError("an explicit integer seed is required")
    if namespace.seed is not None and json_seed is not None and namespace.seed != json_seed:
        raise BakeConfigurationError("--seed conflicts with the seed in the parameter set")
    seed = namespace.seed if namespace.seed is not None else json_seed
    assert isinstance(seed, int)
    _validate_parameter_value("seed", seed)

    if seed_key is not None:
        parameters = {name: value for name, value in parameters.items() if name != seed_key}
    parameters = merge_parameters(parameters, {"seed": seed})

    output_path = normalize_output_path(namespace.output, namespace.object_name, seed)
    manifest_path = Path(namespace.manifest) if namespace.manifest else default_manifest_path(output_path)
    if manifest_path.suffix.lower() != ".json":
        raise BakeConfigurationError("manifest path must end in .json")
    return BakeRequest(
        object_name=namespace.object_name,
        output_path=output_path,
        manifest_path=manifest_path,
        frame=namespace.frame,
        seed=seed,
        parameters=parameters,
    )
