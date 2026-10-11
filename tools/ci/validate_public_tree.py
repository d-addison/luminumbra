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


def code_mask(text: str, powershell: bool) -> str:
    """Preserve offsets while blanking comments and literals for token exemptions.

    This mask never suppresses findings itself. Only a specifically recognized
    code token can be exempted, and all rules still inspect the original text.
    """
    masked = list(text)
    index, size = 0, len(text)

    def blank(start: int, end: int) -> None:
        for offset in range(start, end):
            if masked[offset] != "\n":
                masked[offset] = " "

    while index < size:
        start = index
        if powershell and text.startswith("<#", index):
            index += 2
            depth = 1
            while index < size and depth:
                if text.startswith("<#", index):
                    depth += 1
                    index += 2
                elif text.startswith("#>", index):
                    depth -= 1
                    index += 2
                else:
                    index += 1
        elif (powershell and text[index] == "#") or (not powershell and text.startswith("//", index)):
            end = text.find("\n", index)
            while not powershell and end != -1 and text[index:end].rstrip("\r").endswith("\\"):
                end = text.find("\n", end + 1)
            index = size if end == -1 else end
        elif not powershell and text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = size if end == -1 else end + 2
        elif powershell and text[index:index + 2] in ("@'", '@"'):
            quote = text[index + 1]
            end = re.search(r"(?m)^" + re.escape(quote + "@"), text[index + 2:])
            index = size if end is None else index + 2 + end.end()
        elif not powershell and text.startswith('R"', index):
            raw = re.match(r'R"([^\s()\\]{0,16})\(', text[index:])
            if raw is None:
                index += 1
                continue
            close = ")" + raw.group(1) + '"'
            end = text.find(close, index + raw.end())
            index = size if end == -1 else end + len(close)
        elif text[index] in ("'", '"'):
            quote = text[index]
            index += 1
            while index < size:
                if powershell and quote == "'" and text.startswith("''", index):
                    index += 2
                elif (not powershell and text[index] == "\\") or (
                        powershell and quote == '"' and text[index] == "`"):
                    index += 2
                elif text[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
            index = min(index, size)
        elif powershell and text[index] == "`":
            index = min(index + 2, size)
        else:
            index += 1
            continue
        blank(start, index)
    return "".join(masked)


CPP_INDEX = re.compile(
    r"\b[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*\[\s*"
    r"(?:[A-Za-z_]\w*\.)?(?P<token>phase\s*-\s*1)\s*\]"
)
PS_TUPLE_MEMBER = re.compile(
    r"(?:\]|\)|\$[A-Za-z_]\w*)\s*\.\s*(?P<token>Item[1-9]\d*)\b"
)


def code_token_exemptions(path: str, text: str) -> set[tuple[int, int]]:
    suffix = PurePosixPath(path).suffix.lower()
    if suffix in {".cpp", ".cc", ".cxx", ".h", ".hpp"}:
        pattern, powershell = CPP_INDEX, False
    elif suffix in {".ps1", ".psm1"}:
        pattern, powershell = PS_TUPLE_MEMBER, True
    else:
        return set()
    return {match.span("token") for match in pattern.finditer(code_mask(text, powershell))}


def inspect_text(path: str, text: str) -> list[Finding]:
    if path in TEXT_EXCLUDED_FILES or path.startswith(TEXT_EXCLUDED_PREFIXES):
        return []
    findings: list[Finding] = []
    exemptions = code_token_exemptions(path, text)
    offset = 0
    for line_number, full_line in enumerate(text.splitlines(keepends=True), 1):
        line = full_line.rstrip("\r\n")
        for rule, pattern in TEXT_RULES:
            for match in pattern.finditer(line):
                span = (offset + match.start(), offset + match.end())
                if rule == "implementation-history-reference" and span in exemptions:
                    continue
                findings.append(Finding(rule, path, match.group(0), line_number))
        offset += len(full_line)
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
