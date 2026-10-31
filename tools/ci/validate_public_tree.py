#!/usr/bin/env python3
"""Reject files and provenance that must not enter the public source tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from functools import lru_cache
from pathlib import Path, PurePosixPath


@dataclass(frozen=True)
class Finding:
    rule: str
    path: str
    detail: str
    line: int | None = None


FORBIDDEN_PATHS = (
    (re.compile(r"^\.\x62\x61\x6e\x73\x6f(?:/|$)"), "local orchestration state"),
    (re.compile(r"^references(?:/|$)"), "retired visual references"),
    (re.compile(r"^tools/\x62\x61\x6e\x73\x6f-[^/]+(?:/|$)"), "internal orchestration extension"),
    (re.compile(r"^config/data-classification-policy\.yaml$"), "internal provider policy"),
    (re.compile(r"^(?:build|out|cmake-build-[^/]*)(?:/|$)"), "generated build output"),
    (re.compile(r"^(?:\.worktrees|scratch|scratch_six|logs)(?:/|$)"), "local working state"),
    (re.compile(r"^docs/internal(?:/|$)"), "private documentation"),
    (re.compile(r"^\.tmp-private-notes\.md$"), "private archival notes"),
    (re.compile(r"^(?:assets|res)/audio(?:/|$)"), "local audio asset"),
)

AUDIO_SUFFIXES = {".aac", ".flac", ".m4a", ".mp3", ".ogg", ".opus", ".wav", ".wma"}
BUILD_SUFFIXES = {".a", ".dll", ".dylib", ".exe", ".ilk", ".lib", ".o", ".obj", ".pdb", ".so"}

TEXT_RULES = (
    ("unfinished-marker", re.compile(r"\b(?:\x54ODO|\x46IXME|\x57IP|\x54BD|\x58XX)\b")),
    ("internal-spec-reference", re.compile(r"\bspec(?:ification)?\s*[-#]?\s*\d+", re.IGNORECASE)),
    ("internal-task-reference", re.compile(r"\btask\s*#?\s*\d+", re.IGNORECASE)),
    # A roadmap wave is an ordinal or letter label.
    # Do not confuse it with real simulation terms such as waveStrength.
    ("internal-wave-reference", re.compile(r"\b[Ww]ave[-_ ]+(?:[A-Z]|\d+[A-Za-z]?)\b")),
    # Reject numbered PR prose, web paths and git refs; allow placeholder, templated and wildcard refs.
    ("numbered-pr-reference", re.compile(
        r"\b(?:PR|pull request)\s*#?\s*\d+|"
        r"/pull/\d+", re.IGNORECASE,
    )),
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
        r"\b(?:defect|\x43\x6f\x64\x65\x78)\s+[A-Z]\d+(?:\.\d+)?\b|"
        r"\bT\d+-[A-Za-z][A-Za-z0-9-]*\b|"
        r"\b0\d{2}-[A-Z][A-Za-z0-9.-]*\b|"
        r"(?<![\d.])-0\d{2}\b)"
    )),
    ("internal-leading-label", re.compile(
        r"^\s*(?://+|#+|/\*+|\*|--)\s*(?:\(?P\d+(?:\.\d+)?[a-z]?\)?|0\d{2})\s*(?::|\(|[A-Za-z])"
    )),
    ("internal-roadmap-reference", re.compile(
        r"(?:\b(?:[Pp]illar|[Tt]rack)[ -][A-Z]\b|\bpre-P\d+(?:\.\d+[A-Za-z]?)?\b|"
        r"\b[\x43\x63]\x6f\x64\x65\x78\s+audit(?:\s*#?\d+)?\b|\b017-B\b|"
        r"\b(?:[Ii]ncrement|inc)\s+\d+[A-Za-z]?(?:-\d+)?\b|"
        r"\b(?:\x498|\x42F1|\x413b|\x41-T05b|R1\.[0-9X]+|\x521/\x4fQ-A|OQ-[A-Z]|\x411d)\b)"
    )),
    ("internal-document-reference", re.compile(
        r"\b(?:design-decisions|handoff|spec|plan)\.md\b", re.IGNORECASE
    )),
    ("private-posix-path", re.compile(r"(?:^|[\s'\"])(?:/home/[^/\s]+/|/mnt/[a-zA-Z]/)")),
    ("private-unc-path", re.compile(r"(?:\\\\+|//)wsl(?:\.localhost|\$)[\\/]", re.IGNORECASE)),
    ("private-windows-path", re.compile(r"\b[A-Za-z]:\\(?:Users|src|code)\\", re.IGNORECASE)),
    ("github-token", re.compile(r"\b(?:gh[opsu]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b")),
    ("openai-token", re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{24,}\b")),
    ("aws-access-key", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
)

# These are necessary literal fragments, not replacement validation rules.
TEXT_RULE_HINTS = {
    "unfinished-marker": ("\x74odo", "\x66ixme", "\x77ip", "\x74bd", "\x78xx"),
    "internal-spec-reference": ("spec",),
    "internal-task-reference": ("task",),
    "internal-wave-reference": ("wave",),
    "numbered-pr-reference": ("pr", "pull"),
    "implementation-history-reference": ("iteration", "rank", "item", "critique", "phase", "track", "slice"),
    "internal-document-reference": (".md",),
    "private-posix-path": ("/home/", "/mnt/"),
    "private-unc-path": ("wsl",),
    "private-windows-path": (":\\",),
    "github-token": ("gh", "github_pat_"),
    "aws-access-key": ("akia", "asia"),
    "private-key": ("private key",),
}
TEXT_RULE_STARTS = {
    "internal-requirement-id": re.compile(
        r"\b(?:T-|FR-|AC-|P\d|A-T|AUDIO-|AETHER-|ATMO-|FOLIAGE-|GPU-|"
        r"INSTINCT-|NET-|OPS-|RENDER-|SHIELD-|WATER-|UI-)"),
    "internal-project-id": re.compile(r"\b(?:[ITP]\d|WS-|OQ-|NFR-|KDD-|FFF-|RD-|KD-)"),
    "internal-short-label": re.compile(
        r"\([ABCEFGIMNOPW]\d|\b(?:defect|[A-Z][a-z]{4})\s+[A-Z]\d|\b(?:T\d+-|0\d{2}-)|-0\d{2}"),
}

TEXT_EXCLUDED_PREFIXES = ("vendor/",)

# Provenance can appear in documentation, source comments or script literals.
# Require attribution context so game terms and library namespaces pass.
# Match candidate tokens, then compare their lowercase SHA-256 digests. Keep
# private reviewer vocabulary out of the public source and its test fixtures.
AGENT_NAME_DIGESTS = frozenset({
    "57de4cf40144bdf7d00010f2f5557a7d642c2b9705309bfade167dd313e2ca93",
    "693b286515bd1dd00865e7b60e4e53556537bbe4b1cc90ab608d94eb7c56fdc6",
    "8db59feb4d217f26c79d6e76eea6ff80398e8b823e376bb783be870a96cab9e7",
    "970ec274ca867815174ebe4eff19282000f9495a6c7254e94991d1fb4dc3df30",
    "f6db7a0ff91acb397f419640fb6b101a72738dceb313d11034cda6dba199d8a8",
    "c857d09db23e6822e3600bc06ad8d58f92ed62bc8efd81c753f77048662cb97d",
    "41418a61d982be103ecc30f2e00ca90bb4c4d1b2e107b9bca93f52ea0d10c6b3",
})
# Every name is followed by punctuation or whitespace. Check that boundary
# before attempting the longer context alternatives for each word prefix.
AGENT_NAME = r"([A-Za-z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)*?)(?![A-Za-z0-9])"
ATTRIBUTION_WORD = re.compile(r"[A-Za-z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)*", re.IGNORECASE)


def name_digest(token: str) -> str:
    return hashlib.sha256(token.lower().encode("utf-8")).hexdigest()


@lru_cache(maxsize=8192)
def _word_may_name_attribution(word: str, digests: frozenset[str]) -> bool:
    # Consider every possible name at hyphen boundaries. Bound this prefilter
    # to four components per word; longer compounds use the full matcher.
    # Retain at most 8192 word/vocabulary pairs across files.
    parts = word.split("-", 4)
    if len(parts) > 4:
        return True
    return any(name_digest("-".join(parts[start:end])) in digests
               for start in range(len(parts)) for end in range(start + 1, len(parts) + 1))


def _may_name_attribution(text: str, folded: str | None = None) -> bool:
    if folded is None:
        folded = text.casefold().replace("\u0131", "i").replace("i\u0307", "i")
    # Necessary fragments for all seven name-bearing alternatives below:
    # tiers; name before context; verbs; numbered references (searched below);
    # context before name; signatures; adapters. An alternative without a
    # necessary trigger must bypass this gate and run the full matcher.
    contexts = (
        "/low", "/medium", "/high", "/xhigh", "/max", "/ultra",
        "review", "audit", "verif", "analysis", "model", "agent", "sign", "authoritative", "finding", "critique",
        "accept", "approv", "reject", "found", "correct",
        "—", "--", "adapter",
    )
    if not any(context in folded for context in contexts) and not re.search(r"\br\d+|#\s*\d", text, re.IGNORECASE):
        return False
    digests = frozenset(AGENT_NAME_DIGESTS)
    seen: set[str] = set()
    for match in ATTRIBUTION_WORD.finditer(text):
        word = match[0].lower()
        if word in seen:
            continue
        seen.add(word)
        if _word_may_name_attribution(word, digests):
            return True
    return False


PROVENANCE_HASH = r"(?<![\w#])[`'\"]?[0-9a-f]{7,40}\b[`'\"]?"
HASH_ATTRIBUTION = (
    r"candidate|package|preview|private|native(?:\s+package)?|build|commit|revision|"
    r"snapshot|review(?:ed|ing)?|audit(?:ed|ing)?|verif(?:ied|ication)|"
    r"accept(?:ed|ance)?|qualification|tested|validated"
)
HASH_CONNECTORS = r"(?:\s+(?:of|at|on|for|from|with|against|source|candidate|build)){0,3}\s+"
HASH_NARRATIVE = re.compile(
    r"\b(?:review(?:ed|ing)?|accept(?:ed|ance)?)\b"
    r"(?!\s+(?:documentation|checks|obligations)\b)",
    re.IGNORECASE,
)
PROVENANCE_TEXT_RULES = (
    ("private-provenance", re.compile(
        r"\bprivate[-\s]+(?:native|candidate|package|release\s+campaign|evidence)\b",
        re.IGNORECASE,
    )),
    ("candidate-provenance", re.compile(
        rf"\b(?:{HASH_ATTRIBUTION}){HASH_CONNECTORS}{PROVENANCE_HASH}|"
        rf"{PROVENANCE_HASH}\s+(?:{HASH_ATTRIBUTION})\b|"
        rf"\bat\s+{PROVENANCE_HASH}",
        re.IGNORECASE,
    )),
    ("internal-reviewer-provenance", re.compile(
        r"\breview\s+(?:was\s+)?approved(?=\s*(?:[.;,:!?]|$))", re.IGNORECASE,
    )),
    ("internal-reviewer-provenance", re.compile(
        rf"\b{AGENT_NAME}/(?:low|medium|high|xhigh|max|ultra)\b|"
        rf"\b{AGENT_NAME}(?=[-\s]+(?:(?:source|independent|code|model|agent)[-\s]+){{0,3}}"
        r"(?:review|audit|verification|analysis|model|agent|sign(?:ed)?[-\s]+off|"
        r"authoritative|finding|critique)\b)|"
        rf"\b{AGENT_NAME}\s+(?:reviewed|audited|verified|accepted|approved|rejected|found|corrected)\b|"
        rf"\b{AGENT_NAME}\s+(?:#\s*\d+|r\d+)\b|"
        r"\b(?:review(?:ed)?|audit(?:ed)?|verif(?:ied|ication))\s+"
        rf"(?:(?:by|with|using|from|the|actual)\s+){{0,3}}{AGENT_NAME}\b(?![-\w]|\s*::)|"
        rf"(?:—|--)\s*{AGENT_NAME}\b(?![-\w])|"
        rf"\b{AGENT_NAME}\s+adapters?\b",
        re.IGNORECASE,
    )),
    ("private-process-provenance", re.compile(
        r"\bPID(?:\s+|\s*[:#]\s*)[`'\"]?\d+\b|"
        r"\boperator[-\s]+(?:closed|killed|terminated|cancelled|canceled|stopped)(?![-\w])|"
        r"\b(?:closed|killed|terminated|cancelled|canceled|stopped)\s+by\s+(?:the\s+)?operator\b",
        re.IGNORECASE,
    )),
)
PROVENANCE_RULE_HINTS = {
    "private-provenance": ("private",),
    "private-process-provenance": ("pid", "operator"),
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


CODE_TOKEN_START = re.compile(r"['\"/#<@`R\[]")
CPP_TOKEN_START = re.compile(r"['\"/#<@`R\[]|(?<!\w)[0-9]")
CPP_NUMBER = re.compile(r"[0-9][\w'.]*")


def code_mask(text: str, powershell: bool, *, comments_only: bool = False,
              hash_comments: bool = False, triple_quotes: bool = False,
              cmake: bool = False, cpp: bool = False, yaml: bool = False) -> str:
    """Preserve offsets in code, or retain only comments for prose inspection."""
    masked = list(text if not comments_only else re.sub(
        r"[^\n]+", lambda match: " " * len(match[0]), text))
    index, size = 0, len(text)
    token_start = CPP_TOKEN_START if cpp else CODE_TOKEN_START
    yaml_plain = False
    yaml_flow_depth = 0

    def blank(start: int, end: int) -> None:
        for offset in range(start, end):
            if masked[offset] != "\n":
                masked[offset] = " "

    while index < size:
        # Ordinary code cannot open a comment or literal. YAML also tracks
        # scalar boundaries, so it must continue examining every character.
        if not yaml:
            token = token_start.search(text, index)
            if token is None:
                break
            index = token.start()
        start = index
        is_comment = False
        if yaml and text[index] == "\n":
            yaml_plain = False
        # Separators belong to C++ numeric tokens, including hex digits. Consume
        # the entire token so its apostrophes cannot start character literals.
        if cpp and text[index].isdigit() and (index == 0 or not re.match(r"\w", text[index - 1])):
            number = CPP_NUMBER.match(text, index)
            index = number.end()
            continue
        # YAML quotes only open at the start of a scalar. Once a plain scalar
        # starts, embedded quotes are ordinary characters until the line ends.
        # Tags and anchors are scalar properties, not the scalar itself.
        if yaml and not yaml_plain and text[index] in "!&":
            property_token = re.match(r"(?:!<[^>\n]*>|[!&][^\s,\[\]{}]+|!)(?=\s|$)", text[index:])
            if property_token:
                index += property_token.end()
                continue
        if yaml and text[index] not in " \t\r\n:#,[]{}" and not yaml_plain:
            if text[index] not in "'\"" and not (
                    text[index] in "-?" and (index + 1 == size or text[index + 1].isspace())):
                yaml_plain = True
        if powershell and text.startswith("<#", index):
            is_comment = True
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
        elif cmake and text[index] in "#[" and (bracket := re.match(r"#?\[(=*)\[", text[index:])):
            is_comment = text[index] == "#"
            close = "]" + bracket.group(1) + "]"
            end = text.find(close, index + bracket.end())
            index = size if end == -1 else end + len(close)
        elif ((powershell or hash_comments) and text[index] == "#" and (
                not yaml or index == 0 or text[index - 1].isspace())) or (
                not powershell and not hash_comments and text.startswith("//", index)):
            is_comment = True
            end = text.find("\n", index)
            while not powershell and end != -1 and text[index:end].rstrip("\r").endswith("\\"):
                end = text.find("\n", end + 1)
            index = size if end == -1 else end
        elif not powershell and not hash_comments and text.startswith("/*", index):
            is_comment = True
            end = text.find("*/", index + 2)
            index = size if end == -1 else end + 2
        elif powershell and text[index:index + 2] in ("@'", '@"'):
            quote = text[index + 1]
            end = re.search(r"(?m)^" + re.escape(quote + "@"), text[index + 2:])
            index = size if end is None else index + 2 + end.end()
        elif not powershell and not yaml and text.startswith('R"', index):
            raw = re.match(r'R"([^\s()\\]{0,16})\(', text[index:])
            if raw is None:
                index += 1
                continue
            close = ")" + raw.group(1) + '"'
            end = text.find(close, index + raw.end())
            index = size if end == -1 else end + len(close)
        elif triple_quotes and text[index:index + 3] in ("'''", '"""'):
            quote = text[index:index + 3]
            index += 3
            while index < size:
                if text[index] == "\\":
                    index += 2
                elif text.startswith(quote, index):
                    index += 3
                    break
                else:
                    index += 1
            index = min(index, size)
        elif text[index] in ("'", '"') and not (yaml and yaml_plain):
            quote = text[index]
            index += 1
            while index < size:
                if (powershell or yaml) and quote == "'" and text.startswith("''", index):
                    index += 2
                elif (not powershell and (not yaml or quote == '"') and text[index] == "\\") or (
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
            if yaml:
                if text[index] in "[{" and not yaml_plain:
                    yaml_flow_depth += 1
                elif text[index] in "]}" and yaml_flow_depth:
                    yaml_flow_depth -= 1
                    yaml_plain = False
                elif (text[index] == "," and yaml_flow_depth) or (
                        text[index] == ":" and (index + 1 == size or text[index + 1].isspace())):
                    yaml_plain = False
            index += 1
            continue
        if comments_only:
            if is_comment:
                masked[start:index] = text[start:index]
        else:
            blank(start, index)
    return "".join(masked)


def candidate_prose(path: str, text: str) -> str:
    """Scan prose and comments, never structured data or executable literals."""
    relative = PurePosixPath(path)
    suffix = relative.suffix.lower()
    if (path.startswith(("data/", "assets/", "res/", "docs/assets/", "docs/evidence/"))
            or re.search(r"(?:^|[-_.])(?:manifest|inventory|checksums?|sha256)(?:[-_.]|$)",
                         relative.name, re.IGNORECASE)):
        return ""
    if suffix in {".md", ".rst", ".txt"} and relative.name != "CMakeLists.txt":
        return text
    if suffix in {".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx", ".js", ".ts"}:
        return code_mask(text, False, comments_only=True,
                         cpp=suffix not in {".c", ".js", ".ts"})
    if suffix in {".ps1", ".psm1"}:
        return code_mask(text, True, comments_only=True)
    if suffix in {".py", ".sh", ".bash", ".cmake", ".yaml", ".yml", ".toml"} or relative.name == "CMakeLists.txt":
        return code_mask(text, False, comments_only=True, hash_comments=True,
                         triple_quotes=suffix == ".py",
                         yaml=suffix in {".yaml", ".yml"},
                         cmake=suffix == ".cmake" or relative.name == "CMakeLists.txt")
    return ""


def prose_sentence(text: str, start: int, end: int) -> str:
    """Keep narrative context within a sentence or paragraph boundary."""
    boundaries = list(re.finditer(r"[.!?](?=\s|$)|\n[ \t]*\n", text))
    before = max((match.end() for match in boundaries if match.end() <= start), default=0)
    after = min((match.start() for match in boundaries if match.start() >= end), default=len(text))
    return text[before:after]


def hash_pin_context(text: str, match: re.Match[str]) -> bool:
    """Recognize data checksums and explicitly identified upstream tool pins."""
    prefix = text[max(0, match.start() - 80):match.start()]
    phrase = match.group(0).lower()
    if ((re.match(r"at\s", phrase) and re.search(r"\bworld_hash\s+$", prefix, re.IGNORECASE))
            or (phrase.startswith("revision") and re.search(r"\bhash\s+$", prefix, re.IGNORECASE))):
        return True
    boundary = text.rfind("\n\n", 0, match.start())
    start = boundary + 2 if boundary != -1 else 0
    end = text.find("\n\n", match.end())
    paragraph = text[start:end if end != -1 else len(text)]
    # A fixture generator/source pin is data provenance, not a reviewed build.
    if (phrase.startswith("commit")
            and "test/fixtures/" in paragraph
            and re.search(r"\bbaselines?\b", paragraph, re.IGNORECASE)):
        return True
    if (phrase.startswith("build")
            and re.search(r"\bBlender\s+(?:\d+\.\d+(?:\.\d+)?[, ]\s*)?$", prefix, re.IGNORECASE)):
        return True
    if (phrase.startswith("commit")
            and re.search(r"\bofficial\b", paragraph, re.IGNORECASE)
            and re.search(r"\btag\s+`v?\d+\.\d+", paragraph, re.IGNORECASE)):
        return True
    return False


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
        token = "phase"
    elif suffix in {".ps1", ".psm1"}:
        pattern, powershell = PS_TUPLE_MEMBER, True
        token = "Item"
    else:
        return set()
    if token not in text:
        return set()
    return {match.span("token") for match in pattern.finditer(
        code_mask(text, powershell, cpp=not powershell))}


def inspect_text(path: str, text: str) -> list[Finding]:
    if path.startswith(TEXT_EXCLUDED_PREFIXES):
        return []
    findings: list[Finding] = []
    # Include the extra Unicode letters recognized by case-insensitive ASCII
    # regexes. Extra candidates from case folding still use the exact rules.
    folded = text.casefold().replace("\u0131", "i").replace("i\u0307", "i")
    # Blank comment leaders without changing offsets, allowing prose to wrap
    # across C/C++, CMake, Python, PowerShell and YAML comment lines.
    prose = re.sub(r"(?m)^[ \t]*(?://+|#+|/\*+|\*|<#)[ \t]*",
                   lambda match: " " * len(match.group(0)), text)
    hash_prose = ""
    # Masking code is only needed when a contiguous hash could be present.
    if re.search(r"[0-9a-fA-F]{7}", text):
        hash_prose = candidate_prose(path, text)
        hash_prose = re.sub(r"(?m)^[ \t]*(?://+|#+|/\*+|\*|<#)[ \t]*",
                            lambda match: " " * len(match.group(0)), hash_prose)
    for rule, pattern in PROVENANCE_TEXT_RULES:
        if rule in PROVENANCE_RULE_HINTS and not any(hint in folded for hint in PROVENANCE_RULE_HINTS[rule]):
            continue
        if rule == "internal-reviewer-provenance" and pattern.groups and not _may_name_attribution(prose, folded):
            continue
        context = hash_prose if rule == "candidate-provenance" else prose
        for match in pattern.finditer(context):
            if rule == "internal-reviewer-provenance":
                names = [token for token in match.groups() if token is not None]
                if names and not any(name_digest(token) in AGENT_NAME_DIGESTS for token in names):
                    continue
            if rule == "candidate-provenance" and hash_pin_context(context, match):
                continue
            if rule == "candidate-provenance" and re.match(r"at\s", match.group(0), re.IGNORECASE):
                # A bare 'at <hex>' is attribution only within the same review
                # or acceptance paragraph; unrelated hash pins remain valid.
                if not HASH_NARRATIVE.search(prose_sentence(context, match.start(), match.end())):
                    continue
            findings.append(Finding(rule, path, text[match.start():match.end()],
                                    text.count("\n", 0, match.start()) + 1))
    # A multiline search is a conservative prefilter for the line rules:
    # cross-line matches may do extra work, but no matching line is omitted.
    line_text = re.sub(r"\r\n?|[\v\f\x1c-\x1e\x85\u2028\u2029]", "\n", text)
    active_rules = [(rule, pattern) for rule, pattern in TEXT_RULES
                    if (rule not in TEXT_RULE_HINTS or any(hint in folded for hint in TEXT_RULE_HINTS[rule]))
                    and (rule not in TEXT_RULE_STARTS or TEXT_RULE_STARTS[rule].search(text))
                    and re.search(pattern.pattern, line_text, pattern.flags | re.MULTILINE)]
    if not active_rules:
        return findings
    exemptions = code_token_exemptions(path, text)
    offset = 0
    for line_number, full_line in enumerate(text.splitlines(keepends=True), 1):
        line = full_line.rstrip("\r\n")
        for rule, pattern in active_rules:
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
        if relative.startswith(TEXT_EXCLUDED_PREFIXES):
            continue
        path = root / relative
        try:
            with path.open("rb") as handle:
                # Binary classification examines at most the first 8192 bytes.
                prefix = handle.read(8192)
                if b"\0" in prefix:
                    continue
                data = prefix + handle.read()
        except OSError:
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
