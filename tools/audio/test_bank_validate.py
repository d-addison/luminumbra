#!/usr/bin/env python3
"""Tests for the standalone audio bank validator."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

from bank_validate import Report, validate


def _mp3_bytes(frames: int = 2) -> bytes:
    header = bytes.fromhex("fffb9064")
    frame_length = 417
    return b"".join(header + bytes(frame_length - len(header)) for _ in range(frames))


def _manifest(events: dict[str, object], bank_id: str = "fixture") -> str:
    return json.dumps({"bank_id": bank_id, "events": events}, indent=2)


def _fixture(manifest_text: str, assets: dict[str, bytes]) -> tuple[tempfile.TemporaryDirectory[str], Path]:
    temporary = tempfile.TemporaryDirectory()
    root = Path(temporary.name)
    manifest = root / "data" / "audio" / "fixture.bank.json"
    manifest.parent.mkdir(parents=True)
    manifest.write_text(manifest_text, encoding="utf-8")
    audio_root = root / "assets" / "audio"
    audio_root.mkdir(parents=True)
    for relative, content in assets.items():
        target = audio_root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    return temporary, root


def _codes(report: Report) -> set[str]:
    return {finding.code for finding in report.findings}


def _validate_fixture(manifest_text: str, assets: dict[str, bytes]) -> Report:
    temporary, root = _fixture(manifest_text, assets)
    try:
        return validate(root, ["data/audio/fixture.bank.json"])
    finally:
        temporary.cleanup()


def test_clean_fixture() -> None:
    report = _validate_fixture(
        _manifest(
            {
                "clean": {
                    "files": ["assets/audio/sfx/clean.mp3"],
                    "volume": 0.5,
                    "is_2d": True,
                }
            }
        ),
        {"sfx/clean.mp3": _mp3_bytes()},
    )
    assert report.valid, report.to_dict()


def test_missing_asset_fixture() -> None:
    report = _validate_fixture(
        _manifest({"missing": {"files": ["assets/audio/sfx/missing.mp3"], "volume": 1.0}}),
        {},
    )
    assert "missing-asset" in _codes(report)
    finding = next(item for item in report.findings if item.code == "missing-asset")
    assert finding.severity == "error"
    assert finding.bank == "fixture"
    assert finding.entry_id == "missing"
    assert finding.asset_path == "assets/audio/sfx/missing.mp3"
    assert finding.line > 0


def test_duplicate_id_fixture() -> None:
    manifest_text = """{
  "bank_id": "fixture",
  "events": {
    "repeated": {"files": ["assets/audio/sfx/good.mp3"], "volume": 1.0},
    "repeated": {"files": ["assets/audio/sfx/good.mp3"], "volume": 1.0}
  }
}
"""
    report = _validate_fixture(manifest_text, {"sfx/good.mp3": _mp3_bytes()})
    duplicates = [item for item in report.findings if item.code == "duplicate-event-id"]
    assert len(duplicates) == 1
    assert duplicates[0].entry_id == "repeated"
    assert duplicates[0].line == 5


def test_duplicate_id_across_banks() -> None:
    temporary = tempfile.TemporaryDirectory()
    root = Path(temporary.name)
    try:
        bank_dir = root / "data" / "audio"
        bank_dir.mkdir(parents=True)
        event = {"shared": {"files": ["assets/audio/sfx/good.mp3"], "volume": 1.0}}
        (bank_dir / "one.bank.json").write_text(_manifest(event, "one"), encoding="utf-8")
        (bank_dir / "two.bank.json").write_text(_manifest(event, "two"), encoding="utf-8")
        asset = root / "assets" / "audio" / "sfx" / "good.mp3"
        asset.parent.mkdir(parents=True)
        asset.write_bytes(_mp3_bytes())
        report = validate(
            root,
            ["data/audio/one.bank.json", "data/audio/two.bank.json"],
        )
    finally:
        temporary.cleanup()
    duplicates = [item for item in report.findings if item.code == "duplicate-event-id"]
    assert len(duplicates) == 1
    assert duplicates[0].bank == "two"
    assert "bank 'one'" in duplicates[0].message


def test_undecodable_file_fixture() -> None:
    report = _validate_fixture(
        _manifest({"broken": {"files": ["assets/audio/sfx/broken.mp3"], "volume": 1.0}}),
        {"sfx/broken.mp3": b"this is not encoded audio"},
    )
    assert "undecodable-asset" in _codes(report)
    finding = next(item for item in report.findings if item.code == "undecodable-asset")
    assert finding.entry_id == "broken"
    assert finding.asset_path == "assets/audio/sfx/broken.mp3"


def test_orphaned_asset_fixture() -> None:
    report = _validate_fixture(
        _manifest({"used": {"files": ["assets/audio/sfx/used.mp3"], "volume": 1.0}}),
        {"sfx/used.mp3": _mp3_bytes(), "sfx/orphan.mp3": _mp3_bytes()},
    )
    orphans = [item for item in report.findings if item.code == "orphaned-asset"]
    assert len(orphans) == 1
    assert orphans[0].severity == "warning"
    assert orphans[0].asset_path == "assets/audio/sfx/orphan.mp3"


def test_schema_fixture() -> None:
    report = _validate_fixture(
        """{
  "bank_id": "fixture",
  "streaming": "yes",
  "events": {
    "bad_shape": {
      "files": "assets/audio/sfx/no-array.mp3",
      "volume": true,
      "unexpected": 1
    }
  }
}
""",
        {},
    )
    assert {"schema-type", "schema-field"}.issubset(_codes(report))


def test_json_shape_has_finding_context() -> None:
    report = _validate_fixture(
        _manifest({"missing": {"files": ["assets/audio/sfx/missing.mp3"], "volume": 1.0}}),
        {},
    )
    payload = report.to_dict()
    assert payload["valid"] is False
    assert payload["summary"]["findings"] == len(payload["findings"])
    for finding in payload["findings"]:
        assert {"severity", "bank", "entry_id", "asset_path", "file", "line"} <= finding.keys()


def _run_directly() -> int:
    tests = [
        value
        for name, value in globals().items()
        if name.startswith("test_") and callable(value)
    ]
    tests.sort(key=lambda test: test.__name__)
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"{len(tests)} tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(_run_directly())
