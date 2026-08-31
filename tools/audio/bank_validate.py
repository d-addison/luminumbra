#!/usr/bin/env python3
"""Validate luminumbra audio bank manifests and their referenced assets."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import wave
from dataclasses import asdict, dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Optional, Sequence


DEFAULT_MANIFESTS = (
    "data/audio/music.bank.json",
    "data/audio/sfx_main.bank.json",
)
AUDIO_SUFFIXES = {".mp3", ".wav"}
ROOT_FIELDS = {"bank_id", "streaming", "events"}
EVENT_FIELDS = {
    "files",
    "volume",
    "pitch_variation",
    "strategy",
    "is_2d",
    "is_3d",
    "looping",
    "attenuation_model",
    "min_distance",
    "max_distance",
}


class JSONObject(list):
    """A JSON object represented as ordered pairs so duplicate keys survive parsing."""


@dataclass
class Finding:
    severity: str
    code: str
    message: str
    file: str
    line: int
    bank: Optional[str] = None
    entry_id: Optional[str] = None
    asset_path: Optional[str] = None


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    manifests: int = 0
    entries: int = 0
    referenced_assets: int = 0
    assets: int = 0

    @property
    def valid(self) -> bool:
        return not self.findings

    def to_dict(self) -> dict[str, Any]:
        return {
            "valid": self.valid,
            "summary": {
                "manifests": self.manifests,
                "entries": self.entries,
                "referenced_assets": self.referenced_assets,
                "assets": self.assets,
                "findings": len(self.findings),
            },
            "findings": [asdict(item) for item in self.findings],
        }


class Locator:
    """Best-effort source line lookup for keys and string values in JSON text."""

    def __init__(self, text: str) -> None:
        self._lines = text.splitlines() or [""]
        self._key_uses: dict[str, int] = {}
        self._value_uses: dict[str, int] = {}

    def key(self, value: str) -> int:
        encoded = json.dumps(value, ensure_ascii=False)
        pattern = re.compile(re.escape(encoded) + r"\s*:")
        matches = [number for number, line in enumerate(self._lines, 1) if pattern.search(line)]
        used = self._key_uses.get(value, 0)
        self._key_uses[value] = used + 1
        if not matches:
            return 1
        return matches[min(used, len(matches) - 1)]

    def string(self, value: str) -> int:
        encoded = json.dumps(value, ensure_ascii=False)
        matches = [number for number, line in enumerate(self._lines, 1) if encoded in line]
        used = self._value_uses.get(value, 0)
        self._value_uses[value] = used + 1
        if not matches:
            return 1
        return matches[min(used, len(matches) - 1)]


@dataclass
class AssetReference:
    raw_path: str
    resolved_path: Optional[Path]
    manifest_file: str
    line: int
    bank: Optional[str]
    entry_id: str


def _display_path(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except (OSError, ValueError):
        return str(path)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _finding(
    report: Report,
    *,
    code: str,
    message: str,
    file: str,
    line: int = 1,
    severity: str = "error",
    bank: Optional[str] = None,
    entry_id: Optional[str] = None,
    asset_path: Optional[str] = None,
) -> None:
    report.findings.append(
        Finding(
            severity=severity,
            code=code,
            message=message,
            file=file,
            line=max(1, line),
            bank=bank,
            entry_id=entry_id,
            asset_path=asset_path,
        )
    )


def _validate_scalar_field(
    report: Report,
    manifest_file: str,
    bank: Optional[str],
    entry_id: Optional[str],
    name: str,
    value: Any,
    line: int,
) -> None:
    context = f"event '{entry_id}' field '{name}'" if entry_id else f"field '{name}'"
    if name in {"streaming", "is_2d", "is_3d", "looping"}:
        if not isinstance(value, bool):
            _finding(
                report,
                code="schema-type",
                message=f"{context} must be a boolean",
                file=manifest_file,
                line=line,
                bank=bank,
                entry_id=entry_id,
            )
        return

    if name in {"volume", "pitch_variation", "min_distance", "max_distance"}:
        if not _is_number(value):
            _finding(
                report,
                code="schema-type",
                message=f"{context} must be a finite number",
                file=manifest_file,
                line=line,
                bank=bank,
                entry_id=entry_id,
            )
        elif value < 0:
            _finding(
                report,
                code="schema-value",
                message=f"{context} must not be negative",
                file=manifest_file,
                line=line,
                bank=bank,
                entry_id=entry_id,
            )
        return

    if name in {"strategy", "attenuation_model"} and (
        not isinstance(value, str) or not value.strip()
    ):
        _finding(
            report,
            code="schema-type",
            message=f"{context} must be a non-empty string",
            file=manifest_file,
            line=line,
            bank=bank,
            entry_id=entry_id,
        )


def _resolve_asset(raw_path: str, assets_dir: Path) -> Optional[Path]:
    posix_path = PurePosixPath(raw_path.replace("\\", "/"))
    if posix_path.is_absolute() or ".." in posix_path.parts:
        return None
    expected_prefix = PurePosixPath("assets/audio")
    try:
        relative_asset = posix_path.relative_to(expected_prefix)
    except ValueError:
        return None
    candidate = (assets_dir / Path(*relative_asset.parts)).resolve()
    try:
        candidate.relative_to(assets_dir.resolve())
    except ValueError:
        return None
    if not relative_asset.parts:
        return None
    return candidate


def _parse_manifest(
    path: Path,
    root: Path,
    assets_dir: Path,
    report: Report,
    global_event_ids: dict[str, tuple[str, int, Optional[str]]],
    global_bank_ids: dict[str, tuple[str, int]],
) -> list[AssetReference]:
    manifest_file = _display_path(path, root)
    report.manifests += 1
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        _finding(
            report,
            code="manifest-open",
            message=f"could not open manifest: {exc}",
            file=manifest_file,
        )
        return []

    locator = Locator(text)
    try:
        root_obj = json.loads(text, object_pairs_hook=JSONObject)
    except (json.JSONDecodeError, UnicodeError) as exc:
        line = getattr(exc, "lineno", 1)
        _finding(
            report,
            code="invalid-json",
            message=f"invalid JSON: {exc}",
            file=manifest_file,
            line=line,
        )
        return []

    if not isinstance(root_obj, JSONObject):
        _finding(
            report,
            code="schema-root",
            message="manifest root must be an object",
            file=manifest_file,
        )
        return []

    root_values: dict[str, Any] = {}
    seen_root_fields: set[str] = set()
    for name, value in root_obj:
        line = locator.key(name)
        if name in seen_root_fields:
            _finding(
                report,
                code="duplicate-field",
                message=f"duplicate root field '{name}'",
                file=manifest_file,
                line=line,
            )
        seen_root_fields.add(name)
        root_values[name] = value
        if name not in ROOT_FIELDS and not name.startswith("_"):
            _finding(
                report,
                code="schema-field",
                message=f"unknown root field '{name}'",
                file=manifest_file,
                line=line,
            )

    bank_value = root_values.get("bank_id")
    bank = bank_value if isinstance(bank_value, str) and bank_value.strip() else path.stem
    if "bank_id" not in root_values:
        _finding(
            report,
            code="schema-required",
            message="missing required field 'bank_id'",
            file=manifest_file,
            bank=bank,
        )
    elif not isinstance(bank_value, str) or not bank_value.strip():
        _finding(
            report,
            code="schema-type",
            message="field 'bank_id' must be a non-empty string",
            file=manifest_file,
            line=locator.key("bank_id"),
            bank=bank,
        )
    else:
        bank_line = locator.string(bank_value)
        if bank_value in global_bank_ids:
            first_file, first_line = global_bank_ids[bank_value]
            _finding(
                report,
                code="duplicate-bank-id",
                message=(
                    f"bank id '{bank_value}' duplicates {first_file}:{first_line}"
                ),
                file=manifest_file,
                line=bank_line,
                bank=bank_value,
            )
        else:
            global_bank_ids[bank_value] = (manifest_file, bank_line)

    if "streaming" in root_values:
        _validate_scalar_field(
            report,
            manifest_file,
            bank,
            None,
            "streaming",
            root_values["streaming"],
            locator.key("streaming"),
        )

    if "events" not in root_values:
        _finding(
            report,
            code="schema-required",
            message="missing required field 'events'",
            file=manifest_file,
            bank=bank,
        )
        return []

    events_obj = root_values["events"]
    if not isinstance(events_obj, JSONObject):
        _finding(
            report,
            code="schema-type",
            message="field 'events' must be an object",
            file=manifest_file,
            line=locator.key("events"),
            bank=bank,
        )
        return []
    if not events_obj:
        _finding(
            report,
            code="schema-value",
            message="field 'events' must not be empty",
            file=manifest_file,
            line=locator.key("events"),
            bank=bank,
        )

    references: list[AssetReference] = []
    local_event_ids: dict[str, tuple[str, int]] = {}
    for entry_id, event_obj in events_obj:
        entry_line = locator.key(entry_id)
        report.entries += 1
        if not isinstance(entry_id, str) or not entry_id.strip():
            _finding(
                report,
                code="schema-event-id",
                message="event id must be a non-empty string",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        local_duplicate = entry_id in local_event_ids
        if local_duplicate:
            first_file, first_line = local_event_ids[entry_id]
            _finding(
                report,
                code="duplicate-event-id",
                message=f"event id '{entry_id}' duplicates {first_file}:{first_line}",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        else:
            local_event_ids[entry_id] = (manifest_file, entry_line)

        if entry_id in global_event_ids and not local_duplicate:
            first_file, first_line, first_bank = global_event_ids[entry_id]
            _finding(
                report,
                code="duplicate-event-id",
                message=(
                    f"event id '{entry_id}' duplicates bank '{first_bank}' at "
                    f"{first_file}:{first_line}"
                ),
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        elif entry_id not in global_event_ids:
            global_event_ids[entry_id] = (manifest_file, entry_line, bank)

        if not isinstance(event_obj, JSONObject):
            _finding(
                report,
                code="schema-event",
                message=f"event '{entry_id}' must be an object",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
            continue

        event_values: dict[str, Any] = {}
        seen_fields: set[str] = set()
        for name, value in event_obj:
            field_line = locator.key(name)
            if name in seen_fields:
                _finding(
                    report,
                    code="duplicate-field",
                    message=f"event '{entry_id}' has duplicate field '{name}'",
                    file=manifest_file,
                    line=field_line,
                    bank=bank,
                    entry_id=entry_id,
                )
            seen_fields.add(name)
            event_values[name] = value
            if name not in EVENT_FIELDS and not name.startswith("_"):
                _finding(
                    report,
                    code="schema-field",
                    message=f"event '{entry_id}' has unknown field '{name}'",
                    file=manifest_file,
                    line=field_line,
                    bank=bank,
                    entry_id=entry_id,
                )
            elif name != "files" and not name.startswith("_"):
                _validate_scalar_field(
                    report,
                    manifest_file,
                    bank,
                    entry_id,
                    name,
                    value,
                    field_line,
                )

        files = event_values.get("files")
        if "volume" not in event_values:
            _finding(
                report,
                code="schema-required",
                message=f"event '{entry_id}' is missing required field 'volume'",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        strategy = event_values.get("strategy")
        if isinstance(strategy, str) and strategy not in {"random", "sequential"}:
            _finding(
                report,
                code="schema-value",
                message=f"event '{entry_id}' has unknown strategy '{strategy}'",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        attenuation = event_values.get("attenuation_model")
        if isinstance(attenuation, str) and attenuation not in {"linear", "inverse", "exponential"}:
            _finding(
                report,
                code="schema-value",
                message=f"event '{entry_id}' has unknown attenuation model '{attenuation}'",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        if "attenuation_model" in event_values:
            for required_distance in ("min_distance", "max_distance"):
                if required_distance not in event_values:
                    _finding(
                        report,
                        code="schema-required",
                        message=(
                            f"event '{entry_id}' with attenuation_model is missing "
                            f"'{required_distance}'"
                        ),
                        file=manifest_file,
                        line=entry_line,
                        bank=bank,
                        entry_id=entry_id,
                    )
        elif "min_distance" in event_values or "max_distance" in event_values:
            _finding(
                report,
                code="schema-required",
                message=(
                    f"event '{entry_id}' with attenuation distances is missing "
                    "'attenuation_model'"
                ),
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        if event_values.get("is_2d") is True and event_values.get("is_3d") is True:
            _finding(
                report,
                code="schema-spatial",
                message=f"event '{entry_id}' cannot be both 2D and 3D",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
        if "files" not in event_values:
            _finding(
                report,
                code="schema-required",
                message=f"event '{entry_id}' is missing required field 'files'",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )
            continue
        if not isinstance(files, list) or isinstance(files, JSONObject):
            _finding(
                report,
                code="schema-type",
                message=f"event '{entry_id}' field 'files' must be an array",
                file=manifest_file,
                line=locator.key("files"),
                bank=bank,
                entry_id=entry_id,
            )
            continue
        if not files:
            _finding(
                report,
                code="schema-value",
                message=f"event '{entry_id}' field 'files' must not be empty",
                file=manifest_file,
                line=locator.key("files"),
                bank=bank,
                entry_id=entry_id,
            )
            continue

        seen_asset_paths: set[str] = set()
        for raw_path in files:
            if not isinstance(raw_path, str) or not raw_path.strip():
                _finding(
                    report,
                    code="schema-asset-path",
                    message=f"event '{entry_id}' contains a non-string or empty asset path",
                    file=manifest_file,
                    line=locator.key("files"),
                    bank=bank,
                    entry_id=entry_id,
                )
                continue
            asset_line = locator.string(raw_path)
            if raw_path in seen_asset_paths:
                _finding(
                    report,
                    code="duplicate-asset-reference",
                    message=f"event '{entry_id}' references the same asset more than once",
                    file=manifest_file,
                    line=asset_line,
                    bank=bank,
                    entry_id=entry_id,
                    asset_path=raw_path,
                )
            seen_asset_paths.add(raw_path)
            suffix = PurePosixPath(raw_path.replace("\\", "/")).suffix.lower()
            if suffix not in AUDIO_SUFFIXES:
                _finding(
                    report,
                    code="unsupported-asset-format",
                    message=f"asset uses unsupported audio format '{suffix or '<none>'}'",
                    file=manifest_file,
                    line=asset_line,
                    bank=bank,
                    entry_id=entry_id,
                    asset_path=raw_path,
                )
            resolved = _resolve_asset(raw_path, assets_dir)
            if resolved is None:
                _finding(
                    report,
                    code="asset-path",
                    message="asset path must be under assets/audio/ and may not traverse directories",
                    file=manifest_file,
                    line=asset_line,
                    bank=bank,
                    entry_id=entry_id,
                    asset_path=raw_path,
                )
            references.append(
                AssetReference(
                    raw_path=raw_path,
                    resolved_path=resolved,
                    manifest_file=manifest_file,
                    line=asset_line,
                    bank=bank,
                    entry_id=entry_id,
                )
            )

        minimum = event_values.get("min_distance")
        maximum = event_values.get("max_distance")
        if _is_number(minimum) and _is_number(maximum) and maximum <= minimum:
            _finding(
                report,
                code="schema-distance",
                message=f"event '{entry_id}' max_distance must be at least min_distance",
                file=manifest_file,
                line=entry_line,
                bank=bank,
                entry_id=entry_id,
            )

    return references


def _id3v2_size(data: bytes) -> int:
    if len(data) < 10 or data[:3] != b"ID3":
        return 0
    size_bytes = data[6:10]
    if any(value & 0x80 for value in size_bytes):
        return 0
    tag_size = sum(value << shift for value, shift in zip(size_bytes, (21, 14, 7, 0)))
    footer_size = 10 if data[5] & 0x10 else 0
    return min(len(data), 10 + tag_size + footer_size)


def _mp3_frame(header: bytes) -> Optional[tuple[int, int]]:
    if len(header) < 4:
        return None
    bits = int.from_bytes(header[:4], "big")
    if bits >> 21 != 0x7FF:
        return None
    version_bits = (bits >> 19) & 0x3
    layer_bits = (bits >> 17) & 0x3
    bitrate_index = (bits >> 12) & 0xF
    sample_rate_index = (bits >> 10) & 0x3
    padding = (bits >> 9) & 0x1
    if version_bits == 1 or layer_bits == 0:
        return None
    if bitrate_index in {0, 15} or sample_rate_index == 3:
        return None

    version = {3: 1, 2: 2, 0: 25}[version_bits]
    layer = {3: 1, 2: 2, 1: 3}[layer_bits]
    if version == 1 and layer == 1:
        rates = (32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448)
    elif version == 1 and layer == 2:
        rates = (32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384)
    elif version == 1:
        rates = (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
    elif layer == 1:
        rates = (32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256)
    else:
        rates = (8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160)

    bitrate = rates[bitrate_index - 1] * 1000
    base_sample_rate = (44100, 48000, 32000)[sample_rate_index]
    sample_rate = base_sample_rate if version == 1 else base_sample_rate // (2 if version == 2 else 4)
    if layer == 1:
        frame_length = ((12 * bitrate) // sample_rate + padding) * 4
        samples = 384
    elif layer == 3 and version != 1:
        frame_length = (72 * bitrate) // sample_rate + padding
        samples = 576
    else:
        frame_length = (144 * bitrate) // sample_rate + padding
        samples = 1152
    if frame_length <= 4:
        return None
    return frame_length, samples * 1_000_000 // sample_rate


def _probe_mp3(path: Path) -> tuple[bool, str]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        return False, f"could not open asset: {exc}"
    if not data:
        return False, "asset is empty"

    start = _id3v2_size(data)
    first_frame: Optional[int] = None
    scan_limit = max(start, min(len(data) - 4, start + 256 * 1024))
    for offset in range(start, scan_limit + 1):
        parsed = _mp3_frame(data[offset : offset + 4])
        if parsed is not None and offset + parsed[0] <= len(data):
            first_frame = offset
            break
    if first_frame is None:
        return False, "no complete valid MP3 frame header was found"

    duration_us = 0
    frames = 0
    offset = first_frame
    while offset + 4 <= len(data):
        parsed = _mp3_frame(data[offset : offset + 4])
        if parsed is None:
            break
        frame_length, frame_duration_us = parsed
        if offset + frame_length > len(data):
            break
        frames += 1
        duration_us += frame_duration_us
        offset += frame_length
    if frames == 0 or duration_us <= 0:
        return False, "MP3 has no complete frames with non-zero duration"
    return True, ""


def _probe_wav(path: Path) -> tuple[bool, str]:
    try:
        with wave.open(str(path), "rb") as stream:
            frames = stream.getnframes()
            rate = stream.getframerate()
            channels = stream.getnchannels()
            sample_width = stream.getsampwidth()
            first_frame = stream.readframes(1)
    except (OSError, EOFError, wave.Error) as exc:
        return False, f"could not decode WAV header: {exc}"
    if frames <= 0 or rate <= 0 or channels <= 0 or sample_width <= 0:
        return False, "WAV has zero duration or invalid audio parameters"
    if not first_frame:
        return False, "WAV declares audio frames but contains no decodable frame data"
    return True, ""


def _probe_asset(path: Path) -> tuple[bool, str]:
    suffix = path.suffix.lower()
    if suffix == ".mp3":
        return _probe_mp3(path)
    if suffix == ".wav":
        return _probe_wav(path)
    return False, f"unsupported audio format '{suffix or '<none>'}'"


def _validate_assets(
    references: Iterable[AssetReference],
    assets_dir: Path,
    report: Report,
) -> set[Path]:
    referenced_paths: set[Path] = set()
    for reference in references:
        report.referenced_assets += 1
        if reference.resolved_path is None:
            continue
        path = reference.resolved_path
        referenced_paths.add(path)
        if not path.is_file():
            _finding(
                report,
                code="missing-asset",
                message="referenced asset does not exist or is not a file",
                file=reference.manifest_file,
                line=reference.line,
                bank=reference.bank,
                entry_id=reference.entry_id,
                asset_path=reference.raw_path,
            )
            continue
        valid, reason = _probe_asset(path)
        if not valid:
            _finding(
                report,
                code="undecodable-asset",
                message=reason,
                file=reference.manifest_file,
                line=reference.line,
                bank=reference.bank,
                entry_id=reference.entry_id,
                asset_path=reference.raw_path,
            )
    return referenced_paths


def _find_assets(
    assets_dir: Path,
    root: Path,
    report: Report,
    references: Iterable[AssetReference],
) -> set[Path]:
    if not assets_dir.is_dir():
        _finding(
            report,
            code="assets-directory",
            message="assets/audio directory does not exist or is not a directory",
            file=_display_path(assets_dir, root),
        )
        return set()
    namespaces: set[str] = set()
    for reference in references:
        if reference.resolved_path is None:
            continue
        try:
            relative = reference.resolved_path.relative_to(assets_dir.resolve())
        except ValueError:
            continue
        if relative.parts:
            namespaces.add(relative.parts[0].lower())
    if not namespaces:
        return set()
    try:
        assets = {
            path.resolve()
            for path in assets_dir.rglob("*")
            if path.is_file()
            and path.suffix.lower() in AUDIO_SUFFIXES
            and path.relative_to(assets_dir).parts[0].lower() in namespaces
        }
    except OSError as exc:
        _finding(
            report,
            code="assets-scan",
            message=f"could not scan audio assets: {exc}",
            file=_display_path(assets_dir, root),
        )
        return set()
    report.assets = len(assets)
    return assets


def validate(
    root: Path | str,
    manifests: Optional[Sequence[Path | str]] = None,
    assets_dir: Optional[Path | str] = None,
) -> Report:
    """Validate manifests relative to root and return all findings."""

    root_path = Path(root).resolve()
    assets_path = (
        Path(assets_dir).resolve()
        if assets_dir is not None
        else (root_path / "assets" / "audio").resolve()
    )
    manifest_values: Sequence[Path | str] = manifests or DEFAULT_MANIFESTS
    manifest_paths = [
        Path(value).resolve() if Path(value).is_absolute() else (root_path / value).resolve()
        for value in manifest_values
    ]

    report = Report()
    references: list[AssetReference] = []
    global_event_ids: dict[str, tuple[str, int, Optional[str]]] = {}
    global_bank_ids: dict[str, tuple[str, int]] = {}
    for manifest_path in manifest_paths:
        references.extend(
            _parse_manifest(
                manifest_path,
                root_path,
                assets_path,
                report,
                global_event_ids,
                global_bank_ids,
            )
        )

    referenced_paths = _validate_assets(references, assets_path, report)
    all_assets = _find_assets(assets_path, root_path, report, references)
    for orphan in sorted(all_assets - referenced_paths, key=lambda item: item.as_posix()):
        relative = _display_path(orphan, root_path)
        _finding(
            report,
            code="orphaned-asset",
            message="audio asset is not referenced by any validated bank",
            file=relative,
            severity="warning",
            asset_path=relative,
        )
    return report


def _human_report(report: Report) -> str:
    summary = (
        f"Validated {report.manifests} bank(s), {report.entries} event(s), "
        f"and {report.assets} audio asset(s)."
    )
    if report.valid:
        return f"Audio bank validation passed. {summary}"
    lines = [f"Audio bank validation found {len(report.findings)} finding(s). {summary}"]
    for item in report.findings:
        context = []
        if item.bank is not None:
            context.append(f"bank={item.bank}")
        if item.entry_id is not None:
            context.append(f"entry={item.entry_id}")
        if item.asset_path is not None:
            context.append(f"asset={item.asset_path}")
        suffix = f" ({', '.join(context)})" if context else ""
        lines.append(
            f"{item.severity.upper()} {item.file}:{item.line} [{item.code}] "
            f"{item.message}{suffix}"
        )
    return "\n".join(lines)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate luminumbra audio bank manifests and referenced assets."
    )
    parser.add_argument(
        "manifests",
        nargs="*",
        help="manifest paths relative to --root (defaults to the shipped music and SFX banks)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root",
    )
    parser.add_argument(
        "--assets-dir",
        type=Path,
        help="audio assets directory (defaults to <root>/assets/audio)",
    )
    parser.add_argument("--json", action="store_true", help="write machine-readable JSON")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    if not args.root.is_dir():
        _parser().error(f"repository root is not a directory: {args.root}")
    assets_dir = args.assets_dir
    if assets_dir is not None and not assets_dir.is_absolute():
        assets_dir = args.root / assets_dir
    report = validate(args.root, args.manifests or None, assets_dir)
    if args.json:
        json.dump(report.to_dict(), sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print(_human_report(report))
    return 0 if report.valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
