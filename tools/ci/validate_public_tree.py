#!/usr/bin/env python3
"""Reject files and provenance that must not enter the public source tree."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath


@dataclass(frozen=True)
class Finding:
    rule: str
    path: str
    detail: str
    line: int | None = None


FORBIDDEN_PATHS = (
    (re.compile(r"^\.banso(?:/|$)"), "local Banso state"),
    (re.compile(r"^references(?:/|$)"), "retired visual references"),
    (re.compile(r"^tools/banso-[^/]+(?:/|$)"), "internal orchestration extension"),
    (re.compile(r"^config/data-classification-policy\.yaml$"), "internal provider policy"),
    (re.compile(r"^(?:build|out|cmake-build-[^/]*)(?:/|$)"), "generated build output"),
    (re.compile(r"^(?:\.worktrees|scratch|scratch_six|logs)(?:/|$)"), "local working state"),
    (re.compile(r"^docs/internal(?:/|$)"), "private documentation"),
    (re.compile(r"^(?:assets|res)/audio(?:/|$)"), "local audio asset"),
)

AUDIO_SUFFIXES = {".aac", ".flac", ".m4a", ".mp3", ".ogg", ".opus", ".wav", ".wma"}
BUILD_SUFFIXES = {".a", ".dll", ".dylib", ".exe", ".ilk", ".lib", ".o", ".obj", ".pdb", ".so"}

TEXT_RULES = (
    ("unfinished-marker", re.compile(r"\b(?:TODO|FIXME|WIP|TBD|XXX)\b")),
    ("internal-spec-reference", re.compile(r"\bspec(?:ification)?\s*[-#]?\s*\d+", re.IGNORECASE)),
    ("internal-task-reference", re.compile(r"\btask\s*#?\s*\d+", re.IGNORECASE)),
    # A roadmap wave is an ordinal/letter label ("Wave B", "wave-2").
    # Do not confuse it with real simulation terms such as waveStrength.
    ("internal-wave-reference", re.compile(r"\b[Ww]ave[-_ ]+(?:[A-Z]|\d+[A-Za-z]?)\b")),
    ("numbered-pr-reference", re.compile(r"\b(?:PR|pull request)\s*#?\s*\d+", re.IGNORECASE)),
    ("internal-requirement-id", re.compile(
        r"\b(?:T-(?:I[A-Za-z0-9]+|WORLDGEN|EF|\d+)(?:-[A-Za-z0-9.]+)*|FR-[A-Za-z0-9.]+|"
        r"AC-[A-Za-z0-9.]+|P\d+-T\d+|A-T\d+|"
        r"(?:AUDIO|AETHER|ATMO|FOLIAGE|GPU|INSTINCT|NET|OPS|RENDER|SHIELD|WATER|UI)-"
        r"[A-Z0-9][A-Z0-9.]*)\b"
    )),
    ("internal-project-id", re.compile(
        r"\b(?:I\d+(?:-[A-Z0-9]+)+|WS-\d+|OQ-\d+|NFR-\d+|KDD-\d+|"
        r"FFF-\d+|RD-\d+|KD-\d+|T\d{3,}|P\d+(?:\.\d+)+)\b"
    )),
    ("implementation-history-reference", re.compile(
        r"\b(?:(?:iteration|rank|item)\s*[-#]?\s*\d+[A-Za-z]?|"
        r"critique\s*[A-Z]?\d+[A-Za-z]?|phase[- ]+\d+[A-Za-z]?(?:/\d+)?|"
        r"track\s*\([a-z]\)|slice\s*[- ]?\d+)\b",
        re.IGNORECASE,
    )),
    ("internal-short-label", re.compile(
        r"(?:\((?:A|B|C|E|F|G|I|M|N|O|P|W)\d+(?:\.\d+)?[a-z]?\)|"
        r"\b(?:defect|Codex)\s+[A-Z]\d+(?:\.\d+)?\b|"
        r"\bT\d+-[A-Za-z][A-Za-z0-9-]*\b|"
        r"\b0\d{2}-[A-Z][A-Za-z0-9.-]*\b|"
        r"(?<![\d.])-0\d{2}\b)"
    )),
    ("internal-leading-label", re.compile(
        r"^\s*(?://+|#+|/\*+|\*|--)\s*(?:\(?P\d+(?:\.\d+)?[a-z]?\)?|0\d{2})\s*(?::|\(|[A-Za-z])"
    )),
    ("internal-roadmap-reference", re.compile(
        r"(?:\b(?:[Pp]illar|[Tt]rack)[ -][A-Z]\b|\bpre-P\d+(?:\.\d+[A-Za-z]?)?\b|"
        r"\b[Cc]odex\s+audit(?:\s*#?\d+)?\b|\b017-B\b|"
        r"\b(?:[Ii]ncrement|inc)\s+\d+[A-Za-z]?(?:-\d+)?\b|"
        r"\b(?:I8|BF1|A3b|A-T05b|R1\.[0-9X]+|R1/OQ-A|OQ-[A-Z]|A1d)\b)"
    )),
    ("internal-document-reference", re.compile(
        r"\b(?:design-decisions|handoff|spec|plan)\.md\b", re.IGNORECASE
    )),
    ("private-posix-path", re.compile(r"(?:^|[\s'\"])(?:/home/[^/\s]+/|/mnt/[a-zA-Z]/)")),
    ("private-windows-path", re.compile(r"\b[A-Za-z]:\\(?:Users|src|code)\\", re.IGNORECASE)),
    ("github-token", re.compile(r"\b(?:gh[opsu]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b")),
    ("openai-token", re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{24,}\b")),
    ("aws-access-key", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
)

TEXT_EXCLUDED_PREFIXES = ("vendor/",)
# Generated vendor headers and the validator's self-test fixtures necessarily
# contain the tokens they recognize. Paths are exact so this cannot mask another
# first-party file.
TEXT_EXCLUDED_FILES = {
    "tools/cgltf.h",
    "tools/cgltf_write.h",
    "tools/ci/validate_public_tree.py",
    "tools/ci/test_validate_public_tree.py",
}


def inspect_path(path: str) -> list[Finding]:
    normalized = PurePosixPath(path).as_posix()
    if normalized.startswith("./"):
        normalized = normalized[2:]
    findings: list[Finding] = []
    for pattern, detail in FORBIDDEN_PATHS:
        if pattern.search(normalized):
            findings.append(Finding("forbidden-path", normalized, detail))
    suffix = PurePosixPath(normalized).suffix.lower()
    if suffix in AUDIO_SUFFIXES:
        findings.append(Finding("tracked-audio", normalized, f"audio extension {suffix}"))
    if suffix in BUILD_SUFFIXES:
        findings.append(Finding("generated-binary", normalized, f"binary extension {suffix}"))
    return findings


def inspect_text(path: str, text: str) -> list[Finding]:
    if path in TEXT_EXCLUDED_FILES or path.startswith(TEXT_EXCLUDED_PREFIXES):
        return []
    findings: list[Finding] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        for rule, pattern in TEXT_RULES:
            match = pattern.search(line)
            if match:
                findings.append(Finding(rule, path, match.group(0), line_number))
    return findings


def tracked_paths(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        check=True,
        stdout=subprocess.PIPE,
    )
    return sorted({
        item.decode("utf-8", "surrogateescape")
        for item in result.stdout.split(b"\0")
        if item
    })


def validate(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for relative in tracked_paths(root):
        findings.extend(inspect_path(relative))
        path = root / relative
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        findings.extend(inspect_text(relative, text))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    findings = validate(args.root.resolve())
    if args.json:
        print(json.dumps({"findings": [asdict(item) for item in findings]}, indent=2))
    elif findings:
        for item in findings:
            location = f"{item.path}:{item.line}" if item.line is not None else item.path
            print(f"{location}: {item.rule}: {item.detail}", file=sys.stderr)
    else:
        print("public-tree validation: PASS")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
