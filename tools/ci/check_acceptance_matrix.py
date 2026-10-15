#!/usr/bin/env python3
"""Validate the acceptance matrix and render its generated markdown view."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import unicodedata
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any


SCHEMA = "luminumbra.acceptance_matrix.v2"
INVENTORY_SCHEMA = "luminumbra.acceptance_requirements.v1"
CONTRACT_DOCUMENTS = ("docs/distant-world.md", "docs/active-regions.md")
DEFAULT_ROOT = Path(__file__).resolve().parents[2]
REQUIREMENT_SOURCE = Path("docs/distant-world.md")
REQUIREMENT_HEADING = "## Acceptance requirement IDs"
STATUS_ORDER = ("covered", "partial", "needs-native-run", "planned")
VALID_STATUSES = frozenset(STATUS_ORDER)
EVIDENCE_KINDS = frozenset({"ctest", "script", "native", "doc"})
EXCLUSION_CATEGORIES = frozenset({
    "current-behaviour", "historical", "rationale", "example", "cross-reference",
})
NORMATIVE_MARKER_RE = re.compile(
    r"\b(?:must|shall|requires|is\s+refused|is\s+a\s+failure|never)\b", re.IGNORECASE
)
# Reviewed exceptions are bound to the original source and full statement so an
# id cannot be reused to exclude a different normative claim.
REVIEWED_EXCLUSION_OVERRIDES = {
    "dw.tiers.021": {
        "source": "docs/distant-world.md#tier-ladder",
        "text": "edits reach far tiles only through authoritative SDF brick overlays captured "
                "from chunks that are still resident, because the far store is never attached "
                "to a save directory at runtime;",
        "category": "current-behaviour",
        "reason": "Reviewed Current behaviour paragraph: 'never attached' describes the "
                  "existing runtime gap, not an obligation of the accepted tier contract.",
    },
}

# Closed exclusion audit: every retained quote was read in its
# source paragraph. Entries bind id, source#anchor, category and exact UTF-8 quote
# SHA-256. Exclusion text must equal that full quote; shorter excerpts cannot
# hide obligations. New exclusions require a reviewed change to this table.
# Restored requirements: dw.policy.036 / .039 (C4 behavior and omission rules),
# ar.history.020 (historical evidence must retain its provenance).
# dw.intro.006 now maps the regional continuation qualification plan; the
# superseded navigation sentence is covered by the updated dw.intro.005 quote.
AUDITED_EXCLUSIONS = frozenset({
    # Document acceptance date, implementation status and gap-map cross-references.
    ('dw.intro.001', 'docs/distant-world.md#distant-world-and-distant-simulation-contract',
     'rationale', 'cdac254e0317b823be1051c381f63ce22d94e4ff38600125f5c53ad642d17ef4'),
    ('dw.intro.002', 'docs/distant-world.md#distant-world-and-distant-simulation-contract',
     'rationale', 'c6b7e41f9462666c5fce6bcf6b75753114eebd77a676abee2962fec9b0d7b5c4'),
    ('dw.intro.005', 'docs/distant-world.md#distant-world-and-distant-simulation-contract',
     'cross-reference', 'eb1fa3406481aee5ad81b3e7d00164329382daf82a692d1b91317d8c7fad295e'),
    # Explicit Current behaviour: shipped residency, rendering and scheduler gaps.
    ('dw.sets.001', 'docs/distant-world.md#three-explicit-sets',
     'current-behaviour', '409611d1c1c0ea16809345f9d2bec1917737bf7443cf7dbfc871294f20ca0c93'),
    ('dw.sets.002', 'docs/distant-world.md#three-explicit-sets',
     'current-behaviour', '7e538cf487ec85f42fda1ab354667af457eb2e2ab9082a6c21ef2b0c9425de6e'),
    ('dw.sets.003', 'docs/distant-world.md#three-explicit-sets',
     'current-behaviour', 'e6651f85b6add1d62034863513d38b870a209e39a5cd8c33853a9b88e5f28589'),
    ('dw.sets.004', 'docs/distant-world.md#three-explicit-sets',
     'current-behaviour', '16bc03a4c85578ef6dab2ea721331aca17f7ddd37a8ffb600819296d5e924f24'),
    ('dw.sets.005', 'docs/distant-world.md#three-explicit-sets',
     'current-behaviour', '287cb67cd68a5aa1936826fdebf4a68f7952db0bfe3912dcb7bf76a14655b430'),
    ('dw.sets.006', 'docs/distant-world.md#three-explicit-sets',
     'current-behaviour', 'fe8ebc5f9cd959b956f8d57526e55e40616c7c5c97c3db65d762a8e7e437b5cd'),
    # Explicit Current behaviour: existing heightfield/overlay limitations.
    ('dw.tiers.020', 'docs/distant-world.md#tier-ladder',
     'current-behaviour', '11a84edb05ddccd9f4db845deb18792ebadee85c2e09644cffe59783c28e78ec'),
    ('dw.tiers.021', 'docs/distant-world.md#tier-ladder',
     'current-behaviour', 'ac31f76707b1e82287a15e1bd1bdff03710a62da4716222463713477ca34a396'),
    ('dw.tiers.022', 'docs/distant-world.md#tier-ladder',
     'current-behaviour', '63df65c87941e51dd537a5019c6d8bb9465ef255a6b72e6d9b1fa25c05a0ff39'),
    # Explicit Current behaviour: shipped systems and legacy cadence, not C4 duties.
    ('dw.policy.031', 'docs/distant-world.md#per-system-distant-policy',
     'current-behaviour', 'e9c01e269e8d38eadc083bf2e18abc1f4f325ad39b1cd60eee26319e2760335b'),
    ('dw.policy.032', 'docs/distant-world.md#per-system-distant-policy',
     'current-behaviour', '9434099b059075c7f98e7c74e83e3365561a65a59bb37ea4c1c6c731b24bddae'),
    ('dw.policy.033', 'docs/distant-world.md#per-system-distant-policy',
     'current-behaviour', '9e353e05c508ef37e59954ca963443e5d98106cde1ee99e8b4ab995309070e3f'),
    ('dw.policy.034', 'docs/distant-world.md#per-system-distant-policy',
     'current-behaviour', '794123672747d0730701bb32379773e6dc24558368d53ebc8a575d13137106e5'),
    ('dw.policy.035', 'docs/distant-world.md#per-system-distant-policy',
     'current-behaviour', 'ba95962c50d32ccfb4df8ad58f6885abe89b759850bceb95912ad61d237ba486'),
    ('dw.policy.037', 'docs/distant-world.md#per-system-distant-policy',
     'current-behaviour', 'ccf3278a7425699d76ca30c6349c1cf07c1a6dca1670288993e899606f54b7a9'),
    ('dw.policy.038', 'docs/distant-world.md#per-system-distant-policy',
     'current-behaviour', '9dcea40b87f68a9706b1b16728321c16fc5b11fee3ae5c2f6e9100863526c083'),
    # Explicit Current behaviour: existing measurement gaps.
    ('dw.perf.018', 'docs/distant-world.md#performance-measurement-contract',
     'current-behaviour', 'cb8065e134442e599b949ec44f8413f32b66648c28c900d6fa8842df2bb9812a'),
    ('dw.perf.019', 'docs/distant-world.md#performance-measurement-contract',
     'current-behaviour', 'e9f5d1a853bc104b5ce2691aa95b4db3f8e411754da92dd37853d9b2037917f5'),
    # Pre-repair investigation observations, hashes and historical artifact provenance.
    ('ar.history.001', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'f3eb0146b2325ddd65ec2aeacf580069ea0281d8687006b5efdb20445bea41f1'),
    ('ar.history.002', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '886e7306a06cae4dedb2fb4092d755e24699eebab3661bfbbac34d3418390237'),
    ('ar.history.003', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'fe2a41203526d9432c8d2f8d856e7034cc80359810a94b09099c2a500967b539'),
    ('ar.history.004', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '00695cbe5b36319bbf7a9c2ddc59075d7982ef58ad6fb34c7f121febe64b5537'),
    ('ar.history.005', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'df9b1febead283bd7ca5ebccd26452d3444ac071c9714a36d43e29729071d37d'),
    ('ar.history.006', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '87ef733310ec40feb3a5d22c623c6a04097e426dc25ddf90131a8fdbc5e8fc3b'),
    ('ar.history.007', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '784ae0560f4776d8e9964f569ef42f3666b50ac04fe59dffafdbeb44f7f06e63'),
    ('ar.history.008', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '3ff566618065ec80f2d937746985c4a4a55cc61a02f7928abae7ff4ee841b194'),
    ('ar.history.009', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'ca8d1d1f87aabe4f5242c7e529bb2da70f2e3899c2862fdd527f4e8cd7f07cc5'),
    ('ar.history.010', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '1179746e60813f609c08eb3b65b9f819689906b63417da8a1a13f62a5e0c068a'),
    ('ar.history.011', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '3a7ce33ed1c07d8c7a591ca25d922cdeb62a50d61649983fc0140631e30c8122'),
    ('ar.history.012', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '382aa197d81ca04d82c485e96160db2650a3b786cbf8ead05e34caf1c1ead580'),
    ('ar.history.013', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'e4d2cb6a5d37b5ecf2f154445d4649d55b12739709a72f62e19591b73622ca58'),
    ('ar.history.014', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'a015bb5aaeac0e8392289108259f39b67d28ac809259b7682afc908cfdbef6c4'),
    ('ar.history.015', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', '271e6c42c937bae094e7e6215de6af43fdaa82039f4a217fe5b498224c89d5ba'),
    ('ar.history.017', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'a3ccd5678e29ef7b79f76d23e2b8782c5c4e72a4cd3dd1a67399ae6d14ababf6'),
    ('ar.history.022', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'e4e0652649044745c8a17feb97c354a3244ce70d000eda24f6cd50b956a8fc22'),
    ('ar.history.023', 'docs/active-regions.md#historical-ambient-hash-portability-investigation',
     'historical', 'd34788a580011f4ebd9e3b1aa9b2e17ef728c113b55da1d3190d30cb6269b6f4'),
})

ROW_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
RECEIPT_RE = re.compile(r"^luminumbra\.[a-z0-9_]+\.v[0-9]+$")
ADD_TEST_RE = re.compile(
    r'add_test\(\s*NAME\s+(?:"(?P<quoted>[^"]*)"|(?P<bare>[^\s"()]+))', re.IGNORECASE
)
CTEST_NAME_RE = re.compile(r"[A-Za-z0-9_.-]+")
CMAKE_BRACKET_OPEN_RE = re.compile(r"#\[(=*)\[")
C_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
GTEST_RE = re.compile(r"\bTEST(?:_F|_P)?\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)")
SUITE_REF_RE = re.compile(r"^([A-Za-z_]\w*)\.(\*|[A-Za-z_]\w*)$")
C_DIRECTIVE_RE = re.compile(
    r"^\s*#\s*(elifdef|elifndef|elif|ifdef|ifndef|if|else|endif)\b(.*)$"
)
C_STATIC_FALSE = frozenset({"0", "(0)"})
CMAKE_FLOW_RE = re.compile(r"(elseif|else|endif|if)\s*\(", re.IGNORECASE)
CMAKE_STRING_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"')
CMAKE_STATIC_FALSE = frozenset({"FALSE", "0", "OFF"})


class MatrixError(Exception):
    """The matrix file cannot be read or parsed."""


@dataclass(frozen=True)
class TestIndex:
    ctest_names: frozenset[str]
    gtest_cases: frozenset[tuple[str, str]]
    gtest_suites: frozenset[str]


def load_matrix(path: Path) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise MatrixError(f"cannot read {path}: {exc}") from exc
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise MatrixError(f"cannot parse {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise MatrixError(f"{path}: top-level value must be a JSON object")
    return data


def load_required_ids(path: Path) -> frozenset[str]:
    """Read the authoritative contract list, independently of the evidence rows."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise MatrixError(f"cannot read requirement source {path}: {exc}") from exc
    if lines.count(REQUIREMENT_HEADING) != 1:
        raise MatrixError(f"{path}: expected one {REQUIREMENT_HEADING} section")
    required: set[str] = set()
    for line in lines[lines.index(REQUIREMENT_HEADING) + 1 :]:
        if line.startswith("## "):
            break
        if not line.startswith("- "):
            continue
        match = re.fullmatch(r"- `([a-z0-9][a-z0-9._-]*)`: (\S.*)", line)
        if match is None:
            raise MatrixError(f"{path}: malformed requirement entry: {line}")
        row_id = match.group(1)
        if row_id in required:
            raise MatrixError(f"{path}: duplicate requirement ID: {row_id}")
        required.add(row_id)
    if not required:
        raise MatrixError(f"{path}: requirement list must not be empty")
    return frozenset(required)


def _cmake_files(root: Path) -> list[Path]:
    files: set[Path] = set()
    top = root / "CMakeLists.txt"
    if top.is_file():
        files.add(top)
    for sub in ("test", "tools"):
        base = root / sub
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file() and (path.name == "CMakeLists.txt" or path.suffix == ".cmake"):
                files.add(path)
    return sorted(files)


def _gtest_sources(root: Path) -> list[Path]:
    base = root / "test"
    if not base.is_dir():
        return []
    return sorted(
        path for path in base.rglob("*") if path.is_file() and path.suffix in {".cpp", ".h"}
    )


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _strip_cmake(text: str) -> str:
    """Remove CMake comments; '#' inside a quoted argument is not a comment."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
            continue
        if ch == "#":
            bracket = CMAKE_BRACKET_OPEN_RE.match(text, i)
            if bracket:
                closer = "]" + bracket.group(1) + "]"
                end = text.find(closer, bracket.end())
                stop = n if end == -1 else end + len(closer)
                out.append(_blank(text[i:stop]))
                i = stop
                continue
            end = text.find("\n", i)
            i = n if end == -1 else end
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _blank(comment: str) -> str:
    """Stand-in for a removed comment; keeps its newlines so line numbers survive."""
    return "\n" * comment.count("\n") or " "


def _strip_c(text: str) -> str:
    """Remove C/C++ block and line comments (approximate: ignores string literals)."""
    return C_COMMENT_RE.sub(lambda m: _blank(m.group(0)), text)


def _drop_disabled_c(text: str) -> str:
    """Drop lines inside statically disabled branches of `#if`/`#elif` chains.

    A branch is disabled when its own condition is `0`/`(0)`, whatever the earlier
    branches were. Any other `#elif`, `#elifdef`, `#elifndef` or `#else` branch is live.
    """
    out: list[str] = []
    # One entry per open conditional: [chain_is_static_false, branch_is_disabled].
    stack: list[list[bool]] = []
    for line in text.splitlines(keepends=True):
        match = C_DIRECTIVE_RE.match(line)
        if match is None:
            if not any(dead for _, dead in stack):
                out.append(line)
            continue
        kind, rest = match.group(1), match.group(2).strip()
        if kind in ("if", "ifdef", "ifndef"):
            zero = kind == "if" and rest in C_STATIC_FALSE
            stack.append([zero, zero])
        elif kind == "endif":
            if stack:
                stack.pop()
        elif stack:
            if kind == "elif":
                zero = rest in C_STATIC_FALSE
                stack[-1] = [zero, zero]
            else:
                stack[-1] = [False, False]
        if not any(dead for _, dead in stack):
            out.append(line)
    return "".join(out)


def _cmake_close_paren(text: str, open_idx: int) -> int | None:
    depth = 0
    j = open_idx
    while j < len(text):
        ch = text[j]
        if ch == '"':
            string = CMAKE_STRING_RE.match(text, j)
            if string:
                j = string.end()
                continue
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return None


def _cmake_flow_commands(text: str) -> list[tuple[str, str, int, int]]:
    """Return (name, args, start, end) for each if/elseif/else/endif call outside quotes."""
    found: list[tuple[str, str, int, int]] = []
    i = 0
    while i < len(text):
        if text[i] == '"':
            string = CMAKE_STRING_RE.match(text, i)
            if string:
                i = string.end()
                continue
        match = CMAKE_FLOW_RE.match(text, i)
        if match and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            close = _cmake_close_paren(text, match.end() - 1)
            if close is not None:
                args = text[match.end() : close].strip()
                found.append((match.group(1).lower(), args, i, close + 1))
                i = close + 1
                continue
        i += 1
    return found


def _drop_disabled_cmake(text: str) -> str:
    """Drop statically disabled branches of an if() chain.

    A branch is disabled when its own condition is FALSE/0/OFF (case-insensitive),
    whatever the earlier branches were. Any other elseif() condition and every else() is live.
    """
    # One entry per open if(): [chain_is_static_false, branch_is_disabled].
    stack: list[list[bool]] = []
    spans: list[tuple[int, int]] = []
    dead_from = 0
    for name, args, start, end in _cmake_flow_commands(text):
        was_dead = any(dead for _, dead in stack)
        if name == "if":
            static_false = args.upper() in CMAKE_STATIC_FALSE
            stack.append([static_false, static_false])
        elif name == "endif":
            if stack:
                stack.pop()
        elif name == "elseif":
            if stack:
                zero = args.upper() in CMAKE_STATIC_FALSE
                stack[-1] = [zero, zero]
        elif stack:
            stack[-1] = [False, False]
        is_dead = any(dead for _, dead in stack)
        if is_dead and not was_dead:
            dead_from = end
        elif was_dead and not is_dead:
            spans.append((dead_from, start))
    if any(dead for _, dead in stack):
        spans.append((dead_from, len(text)))
    out: list[str] = []
    cursor = 0
    for begin, stop in spans:
        out.append(text[cursor:begin])
        cursor = stop
    out.append(text[cursor:])
    return "".join(out)


def _is_ctest_name(name: str | None) -> bool:
    # Names containing '$' are CMake variables or generator expressions; their
    # truncated prefix is not the real test name, so they are never indexed.
    return name is not None and "$" not in name and CTEST_NAME_RE.fullmatch(name) is not None


def index_tests(root: Path) -> TestIndex:
    ctest_names: set[str] = set()
    for path in _cmake_files(root):
        text = _drop_disabled_cmake(_strip_cmake(_read_text(path)))
        for match in ADD_TEST_RE.finditer(text):
            name = match.group("quoted") if match.group("quoted") is not None else match.group("bare")
            if _is_ctest_name(name):
                ctest_names.add(name)
    gtest_cases: set[tuple[str, str]] = set()
    for path in _gtest_sources(root):
        text = _drop_disabled_c(_strip_c(_read_text(path)))
        gtest_cases.update((match.group(1), match.group(2)) for match in GTEST_RE.finditer(text))
    return TestIndex(
        frozenset(ctest_names),
        frozenset(gtest_cases),
        frozenset(suite for suite, _ in gtest_cases),
    )


def _ctest_exists(ref: str, index: TestIndex) -> bool:
    if ref in index.ctest_names:
        return True
    match = SUITE_REF_RE.match(ref)
    if not match:
        return False
    suite, case = match.groups()
    if case == "*":
        return suite in index.gtest_suites
    return (suite, case) in index.gtest_cases


def _is_nonempty_str(value: Any) -> bool:
    return isinstance(value, str) and value.strip() != ""


def _path_problem(base: Path, ref: str, must_exist: bool) -> str | None:
    if ref.startswith("/") or re.match(r"^[A-Za-z]:", ref) or PurePosixPath(ref).is_absolute():
        return f"path must be repo-relative: {ref}"
    if ".." in re.split(r"[\\/]", ref):
        return f"path escapes root: {ref}"
    target = base / ref
    try:
        target.resolve().relative_to(base)
    except ValueError:
        return f"path escapes root: {ref}"
    if must_exist and not target.is_file():
        return f"file not found: {ref}"
    return None


def heading_slug(heading: str) -> str:
    """GitHub heading slug: preserve Unicode letters/numbers, '-' and '_'."""
    heading = re.sub(r"<[^>]*>", "", heading).strip().lower()
    return "".join(
        ch for ch in heading
        if ch in "-_ " or unicodedata.category(ch)[0] in {"L", "N", "M"}
    ).replace(" ", "-")


def _headings(text: str) -> dict[int, str]:
    headings: dict[int, str] = {}
    used: set[str] = set()
    fenced = False
    for number, line in enumerate(text.splitlines()):
        if re.match(r"^\s*(```|~~~)", line):
            fenced = not fenced
        if fenced:
            continue
        match = re.match(r"^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$", line)
        if match:
            stem = heading_slug(match.group(1))
            slug, suffix = stem, 0
            while slug in used:
                suffix += 1
                slug = f"{stem}-{suffix}"
            headings[number] = slug
            used.add(slug)
    return headings


def _source_problem(base: Path, source: str) -> str | None:
    if source.startswith("issue"):
        if re.fullmatch(r"issue #[1-9][0-9]*", source) is None:
            return f"invalid issue source: {source}"
        return None
    path, separator, anchor = source.partition("#")
    problem = _path_problem(base, path, must_exist=True)
    if problem:
        return f"source {problem}"
    if separator and anchor not in _headings(_read_text(base / path)).values():
        return f"source anchor not found: {source}"
    return None


def contract_statements(text: str, path: str) -> list[tuple[str, str, bool]]:
    """Account for all prose and table data, not just sentences with 'must'.

    Soft wraps are normalized. Prose, bullets and table rows split at sentence
    and semicolon boundaries. Headings and table scaffolding are not statements.
    Current-behaviour paragraphs are explicitly descriptive.
    Inventory ids are maintained independently of these content units.
    """
    lines = text.splitlines()
    headings = _headings(text)
    source = path
    paragraph: list[str] = []
    result: list[tuple[str, str, bool]] = []

    def flush() -> None:
        if not paragraph:
            return
        block = " ".join(line.strip() for line in paragraph)
        descriptive = block.startswith("Current behaviour:")
        units = re.split(r"(?<=[.!?])\s+(?=[A-Z`])|(?<=;)\s+", block)
        result.extend((source, unit, descriptive) for unit in units)
        paragraph.clear()

    for number, line in enumerate(lines):
        if number in headings:
            flush()
            source = f"{path}#{headings[number]}"
        elif not line.strip():
            flush()
        elif line.startswith("|"):
            flush()
            # The header preceding a separator is table scaffolding as well.
            if re.match(r"^\|[ :|\-]+\|$", line) or (
                number + 1 < len(lines)
                and re.match(r"^\|[ :|\-]+\|$", lines[number + 1])
            ):
                continue
            paragraph.append(line)
            flush()
        elif line.startswith("- "):
            flush()
            paragraph.append(line)
        else:
            paragraph.append(line)
    flush()
    return result


def validate_inventory(
    inventory: dict[str, Any], root: Path
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Require an explicit requirement or justified exclusion for every unit."""
    errors: list[str] = []
    requirements: dict[str, dict[str, Any]] = {}
    expected: Counter[tuple[str, str]] = Counter()
    descriptive: set[tuple[str, str]] = set()
    if inventory.get("schema") != INVENTORY_SCHEMA:
        errors.append(f"inventory schema must be {INVENTORY_SCHEMA}")
    if inventory.get("documents") != list(CONTRACT_DOCUMENTS):
        errors.append("inventory documents must name both reviewed contract documents")
    for path in CONTRACT_DOCUMENTS:
        problem = _path_problem(root, path, must_exist=True)
        if problem:
            errors.append(problem)
            continue
        for source, text, is_descriptive in contract_statements(_read_text(root / path), path):
            expected[(source, text)] += 1
            if is_descriptive:
                descriptive.add((source, text))
    source_problems: dict[str, str | None] = {}
    actual: Counter[tuple[str, str]] = Counter()
    ids: set[str] = set()
    for kind in ("requirements", "exclusions"):
        entries = inventory.get(kind)
        if not isinstance(entries, list):
            errors.append(f"inventory {kind} must be a list")
            continue
        for position, entry in enumerate(entries):
            label = f"{kind}[{position}]"
            if not isinstance(entry, dict):
                errors.append(f"{label}: must be an object")
                continue
            identity = entry.get("id")
            if not isinstance(identity, str) or not ROW_ID_RE.fullmatch(identity):
                errors.append(f"{label}: invalid stable id")
            elif identity in ids:
                errors.append(f"{identity}: duplicate inventory id")
            else:
                label = identity
                ids.add(identity)
                if kind == "requirements":
                    requirements[identity] = entry
            source, text, quote = (entry.get(key) for key in ("source", "text", "quote"))
            if not all(_is_nonempty_str(value) for value in (source, text, quote)):
                errors.append(f"{label}: source, text and quote must be non-empty strings")
                continue
            if source not in source_problems:
                source_problems[source] = _source_problem(root, source)
            problem = source_problems[source]
            if problem:
                errors.append(f"{label}: {problem}")
            if quote not in text:
                errors.append(f"{label}: quote must be a verbatim excerpt of text")
            actual[(source, text)] += 1
            if kind == "exclusions":
                if not _is_nonempty_str(entry.get("reason")):
                    errors.append(f"{label}: exclusion requires a reason")
                category = entry.get("category")
                audited = (
                    isinstance(identity, str) and isinstance(category, str)
                    and text == quote
                    and (identity, source, category, hashlib.sha256(quote.encode("utf-8")).hexdigest())
                    in AUDITED_EXCLUSIONS
                )
                if not audited:
                    errors.append(f"{label}: exclusion is not in the closed audited table")
                if not isinstance(category, str) or category not in EXCLUSION_CATEGORIES:
                    errors.append(f"{label}: exclusion requires an audited non-normative category")
                if category == "current-behaviour" and (source, text) not in descriptive:
                    errors.append(f"{label}: current-behaviour exclusion is not in a Current behaviour paragraph")
                if category == "historical" and "historical" not in source.partition("#")[2]:
                    errors.append(f"{label}: historical exclusion requires a historical heading")
                # Check the full text as well as its excerpt to prevent shortening
                # the quote from hiding a normative marker.
                if NORMATIVE_MARKER_RE.search(re.sub(r"[*_`]+", "", text)):
                    override = REVIEWED_EXCLUSION_OVERRIDES.get(identity) if isinstance(identity, str) else None
                    if not (override and _is_nonempty_str(override.get("reason"))
                            and all(entry.get(key) == override[key] for key in ("source", "text", "category"))):
                        errors.append(f"{label}: normative exclusion requires an exact reviewed override")
    for (source, text), count in (expected - actual).items():
        errors.append(f"inventory unaccounted statement ({count}): {source}: {text[:100]}")
    for (source, text), count in (actual - expected).items():
        errors.append(f"inventory stale or duplicate statement ({count}): {source}: {text[:100]}")
    if not requirements:
        errors.append("inventory requirements must not be empty")
    return requirements, errors


def _check_planned(add: Any, planned: Any) -> None:
    if not _is_nonempty_str(planned):
        add("planned must be a non-empty descriptive string")
        return
    note, separator, details = planned.partition("; owner:")
    if not note.strip():
        add("planned work description must not be empty")
    if not separator or not details.split(";", 1)[0].strip():
        add("planned must include a non-empty '; owner:' workstream")


NATIVE_SCRIPT_RE = re.compile(r"[A-Za-z0-9_./ :\\-]+\.(?:ps1|py|sh|bash|js|rb|pl|cmd|bat)\Z")
# Each alternative has a distinct first character and a single parse: a literal
# character or a delimited placeholder. Malformed suffixes cannot repartition
# runs of literal characters into exponentially many repeated groups.
NATIVE_ARG_RE = re.compile(r"(?:[-A-Za-z0-9_./ :\\=,@+%]|<[A-Za-z0-9_.-]+>)+\Z")


def _normalise_native_path(path: str) -> str:
    path = path.replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    return path


def _native_entry_point(command: str) -> str | None:
    """Parse only the closed command grammar; never search options for a path.

    Hosts are exactly pwsh, python3, python and bash. Only pwsh's optional
    -NoProfile and -ExecutionPolicy Bypass before -File are supported, in that
    order. Python hosts accept only .py, pwsh only .ps1, and bash .sh/.bash.
    Arguments are literal words or documentation placeholders, with
    exactly one script path in the command. Shell syntax is never executable
    evidence. Quotes and Windows path separators are normalized lexically.
    """
    if any(ch in command for ch in "|;&`$\r\n"):
        return None
    lexer = shlex.shlex(command, posix=True)
    lexer.whitespace_split = True
    lexer.commenters = ""
    lexer.escape = ""
    try:
        words = list(lexer)
    except ValueError:
        return None
    if not words:
        return None
    position = 0
    suffixes: set[str] | None = None
    if words[0] in {"python3", "python", "bash"}:
        position = 1
        suffixes = {".py"} if words[0] in {"python3", "python"} else {".sh", ".bash"}
    elif words[0] == "pwsh":
        suffixes = {".ps1"}
        position = 1
        if position < len(words) and words[position] == "-NoProfile":
            position += 1
        if position < len(words) and words[position] == "-ExecutionPolicy":
            if words[position + 1:position + 2] != ["Bypass"]:
                return None
            position += 2
        if position < len(words) and words[position] == "-File":
            position += 1
        elif position != 1:
            return None  # Interpreter options require the -File form.
    if position >= len(words):
        return None
    path = _normalise_native_path(words[position])
    if path.startswith("-") or NATIVE_SCRIPT_RE.fullmatch(path) is None:
        return None
    if suffixes is not None and PurePosixPath(path).suffix not in suffixes:
        return None
    for arg in words[position + 1:]:
        if NATIVE_ARG_RE.fullmatch(arg) is None:
            return None
        # Paths supplied as positional or --key=path arguments also count.
        value = _normalise_native_path(arg.partition("=")[2] if "=" in arg else arg)
        if NATIVE_SCRIPT_RE.fullmatch(value):
            return None
    return path


def _check_item(
    add: Any,
    item: Any,
    base: Path,
    index: TestIndex,
    require_landed: bool,
) -> None:
    if not isinstance(item, dict):
        add("evidence item must be an object")
        return
    kind = item.get("kind")
    if kind not in EVIDENCE_KINDS:
        add(f"unknown evidence kind: {kind}")
        return
    ref = item.get("ref")
    if not _is_nonempty_str(ref):
        add("evidence ref must be a non-empty string")
        return
    is_planned = "planned" in item
    if is_planned:
        _check_planned(add, item["planned"])

    if kind == "ctest":
        if not is_planned and not _ctest_exists(ref, index):
            add(f"ctest not found: {ref}")
    else:
        problem = _path_problem(base, ref, must_exist=not is_planned)
        if problem:
            add(problem)

    if kind == "native":
        command = item.get("command")
        if not _is_nonempty_str(command):
            add("native command must be a non-empty string")
        else:
            entry_point = _native_entry_point(command)
            if entry_point is None:
                add("native command must invoke a repository script entry point")
            else:
                problem = _path_problem(base, entry_point, must_exist=not is_planned)
                if problem:
                    add(f"native command entry point {problem}")
                if entry_point != ref:
                    add(f"native command entry point must match ref: {entry_point} != {ref}")
        receipt = item.get("receipt_schema")
        if not isinstance(receipt, str) or not RECEIPT_RE.match(receipt):
            add("native receipt_schema must match ^luminumbra\\.[a-z0-9_]+\\.v[0-9]+$")

    if is_planned and require_landed:
        add(f"planned evidence not landed: {ref}")


def _check_status(add: Any, status: str, items: list[dict[str, Any]]) -> None:
    has_planned = any("planned" in item for item in items)
    has_plain = any("planned" not in item for item in items)
    kinds = {item.get("kind") for item in items}

    def fail(why: str) -> None:
        add(f"status {status} inconsistent with evidence: {why}")

    if status == "covered":
        if has_planned:
            fail("planned items are not allowed")
        if not kinds & {"ctest", "script"}:
            fail("at least one ctest or script item is required")
        if "native" in kinds:
            fail("native evidence requires needs-native-run")
    elif status == "needs-native-run":
        if "native" not in kinds:
            fail("at least one native item is required")
    elif status == "planned":
        if has_plain:
            fail("all items must be planned")
    elif status == "partial":
        if not (has_planned and has_plain):
            fail("at least one planned and at least one non-planned item are required")


def validate(
    matrix: dict[str, Any],
    index: TestIndex,
    root: Path,
    require_landed: bool,
) -> list[str]:
    base = root.resolve()
    problems: list[tuple[str, str]] = []
    try:
        inventory = load_matrix(base / "tools/ci/acceptance_requirements.json")
        requirements, inventory_errors = validate_inventory(inventory, base)
    except MatrixError as exc:
        requirements, inventory_errors = {}, [str(exc)]
    problems.extend(("<inventory>", message) for message in inventory_errors)
    mapped: set[str] = set()
    rows = matrix.get("rows")

    if matrix.get("schema") != SCHEMA:
        problems.append(("<matrix>", f"schema must be {SCHEMA}"))
    try:
        required_ids = load_required_ids(base / REQUIREMENT_SOURCE)
    except MatrixError as exc:
        problems.append(("<matrix>", str(exc)))
        required_ids = frozenset()
    if not isinstance(rows, list) or not rows:
        problems.append(("<matrix>", "rows must be a non-empty list"))
        return [f"{label}: {message}" for label, message in sorted(problems)]

    source_problems: dict[str, str | None] = {}
    seen_ids: set[str] = set()
    for position, row in enumerate(rows):
        label = f"rows[{position}]"
        if not isinstance(row, dict):
            problems.append((label, "row must be an object"))
            continue

        row_id = row.get("id")
        if _is_nonempty_str(row_id):
            label = row_id
        messages: list[str] = []
        if not _is_nonempty_str(row_id):
            messages.append("id must be a non-empty string")
        elif not ROW_ID_RE.match(row_id):
            messages.append("id does not match ^[a-z0-9][a-z0-9._-]*$")
        elif row_id in seen_ids:
            messages.append("duplicate id")
        else:
            seen_ids.add(row_id)

        for key in ("source", "statement"):
            if not _is_nonempty_str(row.get(key)):
                messages.append(f"{key} must be a non-empty string")
        if "discrepancy" in row and not _is_nonempty_str(row["discrepancy"]):
            messages.append("discrepancy must be a non-empty string")
        if _is_nonempty_str(row.get("source")):
            source = row["source"]
            if source not in source_problems:
                source_problems[source] = _source_problem(base, source)
            problem = source_problems[source]
            if problem:
                messages.append(problem)
        refs = row.get("requirements", [])
        if not isinstance(refs, list) or any(not _is_nonempty_str(ref) for ref in refs):
            messages.append("requirements must be a list of non-empty ids")
        else:
            for ref in refs:
                if ref not in requirements:
                    messages.append(f"unknown requirement: {ref}")
                else:
                    mapped.add(ref)

        status = row.get("status")
        if status not in VALID_STATUSES:
            messages.append(f"unknown status: {status}")

        evidence = row.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            messages.append("evidence must be a non-empty list")
        else:
            for item in evidence:
                item_messages: list[str] = []
                _check_item(item_messages.append, item, base, index, require_landed)
                messages.extend(item_messages)
            dict_items = [item for item in evidence if isinstance(item, dict)]
            if status in VALID_STATUSES and len(dict_items) == len(evidence):
                status_messages: list[str] = []
                _check_status(status_messages.append, status, dict_items)
                messages.extend(status_messages)

        problems.extend((label, message) for message in messages)

    problems.extend((row_id, "required mapping missing") for row_id in required_ids - seen_ids)
    missing = sorted(requirements.keys() - mapped)
    problems.extend(("<inventory>", f"requirement lacks a matrix row: {ref}") for ref in missing)
    if missing or inventory_errors:
        for row in rows:
            if isinstance(row, dict) and row.get("id") == "iss.126" and row.get("status") != "planned":
                problems.append(("iss.126", "inventory incomplete: status must be planned"))

    return [f"{label}: {message}" for label, message in sorted(problems)]


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _discovered_names(path: Path) -> frozenset[str]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        tests = document["tests"]
        if document.get("kind") != "ctestInfo" or not isinstance(tests, list) or not tests:
            raise ValueError("expected a nonempty CTest discovery inventory")
        names = [test["name"] for test in tests]
        if any(not _is_nonempty_str(name) for name in names) or len(set(names)) != len(names):
            raise ValueError("invalid or duplicate discovered test identity")
        return frozenset(names)
    except (OSError, UnicodeDecodeError, ValueError, KeyError, TypeError) as exc:
        raise MatrixError(f"cannot read CTest discovery {path}: {exc}") from exc


def _executed_cases(paths: list[Path]) -> dict[str, list[bool]]:
    """Preserve every occurrence; a successful duplicate cannot hide a failure."""
    cases: dict[str, list[bool]] = {}
    for path in paths:
        try:
            root = ET.parse(path).getroot()
            elements = list(root.iter("testcase"))
            if root.tag not in {"testsuite", "testsuites"} or not elements:
                raise ValueError("expected nonempty CTest JUnit")
            for case in elements:
                name = case.get("name")
                if not _is_nonempty_str(name):
                    raise ValueError("testcase has no identity")
                passed = (
                    case.get("status") == "run"
                    and case.get("result") in {None, "completed"}
                    and all(case.find(tag) is None for tag in ("failure", "error", "skipped"))
                )
                cases.setdefault(name, []).append(passed)
        except (OSError, ET.ParseError, ValueError) as exc:
            raise MatrixError(f"cannot read CTest JUnit {path}: {exc}") from exc
    return cases


def _execution_match(ref: str, name: str, index: TestIndex) -> bool:
    if ref in index.ctest_names:
        return name == ref
    match = SUITE_REF_RE.fullmatch(ref)
    if match is None:
        return False
    suite, case = match.groups()
    case_pattern = r"[A-Za-z_]\w*" if case == "*" else re.escape(case)
    # CTest's parameterized names use Instantiation/Suite.Case/pretty-value.
    # Match the complete identity; classname is not a CTest suite prefix.
    return re.fullmatch(
        r"(?:[^/]+/)*" + re.escape(suite) + r"\." + case_pattern + r"(?:/.*)?", name
    ) is not None


def validate_execution(
    matrix: dict[str, Any], index: TestIndex, reports: list[Path], discovery: Path
) -> dict[str, Any]:
    names = _discovered_names(discovery)
    cases = _executed_cases(reports)
    results: list[dict[str, Any]] = []
    unevaluated: list[dict[str, str]] = []
    for row in matrix["rows"]:
        for item in row["evidence"]:
            if "planned" in item or item["kind"] != "ctest":
                unevaluated.append({"row": row["id"], "kind": item["kind"], "ref": item["ref"],
                                    "reason": "planned" if "planned" in item else "separate-evidence-required"})
                continue
            ref = item["ref"]
            expected = sorted(name for name in names if _execution_match(ref, name, index))
            observed = sorted(name for name in cases if _execution_match(ref, name, index))
            missing = [name for name in expected if name not in cases]
            adverse = [name for name in observed if not all(cases[name])]
            unexpected = sorted(set(observed) - set(expected))
            passed = bool(expected) and not missing and not adverse and not unexpected
            results.append({"row": row["id"], "ref": ref, "passed": passed,
                            "expected": expected, "observed": observed, "missing": missing,
                            "adverse": adverse, "unexpected": unexpected})
    return {"schema": "luminumbra.acceptance_execution.v1", "scope": "ctest-references-only",
            "ctest_references_passed": bool(results) and all(result["passed"] for result in results),
            "full_acceptance_qualified": False, "references": results, "unevaluated": unevaluated,
            "discovery_sha256": _sha256(discovery),
            "reports": [{"name": path.name, "sha256": _sha256(path)} for path in reports]}


def _ci_identity(root: Path) -> dict[str, Any]:
    """Bind job-local reports to the actual checkout; raw reports stay unbound."""
    if os.environ.get("CI") != "true" or os.environ.get("GITHUB_ACTIONS") != "true":
        raise MatrixError("--ci-context requires a GitHub Actions job")
    try:
        checkout = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        if not isinstance(event, dict):
            raise ValueError("workflow event must be an object")
        if checkout != os.environ["GITHUB_SHA"] or re.fullmatch(r"[0-9a-f]{40}", checkout) is None:
            raise ValueError("checkout differs from the workflow revision")
        if "pull_request" in event:
            head = event["pull_request"]["head"]["sha"]
        else:
            head = checkout
        if not isinstance(head, str) or re.fullmatch(r"[0-9a-f]{40}", head) is None:
            raise ValueError("invalid candidate identity")
        run_id, attempt, job = (os.environ[key] for key in
                                ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_JOB"))
        if not run_id.isdecimal() or int(run_id) <= 0 or not attempt.isdecimal() or int(attempt) <= 0 or not job:
            raise ValueError("invalid workflow run identity")
        return {"checkout_sha": checkout, "candidate_sha": head, "run_id": run_id,
                "run_attempt": attempt, "job": job}
    except (OSError, subprocess.CalledProcessError, KeyError, ValueError, TypeError) as exc:
        raise MatrixError(f"cannot bind CI execution identity: {exc}") from exc


def _escape_cell(text: str) -> str:
    return text.replace("|", "\\|")


def _render_item(item: dict[str, Any]) -> str:
    text = f"{item['kind']}: `{item['ref']}`"
    planned = item.get("planned")
    if isinstance(planned, str):
        text += f" (planned: {planned})"
    if item["kind"] == "native":
        text += f" -- command: `{item['command']}`; receipt: `{item['receipt_schema']}`"
    return _escape_cell(text)


def render_doc(matrix: dict[str, Any]) -> str:
    rows = matrix["rows"]
    counts = {status: 0 for status in STATUS_ORDER}
    for row in rows:
        counts[row["status"]] += 1

    lines = [
        "# Acceptance matrix",
        "",
        "This file is generated from `tools/ci/acceptance_matrix.json` by "
        "`tools/ci/check_acceptance_matrix.py --write-doc`. Do not edit it by hand; "
        "change the matrix and regenerate. The requirement inventory is "
        "`tools/ci/acceptance_requirements.json`; every statement in both reviewed "
        "documents is accounted for as a requirement or a justified exclusion.",
        "",
        "Statuses declare the available evidence. Static validation requires a mapping for every ID "
        "in [the contract requirement list](distant-world.md#acceptance-requirement-ids) and checks references; it does not "
        "prove execution. JUnit mode checks every referenced CTest identity in the supplied discovery "
        "roster, including parameter instances. Scripts, documents and native commands need separate "
        "evidence; a passing CTest gate does not qualify the full acceptance contract.",
        "",
        "| Status | Rows |",
        "| --- | ---: |",
    ]
    lines.extend(f"| {status} | {counts[status]} |" for status in STATUS_ORDER)
    lines.append(f"| Total | {len(rows)} |")
    lines.extend(
        [
            "",
            "| Row | Status | Source | Requirements | Statement | Evidence |",
            "| --- | --- | --- | --- | --- | --- |",
        ]
    )
    for row in rows:
        evidence = "<br>".join(_render_item(item) for item in row["evidence"])
        statement = row["statement"]
        if row.get("discrepancy"):
            statement += f" Discrepancy: {row['discrepancy']}"
        lines.append(
            f"| {_escape_cell(row['id'])} | {_escape_cell(row['status'])} "
            f"| `{_escape_cell(row['source'])}` | {_escape_cell(', '.join(row.get('requirements', [])))} "
            f"| {_escape_cell(statement)} | {evidence} |"
        )
    return "\n".join(lines) + "\n"


def _doc_matches(path: Path, expected: str) -> bool:
    try:
        raw = path.read_bytes()
    except OSError:
        return False
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return text.replace("\r\n", "\n") == expected


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check the acceptance matrix.")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--matrix", type=Path, default=None)
    parser.add_argument("--check-doc", type=Path, default=None)
    parser.add_argument("--write-doc", type=Path, default=None)
    parser.add_argument("--require-landed", action="store_true")
    parser.add_argument("--junit", type=Path, action="append", default=[])
    parser.add_argument("--discovery", type=Path)
    parser.add_argument("--execution-receipt", type=Path)
    parser.add_argument("--ci-context", action="store_true")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return int(exc.code or 0)

    root = args.root.resolve()
    if bool(args.junit) != bool(args.discovery) or ((args.execution_receipt or args.ci_context) and not args.junit):
        print("error: --junit and --discovery are required together for execution evidence", file=sys.stderr)
        return 2
    matrix_path = args.matrix if args.matrix is not None else root / "tools" / "ci" / "acceptance_matrix.json"
    try:
        matrix = load_matrix(matrix_path)
    except MatrixError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    index = index_tests(root)
    errors = validate(matrix, index, root, args.require_landed)
    if errors:
        for message in errors:
            print(f"error: {message}", file=sys.stderr)
        return 1

    if args.write_doc is not None:
        try:
            with open(args.write_doc, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(render_doc(matrix))
        except OSError as exc:
            print(f"error: cannot write {args.write_doc}: {exc}", file=sys.stderr)
            return 2

    if args.check_doc is not None and not _doc_matches(args.check_doc, render_doc(matrix)):
        print(
            f"error: {args.check_doc} is stale; regenerate with --write-doc",
            file=sys.stderr,
        )
        return 1

    if args.junit:
        try:
            receipt = validate_execution(matrix, index, args.junit, args.discovery)
            receipt["matrix_sha256"] = _sha256(matrix_path)
            receipt["identity"] = _ci_identity(root) if args.ci_context else None
            if args.execution_receipt:
                args.execution_receipt.parent.mkdir(parents=True, exist_ok=True)
                args.execution_receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        except (MatrixError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        for reference in receipt["references"]:
            if not reference["passed"]:
                print(f"error: {reference['row']}: CTest execution incomplete or failing: {reference['ref']}", file=sys.stderr)
        if not receipt["ctest_references_passed"]:
            return 1
        print(f"acceptance CTest references passed: {len(receipt['references'])}; full acceptance remains unqualified")
    else:
        print(f"acceptance matrix static references ok: {len(matrix['rows'])} rows; execution unevaluated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
