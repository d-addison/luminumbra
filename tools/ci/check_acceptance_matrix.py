#!/usr/bin/env python3
"""Validate the acceptance matrix and render its generated markdown view.

The output/preparation directories are a developer/CI-owned tree.
Non-cooperating concurrent writers in those directories are outside this tool's
threat model. The refresh lock serializes cooperating checker processes; it
does not reserve other names. Identity, content and link checks detect observed
conflicts, but do not provide identity-conditioned unlink or replacement: a
writer can substitute a pathname after verification and before mutation.
Unknown preparation/pending occupants are still refused untouched, without
adoption. Input snapshot checks and all existing refusal checks still apply.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import errno
import hashlib
import hmac
import io
import json
import os
import re
import secrets
import shlex
import stat
import subprocess
import sys
import time
import unicodedata
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterator


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
# source paragraph. Entries bind id, reviewed source#anchor, category and exact UTF-8 quote
# SHA-256. Exclusion text must equal that full quote; shorter excerpts cannot
# hide obligations. Unique exact-text relocations retain the audit and its context
# checks; new exclusions require a reviewed change to this table.
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
CMAKE_TOKEN_RE = re.compile(r'"|(?:elseif|else|endif|if)\s*\(', re.IGNORECASE)
CMAKE_COMMENT_TOKEN_RE = re.compile(r'["#]')
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


def _discovery_files(
    root: Path, *, cmake: bool = True, gtest: bool = True,
) -> tuple[list[Path], list[Path]]:
    # One traversal per source tree and one file-status query per candidate.
    # Both result sets are bounded by the number of source-tree entries.
    cmake_files: set[Path] = set()
    gtest_files: list[Path] = []
    top = root / "CMakeLists.txt"
    if cmake and top.is_file():
        cmake_files.add(top)
    for sub in ("test", "tools") if cmake else ("test",):
        base = root / sub
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            is_cmake = cmake and (path.name == "CMakeLists.txt" or path.suffix == ".cmake")
            is_gtest = gtest and sub == "test" and path.suffix in {".cpp", ".h"}
            if (is_cmake or is_gtest) and path.is_file():
                if is_cmake:
                    cmake_files.add(path)
                if is_gtest:
                    gtest_files.append(path)
    return sorted(cmake_files), sorted(gtest_files)


def _cmake_files(root: Path) -> list[Path]:
    return _discovery_files(root, gtest=False)[0]


def _gtest_sources(root: Path) -> list[Path]:
    return _discovery_files(root, cmake=False)[1]


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _strip_cmake(text: str) -> str:
    """Remove CMake comments; '#' inside a quoted argument is not a comment."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        token = CMAKE_COMMENT_TOKEN_RE.search(text, i)
        if token is None:
            out.append(text[i:])
            break
        out.append(text[i:token.start()])
        i = token.start()
        ch = token.group()
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
        token = CMAKE_TOKEN_RE.search(text, i)
        if token is None:
            break
        i = token.start()
        if token.group() == '"':
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
    cmake_files, gtest_files = _discovery_files(root)
    ctest_names: set[str] = set()
    for path in cmake_files:
        ctest_names.update(_source_names(_read_text(path), cmake=True))
    gtest_cases: set[tuple[str, str]] = set()
    for path in gtest_files:
        gtest_cases.update(_source_names(_read_text(path), cmake=False))
    return TestIndex(
        frozenset(ctest_names),
        frozenset(gtest_cases),
        frozenset(suite for suite, _ in gtest_cases),
    )


def _source_names(text: str, *, cmake: bool) -> frozenset[Any]:
    # Always read and discover sources afresh. Only immutable parsing results
    # are shared; equal size/timestamps or a rebound name cannot hide new text.
    # Retain at most 256 texts of 64 Ki characters (16 Mi source characters).
    if len(text) > 64 * 1024:
        return _parse_source_names(text, cmake=cmake)
    return _cached_source_names(text, cmake=cmake)


@lru_cache(maxsize=256)
def _cached_source_names(text: str, *, cmake: bool) -> frozenset[Any]:
    return _parse_source_names(text, cmake=cmake)


def _parse_source_names(text: str, *, cmake: bool) -> frozenset[Any]:
    if cmake:
        text = _drop_disabled_cmake(_strip_cmake(text))
        names = (match.group("quoted") if match.group("quoted") is not None else match.group("bare")
                 for match in ADD_TEST_RE.finditer(text))
        return frozenset(name for name in names if _is_ctest_name(name))
    text = _drop_disabled_c(_strip_c(text))
    return frozenset((match.group(1), match.group(2)) for match in GTEST_RE.finditer(text))


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


def _string_problem(value: Any, field: str) -> str | None:
    return None if _is_nonempty_str(value) else f"{field} must be a non-empty string"


def _path_problem(
    base: Path, ref: str, must_exist: bool, *, resolved_base: Path | None = None,
) -> str | None:
    if ref.startswith("/") or re.match(r"^[A-Za-z]:", ref) or PurePosixPath(ref).is_absolute():
        return f"path must be repo-relative: {ref}"
    if ".." in re.split(r"[\\/]", ref):
        return f"path escapes root: {ref}"
    base = base.resolve() if resolved_base is None else resolved_base
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


@lru_cache(maxsize=16)
def _heading_items(text: str) -> tuple[tuple[int, str], ...]:
    # Bound retained document versions; content keys keep edits visible.
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
    return tuple(headings.items())


def _headings(text: str) -> dict[int, str]:
    return dict(_heading_items(text))


def _source_problem(
    base: Path, source: str,
    documents: dict[str, tuple[str | None, frozenset[str] | None]],
) -> str | None:
    if source.startswith("issue"):
        if re.fullmatch(r"issue #[1-9][0-9]*", source) is None:
            return f"invalid issue source: {source}"
        return None
    path, separator, anchor = source.partition("#")
    if path not in documents:
        documents[path] = (_path_problem(base, path, must_exist=True), None)
    problem, anchors = documents[path]
    if problem:
        return f"source {problem}"
    if separator:
        if anchors is None:
            try:
                anchors = frozenset(_headings(_read_text(base / path)).values())
            except OSError as exc:
                return f"source cannot read {path}: {exc}"
            documents[path] = (None, anchors)
        if anchor not in anchors:
            return f"source anchor not found: {source}"
    return None


def contract_statements(text: str, path: str) -> list[tuple[str, str, bool]]:
    return list(_contract_statements(text, path))


@lru_cache(maxsize=16)
def _contract_statements(text: str, path: str) -> tuple[tuple[str, str, bool], ...]:
    """Account for all prose and table data, not just sentences with 'must'.

    Soft wraps are normalized. Prose, bullets and table rows split at sentence
    and semicolon boundaries. Headings and table scaffolding are not statements.
    Current-behaviour paragraphs are explicitly descriptive.
    Inventory ids are maintained independently of these content units.
    """
    # Bound retained versions and return immutable units to all validations.
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
    return tuple(result)


def validate_inventory(
    inventory: dict[str, Any], root: Path
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Require an explicit requirement or justified exclusion for every unit."""
    errors: list[str] = []
    base = root.resolve()
    requirements: dict[str, dict[str, Any]] = {}
    expected: Counter[tuple[str, str]] = Counter()
    descriptive: set[tuple[str, str]] = set()
    if inventory.get("schema") != INVENTORY_SCHEMA:
        errors.append(f"inventory schema must be {INVENTORY_SCHEMA}")
    if inventory.get("documents") != list(CONTRACT_DOCUMENTS):
        errors.append("inventory documents must name both reviewed contract documents")
    for path in CONTRACT_DOCUMENTS:
        problem = _path_problem(root, path, must_exist=True, resolved_base=base)
        if problem:
            errors.append(problem)
            continue
        try:
            document = (root / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            errors.append(f"cannot read contract {path}: {exc}")
            continue
        for source, text, is_descriptive in contract_statements(document, path):
            expected[(source, text)] += 1
            if is_descriptive:
                descriptive.add((source, text))
    source_problems: dict[str, str | None] = {}
    documents: dict[str, tuple[str | None, frozenset[str] | None]] = {}
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
                source_problems[source] = _source_problem(root, source, documents)
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
                    and _audited_locator(identity, source, category, quote, expected)
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
                            and all(entry.get(key) == override[key] for key in ("text", "category"))
                            and _audited_locator(identity, source, category, quote, expected)):
                        errors.append(f"{label}: normative exclusion requires an exact reviewed override")
    for (source, text), count in (expected - actual).items():
        errors.append(f"inventory unaccounted statement ({count}): {source}: {text[:100]}")
    for (source, text), count in (actual - expected).items():
        errors.append(f"inventory stale or duplicate statement ({count}): {source}: {text[:100]}")
    if not requirements:
        errors.append("inventory requirements must not be empty")
    return requirements, errors


def _audited_locator(
    identity: str, source: str, category: str, quote: str,
    expected: Counter[tuple[str, str]],
) -> bool:
    """An exact reviewed quote can follow a unique relocation, never a copy."""
    digest = hashlib.sha256(quote.encode("utf-8")).hexdigest()
    if (identity, source, category, digest) in AUDITED_EXCLUSIONS:
        return True
    return (
        expected[(source, quote)] == 1
        and sum(count for (_, text), count in expected.items() if text == quote) == 1
        and any(
            (identity, category, digest) == (old_id, old_category, old_digest)
            and not expected[(old_source, quote)]
            for old_id, old_source, old_category, old_digest in AUDITED_EXCLUSIONS
        )
    )


def _inventory_units(root: Path) -> Counter[tuple[str, str]]:
    units: Counter[tuple[str, str]] = Counter()
    base = root.resolve()
    for path in CONTRACT_DOCUMENTS:
        problem = _path_problem(root, path, must_exist=True, resolved_base=base)
        if problem:
            raise MatrixError(problem)
        try:
            document = (root / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            raise MatrixError(f"cannot read contract {path}: {exc}") from exc
        units.update((source, text) for source, text, _ in contract_statements(document, path))
    return units


def _candidate_rows(matrix: dict[str, Any], source: str, identity: str = "") -> list[dict[str, Any]]:
    rows = matrix.get("rows", [])
    if not isinstance(rows, list):
        return []
    return sorted(
        ({key: sorted(row.get(key, [])) if key == "requirements" else row.get(key)
          for key in ("id", "source", "status", "requirements")}
         for row in rows if isinstance(row, dict)
         and (identity in row.get("requirements", [])
              or str(row.get("source", "")).partition("#")[0] == source.partition("#")[0])),
        key=lambda row: str(row["id"]),
    )


def refresh_inventory(
    inventory: dict[str, Any], matrix: dict[str, Any], root: Path,
) -> tuple[dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    """Pair exact statement text only, after reserving unchanged occurrences.

    No fuzzy matching, new accounting, retirement, or status changes are allowed.
    A duplicated old locator cannot identify which stable ID moved.
    """
    expected = _inventory_units(root)
    remaining = expected.copy()
    refreshed = copy.deepcopy(inventory)
    updated_matrix = copy.deepcopy(matrix)
    entries: list[tuple[str, dict[str, Any]]] = []
    rows = matrix.get("rows")
    if (not isinstance(rows, list) or any(
            not isinstance(row, dict) or not _is_nonempty_str(row.get("id"))
            or not ROW_ID_RE.fullmatch(row["id"])
            or not isinstance(row.get("requirements", []), list)
            or any(not _is_nonempty_str(ref) for ref in row.get("requirements", []))
            for row in rows)
            or len({row["id"] for row in rows}) != len(rows)):
        raise MatrixError("refresh requires valid matrix row identities and requirement lists")
    for row in rows:
        problem = _string_problem(row.get("source"), "source")
        if problem:
            raise MatrixError(f"{row['id']}: {problem}")
    if (inventory.get("schema") != INVENTORY_SCHEMA
            or inventory.get("documents") != list(CONTRACT_DOCUMENTS)):
        raise MatrixError("refresh requires the reviewed inventory schema and documents")
    for kind in ("requirements", "exclusions"):
        values = refreshed.get(kind)
        if not isinstance(values, list):
            raise MatrixError(f"inventory {kind} must be a list")
        for entry in values:
            if (not isinstance(entry, dict)
                    or not all(_is_nonempty_str(entry.get(key)) for key in ("id", "source", "text", "quote"))
                    or not ROW_ID_RE.fullmatch(entry["id"])
                    or entry["quote"] not in entry["text"]):
                raise MatrixError(f"refresh requires valid {kind} entries")
            if entry["source"].partition("#")[0] not in CONTRACT_DOCUMENTS:
                raise MatrixError("refresh requires inventory locators in the reviewed documents")
            entries.append((kind, entry))
    if len({entry["id"] for _, entry in entries}) != len(entries):
        raise MatrixError("refresh refuses duplicate inventory ids")
    entries.sort(key=lambda item: item[1]["id"])
    old_counts = Counter((entry["source"], entry["text"]) for _, entry in entries)
    unmatched: list[tuple[str, dict[str, Any]]] = []
    for kind, entry in entries:
        key = (entry["source"], entry["text"])
        if remaining[key]:
            remaining[key] -= 1
        else:
            unmatched.append((kind, entry))
    old_by_text: dict[str, list[tuple[str, dict[str, Any]]]] = {}
    new_by_text: dict[str, list[str]] = {}
    for kind, entry in unmatched:
        old_by_text.setdefault(entry["text"], []).append((kind, entry))
    for (source, text), count in remaining.items():
        if count:
            new_by_text.setdefault(text, []).extend([source] * count)
    moved: set[str] = set()
    for text, old in sorted(old_by_text.items()):
        targets = new_by_text.get(text, [])
        if len(old) != 1 or len(targets) != 1:
            continue
        kind, entry = old[0]
        if old_counts[(entry["source"], text)] != 1:
            continue
        target = targets[0]
        if kind == "exclusions":
            candidate = dict(entry, source=target)
            # Reuse all audit and context checks before moving any exclusion.
            trial = copy.deepcopy(refreshed)
            trial["exclusions"] = [candidate if item["id"] == entry["id"] else item
                                   for item in trial["exclusions"]]
            if any(error.startswith(entry["id"] + ":")
                   for error in validate_inventory(trial, root)[1]):
                continue
        entry["source"] = target
        remaining[(target, text)] -= 1
        moved.add(entry["id"])
    decisions: list[dict[str, Any]] = []
    suggested_ids = {entry["id"] for _, entry in entries}
    for (source, text), count in sorted(remaining.items()):
        if not count:
            continue
        digest = hashlib.sha256((source + "\n" + text).encode("utf-8")).hexdigest()[:16]
        stem = "statement." + digest
        suffix = 1
        for _ in range(count):
            identity = stem if suffix == 1 else f"{stem}.{suffix}"
            while identity in suggested_ids:
                suffix += 1
                identity = f"{stem}.{suffix}"
            suggested_ids.add(identity)
            decisions.append({"decision": "unaccounted", "kind": "requirements",
                              "entry": {"id": identity, "source": source,
                                        "text": text, "quote": text},
                              "candidate_rows": _candidate_rows(matrix, source)})
    for kind, entry in unmatched:
        if entry["id"] not in moved:
            decisions.append({"decision": "removed-or-reworded-or-ambiguous", "kind": kind,
                              "entry": entry,
                              "candidate_rows": _candidate_rows(matrix, entry["source"], entry["id"])})
    by_id = {entry["id"]: entry for _, entry in entries}
    mapped = {ref for row in rows for ref in row.get("requirements", [])}
    stale_ids = {entry["id"] for _, entry in unmatched if entry["id"] not in moved}
    for kind, entry in entries:
        if kind == "requirements" and entry["id"] not in mapped and entry["id"] not in stale_ids:
            decisions.append({"decision": "unmapped-requirement", "kind": kind, "entry": entry,
                              "candidate_rows": _candidate_rows(matrix, entry["source"], entry["id"])})
    old_by_id = {entry["id"]: entry for kind in ("requirements", "exclusions")
                 for entry in inventory[kind]}
    for row in updated_matrix.get("rows", []):
        if not isinstance(row, dict) or not isinstance(row.get("requirements"), list):
            continue
        refs = row["requirements"]
        if not refs or not any(ref in moved for ref in refs) or any(ref not in by_id for ref in refs):
            continue
        sources = {by_id[ref]["source"] for ref in refs}
        old_sources = {old_by_id[ref]["source"] for ref in refs}
        if len(sources) == 1 and row.get("source") in old_sources:
            row["source"] = next(iter(sources))
        elif row.get("source") in old_sources:
            decisions.append({"decision": "row-source", "entry": row,
                              "candidate_sources": sorted(sources),
                              "candidate_rows": _candidate_rows(matrix, row["source"], refs[0])})
    if moved:
        for kind in ("requirements", "exclusions"):
            refreshed[kind].sort(key=lambda entry: entry["id"])
        updated_matrix["rows"].sort(key=lambda row: row["id"])
        for row in updated_matrix["rows"]:
            if "requirements" in row:
                row["requirements"].sort()
    decisions.sort(key=lambda decision: json.dumps(decision, sort_keys=True))
    return refreshed, updated_matrix, decisions


def _refresh_lock_path(root: Path, *, required: bool = True) -> Path | None:
    """Keep the persistent lock outside the public tree, including worktrees."""
    root = root.resolve()
    metadata = root / ".git"
    located = metadata.is_file()
    if located:
        locator = metadata.read_text(encoding="utf-8").strip()
        if not locator.startswith("gitdir: ") or not locator[8:].strip():
            raise MatrixError("cannot locate refresh lock: invalid Git directory locator")
        metadata = root / locator[8:]
    # Canonicalize directory-form metadata too (symlinks, junctions and
    # relative roots), before deriving any cooperating transaction pathname.
    metadata = metadata.resolve()
    if located and metadata.is_relative_to(root):
        raise MatrixError("refresh lock requires Git metadata outside the public tree")
    if not metadata.is_dir():
        if not required:
            return None
        raise MatrixError("inventory refresh requires a Git metadata directory")
    return metadata / "acceptance-refresh.lock"


def _try_refresh_lock(handle: Any, *, shared: bool = False) -> None:
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        import msvcrt
        # Lock one byte even in an empty file. Never read or write this mandatory
        # locked range through another handle, or rename/unlink the lock file.
        # CRT read-lock modes are exclusive too; LockFileEx supplies shared locks.
        class Overlapped(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                        ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                        ("hEvent", wintypes.HANDLE)]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        acquire = kernel.LockFileEx
        acquire.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                            wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(Overlapped)]
        acquire.restype = wintypes.BOOL
        # FAIL_IMMEDIATELY (1), plus EXCLUSIVE_LOCK (2) for writers only.
        overlapped = Overlapped()
        if not acquire(msvcrt.get_osfhandle(handle.fileno()), 1 if shared else 3,
                       0, 1, 0, ctypes.byref(overlapped)):
            error = ctypes.get_last_error()
            if error == 33:  # ERROR_LOCK_VIOLATION: retry under the same deadline.
                raise OSError(errno.EACCES, "inventory refresh lock is held")
            raise ctypes.WinError(error)
    else:
        import fcntl
        fcntl.flock(handle.fileno(), (fcntl.LOCK_SH if shared else fcntl.LOCK_EX) | fcntl.LOCK_NB)


@contextlib.contextmanager
def _refresh_lock(root: Path, *, shared: bool = False, timeout: float = 2.0) -> Iterator[bool]:
    # Kernel ownership, rather than a PID or age check, makes a crashed holder
    # harmless and prevents stealing a live holder's lock. Closing releases it.
    path = _refresh_lock_path(root, required=not shared)
    if path is None:
        yield False
        return
    try:
        # Existing readers need only read access, including on Windows.
        handle = path.open("rb" if shared else "a+b")
    except FileNotFoundError:
        if not shared:
            raise
        try:
            # Both platforms lock the same persistent inode even when a reader
            # and the first writer create it concurrently. Never truncate it.
            descriptor = os.open(path, os.O_RDONLY | os.O_CREAT, 0o600)
        except OSError as exc:
            if exc.errno not in (errno.EACCES, errno.EPERM, errno.EROFS):
                raise
            print(f"warning: cannot create inventory refresh lock; using detect-and-retry: {exc}",
                  file=sys.stderr)
            yield False
            return
        try:
            handle = os.fdopen(descriptor, "rb")
        except BaseException:
            os.close(descriptor)
            raise
    with handle:
        deadline = time.monotonic() + timeout
        while True:
            try:
                _try_refresh_lock(handle, shared=shared)
                break
            except OSError as exc:
                if exc.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                    raise
                if time.monotonic() >= deadline:
                    raise MatrixError("cannot acquire inventory refresh lock within deadline") from exc
                time.sleep(min(0.02, max(0.0, deadline - time.monotonic())))
        yield True


class _ReadDiagnostics(io.StringIO):
    # A speculative pass retains at most 256 Ki characters per stream. If no
    # lock appears, overflow is an explicit refusal, never truncated success.
    LIMIT = 256 * 1024

    def __init__(self) -> None:
        super().__init__()
        self.overflow = False

    def write(self, text: str) -> int:
        if self.overflow or len(text) > self.LIMIT - self.tell():
            self.overflow = True
            return len(text)
        return super().write(text)


class _RefreshSnapshot:
    """Bind validation to input bytes and the complete discovery file sets."""
    def __init__(self, root: Path, paths: list[Path], *, aliases: dict[Path, Path] | None = None) -> None:
        self.root = root
        self.discovery = self._discover()
        self.fingerprints: dict[Path, tuple[Path, bytes | None]] = {}
        self.identities: dict[Path, tuple[int, int, int] | None] = {}
        self.add([*paths, *sorted(self.discovery), *(root / path for path in CONTRACT_DOCUMENTS)])
        for original, target in (aliases or {}).items():
            path = original.absolute()
            # Bind the name to the destination selected before lock-overlap
            # checks. Never read through a newly retargeted alias here.
            if path.resolve() != target:
                raise MatrixError("refresh input snapshot changed; retry after source edits finish")
            if path not in self.fingerprints:
                self.identities[path] = self._identity(path)
                self.fingerprints[path] = self.fingerprints[target]

    def _discover(self) -> set[Path]:
        cmake_files, gtest_files = _discovery_files(self.root)
        return set(cmake_files + gtest_files)

    @staticmethod
    def _fingerprint(path: Path) -> bytes | None:
        try:
            # The same closed-file reader bounds allocation, rejects devices
            # without blocking, and checks bytes/identity through its handle.
            return hashlib.sha256(_read_transaction_file(path, _TRANSACTION_BYTES, sole_link=False)[0]).digest()
        except FileNotFoundError:
            return None

    @staticmethod
    def _identity(path: Path) -> tuple[int, int, int] | None:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return None
        # lstat supplies real IDs on Windows; DirEntry.stat may return zeros.
        if not metadata.st_ino:
            raise MatrixError(f"snapshot file identity unavailable: {path}")
        return metadata.st_dev, metadata.st_ino, stat.S_IFMT(metadata.st_mode)

    def add(self, paths: list[Path]) -> None:
        for path in paths:
            path = path.absolute()
            if path not in self.fingerprints:
                self.identities[path] = self._identity(path)
                target = path.resolve()
                self.fingerprints[path] = (target, self._fingerprint(target))

    def verify(self, published: dict[Path, bytes]) -> None:
        if (self._discover() != self.discovery or any(
                path.resolve() != target or self._fingerprint(target) != published.get(target, fingerprint)
                # Publishing replaces regular files, but must preserve links
                # used to reach them. Unpublished names retain their identities
                # even when an external replacement has identical bytes.
                or ((target not in published or (self.identities[path] is not None
                                                and stat.S_ISLNK(self.identities[path][2])))
                    and self._identity(path) != self.identities[path])
                for path, (target, fingerprint) in self.fingerprints.items())):
            raise MatrixError("refresh input snapshot changed; retry after source edits finish")


# Recovery has at most five destinations (three defaults, document, receipt),
# a 64 KiB journal/preparation marker and 16 MiB per file. At most three files
# per destination (backup, publication, restoration) are retained. Fixed markers
# make incomplete preparation discoverable without a directory scan or retry.
_TRANSACTION_OUTPUTS = 5
_TRANSACTION_BYTES = 16 * 1024 * 1024
_JOURNAL_BYTES = 64 * 1024
# Five destinations, three artifacts each, and two journal candidates: each
# gets at most intention/enrollment/completion records, within 64 total events.
_PREPARATION_EVENTS = 64


@lru_cache(maxsize=1)
def _windows_file_information() -> tuple[Any, Any, Any]:
    """Declare the native ABI once, rather than for each recovery file read."""
    import ctypes
    from ctypes import wintypes

    class FileIdentity(ctypes.Structure):
        _fields_ = [("volume", ctypes.c_ulonglong), ("identity", ctypes.c_ubyte * 16)]

    class FileStandard(ctypes.Structure):
        _fields_ = [("allocation", ctypes.c_longlong), ("size", ctypes.c_longlong),
                    ("links", wintypes.DWORD), ("deleted", ctypes.c_ubyte),
                    ("directory", ctypes.c_ubyte)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    query = kernel.GetFileInformationByHandleEx
    query.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    query.restype = wintypes.BOOL
    return query, FileIdentity, FileStandard


def _transaction_metadata(handle: Any) -> os.stat_result:
    metadata = os.fstat(handle.fileno())
    if os.name != "nt":
        return metadata
    # CRT fstat can report zero identity/link fields. Query the owning handle,
    # including the full 128-bit file ID used by modern Windows filesystems.
    import ctypes
    import msvcrt

    query, FileIdentity, FileStandard = _windows_file_information()
    identity, standard = FileIdentity(), FileStandard()
    native = msvcrt.get_osfhandle(handle.fileno())
    if (not query(native, 18, ctypes.byref(identity), ctypes.sizeof(identity))
            or not query(native, 1, ctypes.byref(standard), ctypes.sizeof(standard))):
        raise ctypes.WinError(ctypes.get_last_error())
    values = list(metadata)
    values[1] = int.from_bytes(bytes(identity.identity), "little")
    values[2], values[3] = identity.volume, standard.links
    return os.stat_result(values, {"st_mtime_ns": metadata.st_mtime_ns,
                                   "st_ctime_ns": metadata.st_ctime_ns})


def _same_transaction_identity(path: Path, expected: os.stat_result) -> bool:
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode):
        return False
    if os.name != "nt":
        return os.path.samestat(before, expected)
    # Use the same handle representation on both sides, independent of Python's
    # lstat ID width. No handle remains open across rename/unlink on Windows.
    with os.fdopen(os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0)), "rb") as handle:
        actual = _transaction_metadata(handle)
        return os.path.samestat(actual, expected) and os.path.samestat(before, path.lstat())


def _read_transaction_file(path: Path, limit: int, *, sole_link: bool = True) -> tuple[bytes, os.stat_result]:
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags), "rb") as handle:
        before = _transaction_metadata(handle)
        if not stat.S_ISREG(before.st_mode) or not before.st_ino or before.st_size > limit:
            raise MatrixError(f"invalid recovery file or size: {path}")
        if sole_link and before.st_nlink != 1:
            raise MatrixError(f"recovery file requires a sole link; transaction retained: {path}")
        # Allocate from the verified size, with one byte to detect growth;
        # even an empty file never requests the full 16 MiB file bound.
        data = handle.read(before.st_size + 1)
        after = _transaction_metadata(handle)
        if sole_link and (after.st_nlink != 1 or path.lstat().st_nlink != 1):
            raise MatrixError(f"recovery file requires a sole link; transaction retained: {path}")
        same_identity = _same_transaction_identity(path, before)
    if (len(data) > limit or len(data) != before.st_size
            or (before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            != (after.st_size, after.st_mtime_ns, after.st_ctime_ns)
            or not same_identity):
        raise MatrixError(f"recovery file changed: {path}")
    return data, before


def _transaction_record(path: Path, limit: int | None = None) -> dict[str, Any]:
    data, metadata = _read_transaction_file(path, _TRANSACTION_BYTES if limit is None else limit)
    return {"path": str(path), "device": metadata.st_dev, "inode": metadata.st_ino,
            "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def _verify_transaction_file(record: dict[str, Any], limit: int | None = None) -> Path:
    path = Path(record["path"])
    actual = _transaction_record(path, limit)
    if actual != record:
        raise MatrixError(f"recovery file identity or content changed: {path}")
    return path


def _remove_transaction_file(record: dict[str, Any], limit: int | None = None) -> None:
    # Both platform paths below rely on the module's cooperating-writer model.
    # Verification precedes named mutation; it is not identity-conditioned unlink.
    artifact = _verify_transaction_file(record, limit)
    try:
        artifact.unlink()
    except PermissionError:
        # Windows read-only candidates cannot be unlinked directly. Recheck
        # the retained identity and bytes before both chmod and removal.
        _verify_transaction_file(record, limit).chmod(stat.S_IRUSR | stat.S_IWUSR)
        _verify_transaction_file(record, limit).unlink()


def _written_transaction_prefix(
    path: Path, data: bytes, identity: os.stat_result, limit: int | None = None,
) -> dict[str, Any]:
    # Failed flush/close can leave any prefix, including an empty file. Read at
    # most the 16 MiB file bound, and recognize only bytes this call supplied
    # through its retained identity; an external edit is never adopted.
    written, actual = _read_transaction_file(path, _TRANSACTION_BYTES if limit is None else limit)
    if (not identity.st_ino or not os.path.samestat(actual, identity)
            or len(written) > len(data) or written != memoryview(data)[:len(written)]):
        raise MatrixError(f"temporary output identity or content changed: {path}")
    return {"path": str(path), "device": identity.st_dev, "inode": identity.st_ino,
            "size": len(written), "sha256": hashlib.sha256(written).hexdigest()}


def _preparation_pending(journal: Path | None) -> bool:
    # Even an empty marker or a dangling alias must stop readers and writers.
    # Never open or interpret a pre-existing preparation file as ownership.
    return journal is not None and any(os.path.lexists(journal.with_suffix(suffix))
                                       for suffix in (".preparation", ".pending"))


class _OutputPreparation:
    """Legacy transient intentions; only this live call can clear them.

    A restart cannot prove ownership across an interrupted exclusive create.
    Fixed markers therefore refuse recovery, preserving every byte, until the
    uncertainty is resolved externally. Completed journals remain v1/v2.
    """
    def __init__(self, path: Path) -> None:
        self.path = path
        self.identity: os.stat_result | None = None
        self.data = b""
        self.uncertain = False
        self.pending: dict[str, Any] | None = None

    def append(self, event: dict[str, Any]) -> None:
        # At most fifteen artifacts (including direct-call rollback), with two
        # records each, share the existing 64 KiB metadata bound. No scan/retry.
        addition = json.dumps(event, sort_keys=True).encode("utf-8") + b"\n"
        if len(addition) > _JOURNAL_BYTES - len(self.data):
            raise MatrixError("recovery preparation exceeds its bound")
        flags = os.O_RDWR | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
        if self.identity is None:
            flags |= os.O_CREAT | os.O_EXCL
        try:
            descriptor = os.open(self.path, flags, 0o600)
        except FileExistsError as exc:
            raise MatrixError(f"uncertain recovery preparation; bytes retained: {self.path}") from exc
        try:
            handle = os.fdopen(descriptor, "r+b")
        except BaseException:
            os.close(descriptor)
            self.uncertain = True
            raise
        with handle:
            actual = _transaction_metadata(handle)
            if (not stat.S_ISREG(actual.st_mode) or not actual.st_ino or actual.st_nlink != 1
                    or not _same_transaction_identity(self.path, actual)
                    or (self.identity is not None and not os.path.samestat(actual, self.identity))):
                self.uncertain = True
                raise MatrixError("recovery preparation identity changed; bytes retained")
            if self.identity is None:
                self.identity = actual
            if actual.st_size != len(self.data) or handle.read(len(self.data) + 1) != self.data:
                self.uncertain = True
                raise MatrixError("recovery preparation content changed; bytes retained")
            self.data += addition
            handle.seek(0)
            if handle.write(self.data) != len(self.data):
                raise OSError("incomplete recovery preparation write")
            handle.flush()
            os.fsync(handle.fileno())
        _verify_transaction_file(self.record(), _JOURNAL_BYTES)

    def record(self) -> dict[str, Any]:
        if self.identity is None:
            raise MatrixError("recovery preparation identity unavailable; bytes retained")
        return {"path": str(self.path), "device": self.identity.st_dev, "inode": self.identity.st_ino,
                "size": len(self.data), "sha256": hashlib.sha256(self.data).hexdigest()}

    def intend(self, artifact: Path, destination: Path, data: bytes) -> None:
        self.append({"create": str(artifact), "destination": str(destination),
                     "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})

    def bind(self, record: dict[str, Any]) -> None:
        _verify_transaction_file(record)
        self.append({"bound": record})

    def enroll(self, artifact: Path, identity: os.stat_result) -> None:
        # Transient preparations retain their original fail-closed semantics.
        pass

    def cancel(self, artifact: Path) -> None:
        pass

    def clear_marker(self) -> None:
        if self.uncertain:
            raise MatrixError(f"uncertain recovery preparation; bytes retained: {self.path}")
        if self.identity is not None:
            if self.path.lstat().st_nlink != 1:
                raise MatrixError("recovery preparation link count changed; bytes retained")
            _remove_transaction_file(_written_transaction_prefix(
                self.path, self.data, self.identity, _JOURNAL_BYTES), _JOURNAL_BYTES)
            self.identity = None
            self.data = b""

    def handoff(self, record: dict[str, Any]) -> None:
        # A fully bound fixed pending journal takes over discovery BEFORE the
        # preparation marker is removed. A crash on either side is discoverable.
        _verify_transaction_file(record, _JOURNAL_BYTES)
        self.pending = record
        self.clear_marker()

    def finish(self) -> None:
        self.clear_marker()
        if self.pending is not None:
            _remove_transaction_file(self.pending, _JOURNAL_BYTES)
            self.pending = None


def _custody_json(value: Any) -> bytes:
    # Measure the exact ASCII JSON encoding before allocating it. Both traversal
    # and output are bounded by 64 KiB; nesting is capped at 64 levels. Native
    # identities need at most 128 bits, so integer conversion is bounded too.
    remaining = _JOURNAL_BYTES

    def reserve(size: int) -> None:
        nonlocal remaining
        if size > remaining:
            raise MatrixError("recovery preparation exceeds its bound")
        remaining -= size

    def measure(item: Any, depth: int = 0) -> None:
        if depth > 64:
            raise MatrixError("recovery preparation nesting exceeds its bound")
        if isinstance(item, str):
            if len(item) > remaining:
                raise MatrixError("recovery preparation exceeds its bound")
            reserve(2)
            for char in item:
                reserve(2 if char in '"\\\b\f\n\r\t' else 1 if 0x20 <= ord(char) < 0x7f
                        else 6 if ord(char) <= 0xffff else 12)
        elif item is None or type(item) is bool:
            reserve(4 if item is None or item is True else 5)
        elif type(item) is int and item.bit_length() <= 128:
            reserve(len(str(item)))
        elif isinstance(item, (dict, list)):
            reserve(2 + max(0, len(item) - 1))
            for key in item:
                if isinstance(item, dict):
                    if not isinstance(key, str):
                        raise MatrixError("invalid preparation JSON key")
                    measure(key, depth + 1)
                    reserve(1)
                    measure(item[key], depth + 1)
                else:
                    measure(key, depth + 1)
        else:
            raise MatrixError("invalid preparation JSON value")

    measure(value)
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


class _PreparationCustody(_OutputPreparation):
    """Explicit caller receipt is the enrollment authority, never the filename.

    The independent receipt binds the marker's native identity and authenticates
    a bounded event chain. Identity enrollment precedes payload writes. A crash
    between exclusive creation and enrollment leaves UNKNOWN bytes untouched.
    An enrolled identity grants reset authority over that staging file's content;
    it never grants authority over a different inode or an output destination.
    All named mutations still require the cooperating-writer threat model.
    """
    def __init__(self, journal: Path, receipt: str) -> None:
        journal = journal.resolve()
        super().__init__(journal.with_suffix(".preparation"))
        if len(receipt) > _JOURNAL_BYTES:
            raise MatrixError("invalid preparation custody receipt")
        try:
            value = json.loads(receipt)
            if (not isinstance(value, dict) or set(value) != {
                    "version", "journal", "device", "inode", "nonce", "key"}
                    or type(value["version"]) is not int or value["version"] != 1
                    or value["journal"] != str(journal)
                    or any(type(value[k]) is not int for k in ("device", "inode"))
                    or value["device"] < 0 or value["inode"] <= 0
                    or any(not isinstance(value[k], str) or not re.fullmatch(r"[0-9a-f]{64}", value[k])
                           for k in ("nonce", "key"))):
                raise ValueError("invalid receipt fields")
        except (ValueError, TypeError, RecursionError) as exc:
            raise MatrixError("invalid preparation custody receipt") from exc
        self.journal = journal
        self.key = bytes.fromhex(value.pop("key"))
        self.enrollment = {"enrollment": value}
        self.events: list[dict[str, Any]] = []
        self.chain = b""
        raw, identity = _read_transaction_file(self.path, _JOURNAL_BYTES)
        if ((identity.st_dev, identity.st_ino) != (value["device"], value["inode"])
                or identity.st_nlink != 1):
            raise MatrixError("foreign or stale preparation custody; bytes retained")
        self.identity = identity
        self.data = raw
        # splitlines would accept CR and other separators. Only LF commits an
        # event; a torn final suffix carries no enrollment/commit authority.
        lines = raw.split(b"\n")
        if len(lines) > _PREPARATION_EVENTS + 1:
            raise MatrixError("recovery preparation exceeds its record bound")
        self.complete = b""
        try:
            for line in lines[:-1]:
                entry = json.loads(line)
                if not isinstance(entry, dict) or set(entry) != {"event", "mac"}:
                    raise ValueError("unknown event metadata")
                event = entry["event"]
                expected = hmac.digest(self.key, self.chain + _custody_json(event), "sha256")
                if not isinstance(entry["mac"], str) or not hmac.compare_digest(entry["mac"], expected.hex()):
                    raise ValueError("event authentication failed")
                if not self.events and event != self.enrollment:
                    raise ValueError("enrollment differs from receipt")
                self.events.append(event)
                self.chain = expected
                self.complete += line + b"\n"
            if not self.events:
                raise ValueError("enrollment was not committed")
        except (ValueError, TypeError, KeyError, RecursionError) as exc:
            raise MatrixError(f"uncertain recovery preparation; bytes retained: {exc}") from exc

    @classmethod
    def create(cls, journal: Path, journal_record: dict[str, Any] | None = None,
               *, receipt_file: Path | None = None) -> str:
        journal = journal.resolve()
        path = journal.with_suffix(".preparation")
        if journal_record is not None:
            if Path(journal_record["path"]) != journal:
                raise MatrixError("preparation enrollment journal differs from validated recovery")
            _verify_transaction_file(journal_record, _JOURNAL_BYTES)
        if (any(os.path.lexists(p) for p in (path, journal.with_suffix(".pending")))
                or journal_record is None and os.path.lexists(journal)):
            raise MatrixError("uncertain recovery preparation; enrollment requires empty recovery state")
        # CLI enrollment stages beside the journal for atomic publication.
        # A crash before receipt persistence cannot publish a recovery marker.
        # Legacy direct helpers retain their explicit UNKNOWN failure contract.
        staged = (path.parent / (".acceptance-enrollment-" + secrets.token_hex(16))
                  if receipt_file is not None else path)
        identity = None
        data = b""
        try:
            descriptor = os.open(staged, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                handle = os.fdopen(descriptor, "w+b")
            except BaseException:
                os.close(descriptor)
                raise
            with handle:
                identity = _transaction_metadata(handle)
                if not identity.st_ino or identity.st_nlink != 1 or not _same_transaction_identity(staged, identity):
                    raise MatrixError("preparation enrollment identity unavailable; bytes retained")
                receipt = {"version": 1, "journal": str(journal), "device": identity.st_dev,
                           "inode": identity.st_ino, "nonce": secrets.token_hex(32), "key": secrets.token_hex(32)}
                event = {"enrollment": {k: v for k, v in receipt.items() if k != "key"}}
                mac = hmac.digest(bytes.fromhex(receipt["key"]), _custody_json(event), "sha256")
                data = _custody_json({"event": event, "mac": mac.hex()}) + b"\n"
                if len(data) > _JOURNAL_BYTES or len(_custody_json(receipt)) + (receipt_file is not None) > _JOURNAL_BYTES:
                    raise MatrixError("recovery preparation exceeds its bound")
                if handle.write(data) != len(data):
                    raise OSError("incomplete preparation enrollment write")
                handle.flush()
                os.fsync(handle.fileno())
            if receipt_file is not None:
                # Exclusive creation never replaces caller authority. Persist and
                # close it BEFORE making the marker visible, including on Windows.
                encoded = _custody_json(receipt) + b"\n"
                receipt_descriptor = os.open(receipt_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                try:
                    authority = os.fdopen(receipt_descriptor, "wb")
                except BaseException:
                    os.close(receipt_descriptor)
                    raise
                with authority:
                    if authority.write(encoded) != len(encoded):
                        raise OSError("incomplete preparation receipt write")
                    authority.flush()
                    os.fsync(authority.fileno())
                # The same-filesystem rename publishes only a closed, synced marker.
                _verify_transaction_file(_written_transaction_prefix(
                    staged, data, identity, _JOURNAL_BYTES), _JOURNAL_BYTES)
                if os.path.lexists(path):
                    raise MatrixError("preparation marker appeared before enrollment publication")
                os.rename(staged, path)
            return _custody_json(receipt).decode("utf-8")
        except BaseException:
            if receipt_file is not None and identity is not None and os.path.lexists(staged):
                _remove_transaction_file(_written_transaction_prefix(
                    staged, data, identity, _JOURNAL_BYTES), _JOURNAL_BYTES)
            raise

    def append(self, event: dict[str, Any]) -> None:
        if len(self.events) >= _PREPARATION_EVENTS or self.data != self.complete:
            raise MatrixError("recovery preparation exceeds its record bound or needs restart reset")
        mac = hmac.digest(self.key, self.chain + _custody_json(event), "sha256")
        addition = _custody_json({"event": event, "mac": mac.hex()}) + b"\n"
        if len(addition) > _JOURNAL_BYTES - len(self.data):
            raise MatrixError("recovery preparation exceeds its bound")
        self._rewrite(self.data + addition)
        self.chain = mac
        self.events.append(event)

    def _rewrite(self, data: bytes) -> None:
        # The receipt owns the marker identity, including its incomplete suffix.
        # Verify through the same handle before reset; close before unlink/rename.
        flags = os.O_RDWR | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(self.path, flags), "r+b") as handle:
            actual = _transaction_metadata(handle)
            if (not stat.S_ISREG(actual.st_mode) or actual.st_nlink != 1
                    or not os.path.samestat(actual, self.identity)
                    or not _same_transaction_identity(self.path, actual)
                    or actual.st_size != len(self.data) or handle.read(len(self.data) + 1) != self.data):
                raise MatrixError("preparation custody changed; bytes retained")
            # Append preserves the previously committed prefix even on a short
            # write. Reset retains the first enrollment line, never rewriting it.
            common = len(self.complete) if len(data) >= len(self.complete) else len(data)
            handle.seek(common)
            suffix = data[common:]
            if handle.write(suffix) != len(suffix):
                raise OSError("incomplete preparation custody write")
            if (_transaction_metadata(handle).st_nlink != 1
                    or not _same_transaction_identity(self.path, actual)):
                raise MatrixError("preparation custody requires a sole link; bytes retained")
            handle.truncate(len(data))
            handle.flush()
            os.fsync(handle.fileno())
        self.data = self.complete = data

    def enroll(self, artifact: Path, identity: os.stat_result) -> None:
        if (not stat.S_ISREG(identity.st_mode) or not identity.st_ino or identity.st_nlink != 1
                or not _same_transaction_identity(artifact, identity)):
            raise MatrixError("preparation artifact identity unavailable; bytes retained")
        self.append({"enrolled": {"path": str(artifact), "device": identity.st_dev, "inode": identity.st_ino}})

    def cancel(self, artifact: Path) -> None:
        # Only a live definitive failed create can cancel an intention. Restart
        # never infers cancellation or ownership from absence or an occupant.
        self.append({"cancelled": str(artifact)})

    def artifacts(self) -> dict[Path, tuple[int, int] | None]:
        artifacts: dict[Path, tuple[int, int] | None] = {}
        destinations: set[Path] = set()
        pending = self.journal.with_suffix(".pending")
        try:
            for event in self.events[1:]:
                if not isinstance(event, dict):
                    raise ValueError("invalid event")
                if set(event) == {"create", "destination", "size", "sha256"}:
                    path, destination = Path(event["create"]), Path(event["destination"])
                    if (not destination.is_absolute() or destination.resolve() != destination
                            or (path == pending and destination != self.journal)
                            or (path != pending and (
                                destination in (self.path, pending, self.journal, self.journal.with_suffix(".lock"))
                                or path.parent != destination.parent
                                or not re.fullmatch(r"\.acceptance-[0-9a-f]{32}", path.name)))
                            or type(event["size"]) is not int or not 0 <= event["size"] <= _TRANSACTION_BYTES
                            or path == pending and event["size"] > _JOURNAL_BYTES
                            or not isinstance(event["sha256"], str)
                            or not re.fullmatch(r"[0-9a-f]{64}", event["sha256"])):
                        raise ValueError("invalid artifact intention")
                    if path != pending:
                        destinations.add(destination)
                    artifacts[path] = None
                elif set(event) == {"enrolled"}:
                    record = event["enrolled"]
                    if not isinstance(record, dict) or set(record) != {"path", "device", "inode"}:
                        raise ValueError("invalid artifact enrollment")
                    path = Path(record["path"])
                    if (path not in artifacts or artifacts[path] is not None
                            or any(type(record[k]) is not int for k in ("device", "inode"))
                            or record["device"] < 0 or record["inode"] <= 0):
                        raise ValueError("invalid artifact identity")
                    artifacts[path] = (record["device"], record["inode"])
                elif set(event) == {"cancelled"}:
                    path = Path(event["cancelled"])
                    if path not in artifacts or artifacts[path] is not None:
                        raise ValueError("invalid intention cancellation")
                    del artifacts[path]
                elif set(event) == {"bound"}:
                    record = event["bound"]
                    if (not isinstance(record, dict) or set(record) != {"path", "device", "inode", "size", "sha256"}
                            or artifacts.get(Path(record["path"])) != (record["device"], record["inode"])
                            or any(type(record[k]) is not int for k in ("device", "inode"))
                            or type(record["size"]) is not int or not 0 <= record["size"] <= _TRANSACTION_BYTES
                            or not isinstance(record["sha256"], str)
                            or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
                        raise ValueError("invalid artifact completion")
                else:
                    raise ValueError("unknown event")
                if (len(destinations) > _TRANSACTION_OUTPUTS
                        or sum(path != pending for path in artifacts) > 3 * _TRANSACTION_OUTPUTS):
                    raise ValueError("artifact count exceeds bound")
        except (ValueError, TypeError, KeyError) as exc:
            raise MatrixError(f"uncertain recovery preparation; bytes retained: {exc}") from exc
        return artifacts

    def retained(self) -> list[dict[str, Any]]:
        records = []
        artifacts = self.artifacts()
        # Verify the entire bounded set before any reset/removal. Missing files
        # may have been consumed by a journaled publication or earlier cleanup.
        for path, identity in artifacts.items():
            if not os.path.lexists(path):
                continue
            data, actual = _read_transaction_file(path, _JOURNAL_BYTES if path == self.journal.with_suffix(".pending")
                                                  else _TRANSACTION_BYTES)
            if identity != (actual.st_dev, actual.st_ino) or actual.st_nlink != 1:
                raise MatrixError("foreign or unenrolled preparation artifact; bytes retained")
            records.append({"path": str(path), "device": actual.st_dev, "inode": actual.st_ino,
                            "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        # An unlisted pending occupant is UNKNOWN, even if it parses as a journal.
        pending = self.journal.with_suffix(".pending")
        if os.path.lexists(pending) and pending not in artifacts:
            raise MatrixError("uncertain recovery preparation; pending bytes retained")
        return records

    def reset(self, preserve: frozenset[Path] = frozenset()) -> None:
        _verify_transaction_file(self.record(), _JOURNAL_BYTES)
        records = self.retained()
        for record in records:
            if Path(record["path"]) in preserve:
                continue  # The fully validated journal now supplies custody.
            _remove_transaction_file(record, _JOURNAL_BYTES if Path(record["path"]) == self.journal.with_suffix(".pending")
                                     else _TRANSACTION_BYTES)
        first = self.complete[:self.complete.index(b"\n") + 1]
        self._rewrite(first)
        self.events = [self.enrollment]
        self.chain = hmac.digest(self.key, _custody_json(self.enrollment), "sha256")
        self.pending = None

    def clear_marker(self) -> None:
        # Persistent enrollment survives handoff and successful cleanup.
        _verify_transaction_file(self.record(), _JOURNAL_BYTES)

    def handoff(self, record: dict[str, Any]) -> None:
        _verify_transaction_file(record, _JOURNAL_BYTES)
        self.pending = record

    def finish(self) -> None:
        self.clear_marker()

    def retire(self) -> None:
        if os.path.lexists(self.journal):
            raise MatrixError("recover the journal before retiring preparation custody")
        self.reset()
        _remove_transaction_file(self.record(), _JOURNAL_BYTES)


def _write_transaction(
    journal: Path, entries: list[dict[str, Any]], *, committed: bool,
    publications: list[dict[str, Any]] | None = None,
    restorations: list[dict[str, Any] | None] | None = None,
    preparation: _OutputPreparation | None = None,
) -> None:
    journal = journal.parent.resolve() / journal.name
    if not 1 <= len(entries) <= _TRANSACTION_OUTPUTS:
        raise MatrixError("recovery transaction exceeds its bound")
    transaction: dict[str, Any] = {"version": 1, "committed": committed, "outputs": entries}
    if restorations is not None:
        if len(restorations) != len(entries):
            raise MatrixError("invalid restoration count")
        # Version 1 remains readable. Version 2 binds independent restore
        # candidates so a restart never consumes the retained original copies.
        transaction.update(version=2, restorations=restorations)
    data = json.dumps(transaction, sort_keys=True).encode("utf-8")
    if len(data) > _JOURNAL_BYTES:
        raise MatrixError("recovery transaction exceeds its bound")
    previous = _transaction_record(journal, _JOURNAL_BYTES) if os.path.lexists(journal) else None
    candidate = journal.with_suffix(".pending")
    if isinstance(preparation, _PreparationCustody):
        preparation.intend(candidate, journal, data)
    # This fixed name is itself the write-ahead marker for journal creation.
    # Its empty/partial state is discoverable. Never truncate/adopt an occupant.
    flags = os.O_WRONLY | os.O_CREAT | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    metadata = None
    created = False
    try:
        try:
            descriptor = os.open(candidate, flags | os.O_EXCL, 0o600)
            created = True
        except FileExistsError as exc:
            raise MatrixError("uncertain recovery preparation; pending bytes retained") from exc
        try:
            handle = os.fdopen(descriptor, "wb")
        except BaseException:
            os.close(descriptor)
            raise
        with handle:
            metadata = _transaction_metadata(handle)
            if (not stat.S_ISREG(metadata.st_mode) or not metadata.st_ino
                    or metadata.st_nlink != 1 or not _same_transaction_identity(candidate, metadata)):
                metadata = None
                raise MatrixError("invalid recovery journal candidate")
            if preparation is not None:
                preparation.enroll(candidate, metadata)
            _verify_transaction_file(_written_transaction_prefix(candidate, b"", metadata, _JOURNAL_BYTES),
                                     _JOURNAL_BYTES)
            handle.truncate(0)
            if handle.write(data) != len(data):
                raise OSError("incomplete recovery journal write")
            handle.flush()
            os.fsync(handle.fileno())
        record = _transaction_record(candidate, _JOURNAL_BYTES)
        if (not _same_transaction_identity(candidate, metadata)
                or (record["device"], record["inode"]) != (metadata.st_dev, metadata.st_ino)
                or record["sha256"] != hashlib.sha256(data).hexdigest()):
            raise MatrixError("recovery journal candidate changed")
        if publications is not None:
            # Retain the source identity before the native operation: it may
            # succeed and then deliver a signal without returning to Python.
            publications.append(dict(record, path=str(journal)))
        _verify_transaction_file(record, _JOURNAL_BYTES)
        if preparation is not None:
            preparation.handoff(record)
        _verify_transaction_file(record, _JOURNAL_BYTES)
        if previous is not None:
            _verify_transaction_file(previous, _JOURNAL_BYTES)
        elif os.path.lexists(journal):
            raise MatrixError("recovery journal appeared; transaction retained")
        os.replace(candidate, journal)
    finally:
        if preparation is not None and preparation.pending is not None:
            if not os.path.lexists(candidate):
                # Absence is never ownership: verify the bound identity/bytes
                # at the rename destination before releasing pending accounting.
                _verify_transaction_file(dict(preparation.pending, path=str(journal)), _JOURNAL_BYTES)
                preparation.pending = None
        elif created and metadata is not None and candidate.exists() and _same_transaction_identity(candidate, metadata):
            _remove_transaction_file(_written_transaction_prefix(candidate, data, metadata, _JOURNAL_BYTES),
                                     _JOURNAL_BYTES)


class _ArtifactCreation:
    """Track one exclusive create without adopting an unknown occupant."""
    def __init__(self, mode: int) -> None:
        self.mode = mode
        self.attempted = False
        self.failed = False

    def __call__(self, name: str, flags: int) -> int:
        self.attempted = True
        path = Path(name)
        try:
            before = path.lstat()
        except FileNotFoundError:
            before = None
        try:
            return os.open(name, flags, self.mode)
        except OSError as failure:
            # An error alone cannot prove failure: a signal handler can raise
            # after native creation. Use at most two lstat calls, never open or
            # remove an occupant. Inaccessible/unknown identities fail closed.
            if failure.errno is None or failure.errno == errno.EINTR:
                raise
            try:
                after = path.lstat()
            except FileNotFoundError:
                # Under the cooperating-writer model, no artifact was left by
                # this failed call. This releases only its live owned marker;
                # absence never grants ownership of an artifact or old marker.
                self.failed = before is None
            except OSError:
                pass
            else:
                # A genuine exclusive-create collision leaves the pre-existing
                # occupant untouched. A newly created/replaced file is uncertain.
                self.failed = (failure.errno == errno.EEXIST and before is not None
                               and bool(before.st_ino) and os.path.samestat(before, after)
                               and (before.st_mode, before.st_size, before.st_mtime_ns,
                                    before.st_ctime_ns, before.st_nlink)
                               == (after.st_mode, after.st_size, after.st_mtime_ns,
                                   after.st_ctime_ns, after.st_nlink))
            raise


def _stage_recovery_copy(
    backup: dict[str, Any], preparation: _OutputPreparation, destination: Path,
) -> dict[str, Any]:
    source = _verify_transaction_file(backup)
    data, mode = _read_transaction_file(source, _TRANSACTION_BYTES)
    candidate = source.parent / (".acceptance-" + secrets.token_hex(16))
    metadata = None
    record = None
    creation = _ArtifactCreation(0o600)
    preparation.intend(candidate, destination, data)
    try:
        with open(candidate, "xb", opener=creation) as handle:
            metadata = _transaction_metadata(handle)
            _written_transaction_prefix(candidate, b"", metadata)
            preparation.enroll(candidate, metadata)
            _written_transaction_prefix(candidate, b"", metadata)
            record = {"path": str(candidate), "device": metadata.st_dev, "inode": metadata.st_ino,
                      "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            if handle.write(data) != len(data):
                raise OSError("incomplete recovery candidate write")
            handle.flush()
            os.fsync(handle.fileno())
        _verify_transaction_file(backup)
        if record is None or record["sha256"] != backup["sha256"] or not metadata.st_ino:
            raise MatrixError("recovery candidate identity or content changed")
        _verify_transaction_file(record).chmod(stat.S_IMODE(mode.st_mode))
        preparation.bind(record)
        return record
    except BaseException:
        if creation.failed:
            preparation.cancel(candidate)
        if metadata is not None:
            try:
                _verify_transaction_file(backup)
                _remove_transaction_file(_written_transaction_prefix(candidate, data, metadata))
            except BaseException:
                preparation.uncertain = True
                raise
        elif creation.attempted and not creation.failed:
            preparation.uncertain = True
        raise


def _recover_outputs(journal: Path | None, custody: str | None = None, *, enroll_recovery: bool = False,
                     receipt_file: Path | None = None) -> str | None:
    """Recover outputs, transferring any new automatic custody to the caller.

    The caller must retain that custody through publication and retire it when
    the invocation finishes. Failed recovery retains the delivered receipt's
    existing restart authority.
    """
    journal = journal.parent.resolve() / journal.name if journal is not None else None
    owner = _PreparationCustody(journal, custody) if journal is not None and custody is not None else None
    if owner is not None:
        # Validate before any journal recovery mutation. Pending bytes and torn
        # suffixes are reset only after the entire journal also validates.
        owner.retained()
    elif _preparation_pending(journal):
        raise MatrixError(f"uncertain recovery preparation; outputs and recovery copies retained; inspect "
                          f"{journal.with_suffix('.preparation')} and {journal.with_suffix('.pending')}")
    automatic_custody = _recover_journal(journal, owner, enroll_recovery=enroll_recovery, receipt_file=receipt_file)
    if owner is not None:
        owner.reset()
    return automatic_custody


def _recover_journal(journal: Path | None, owner: _PreparationCustody | None = None,
                     *, enroll_recovery: bool = False, receipt_file: Path | None = None) -> str | None:
    if journal is None or not journal.exists():
        return
    if _same_path(journal, journal.with_suffix(".lock")):
        raise MatrixError("recovery journal aliases refresh lock")
    raw, journal_identity = _read_transaction_file(journal, _JOURNAL_BYTES)
    journal_record = {"path": str(journal), "device": journal_identity.st_dev, "inode": journal_identity.st_ino,
                      "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    try:
        transaction = json.loads(raw)
        if (not isinstance(transaction, dict) or type(transaction.get("version")) is not int
                or transaction["version"] not in (1, 2)
                or set(transaction) != ({"version", "committed", "outputs"}
                                       | ({"restorations"} if transaction["version"] == 2 else set()))
                or type(transaction["committed"]) is not bool):
            raise ValueError("unknown transaction metadata")
        entries = transaction["outputs"]
        if not isinstance(entries, list) or not 1 <= len(entries) <= _TRANSACTION_OUTPUTS:
            raise ValueError("invalid output count")
        restorations = transaction.get("restorations", [None] * len(entries))
        if not isinstance(restorations, list) or len(restorations) != len(entries):
            raise ValueError("invalid restoration count")
        names: set[Path] = {journal, journal.with_suffix(".pending")}
        for index, entry in enumerate(entries):
            if not isinstance(entry, dict) or set(entry) != {"path", "old", "backup", "new"}:
                raise ValueError("invalid output metadata")
            path = Path(entry["path"])
            if not path.is_absolute() or path in names or path.resolve() != path:
                raise ValueError("invalid output path")
            names.add(path)
            old = entry["old"]
            if old is not None and (not isinstance(old, str) or not re.fullmatch(r"[0-9a-f]{64}", old)):
                raise ValueError("invalid original digest")
            if (old is None) != (entry["backup"] is None):
                raise ValueError("invalid backup metadata")
            if not isinstance(entry["new"], dict):
                raise ValueError("missing publication metadata")
            for record in (entry["new"], entry["backup"], restorations[index]):
                if record is None:
                    continue
                if not isinstance(record, dict) or set(record) != {"path", "device", "inode", "size", "sha256"}:
                    raise ValueError("invalid recovery metadata")
                artifact = Path(record["path"])
                if (artifact.parent != path.parent or artifact in names
                        or not re.fullmatch(r"\.acceptance-[0-9a-f]{32}", artifact.name)
                        or any(type(record[key]) is not int for key in ("device", "inode", "size"))
                        or record["device"] < 0 or record["inode"] <= 0
                        or not 0 <= record["size"] <= _TRANSACTION_BYTES
                        or not isinstance(record["sha256"], str)
                        or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
                    raise ValueError("invalid recovery identity")
                names.add(artifact)
            if entry["backup"] is not None and entry["backup"]["sha256"] != old:
                raise ValueError("backup digest differs from original")
            if restorations[index] is not None and restorations[index]["sha256"] != old:
                raise ValueError("restoration digest differs from original")
    except (ValueError, TypeError, KeyError) as exc:
        raise MatrixError(f"invalid recovery journal: {exc}") from exc

    restore: list[int] = []
    # Validate the entire bounded transaction before restoring any destination.
    _verify_transaction_file(journal_record, _JOURNAL_BYTES)
    if owner is not None:
        _verify_transaction_file(owner.record(), _JOURNAL_BYTES)
        owner.retained()
    for index, entry in enumerate(entries):
        for record in (entry["new"], entry["backup"], restorations[index]):
            if record is not None and Path(record["path"]).exists():
                _verify_transaction_file(record)
        if transaction["committed"]:
            continue
        path = Path(entry["path"])
        try:
            current = _transaction_record(path)
        except FileNotFoundError:
            current = None
        if (current["sha256"] if current is not None else None) == entry["old"]:
            continue  # Unpublished, or already restored by an interrupted recovery.
        expected = dict(entry["new"], path=str(path))
        if current != expected:
            raise MatrixError(f"recovery output changed; recovery copies retained: {path}")
        if entry["backup"] is not None:
            _verify_transaction_file(entry["backup"])
        restore.append(index)
    automatic_owner = enroll_recovery and owner is None and bool(restore)
    if automatic_owner:
        # Legacy v1/v2 journals can need new restoration payloads too. Enroll
        # only after their complete validation, and deliver restart authority
        # before staging, exactly as for a new CLI publication.
        receipt = _create_cli_custody(journal, receipt_file, journal_record)
        print("preparation custody receipt: " + receipt, file=sys.stderr, flush=True)
        owner = _PreparationCustody(journal, receipt)
    if owner is not None:
        # Compact only after every journal record and destination was validated.
        # The journal retains discovery/identity authority for its files; extra
        # enrolled staging files can be reset. Repeated interrupted recovery
        # therefore never accumulates records or consumes the event budget.
        owner.reset(frozenset(Path(record["path"]) for index, entry in enumerate(entries)
                              for record in (entry["new"], entry["backup"], restorations[index])
                              if record is not None))
    for index in reversed(restore):
        entry = entries[index]
        path = Path(entry["path"])
        _verify_transaction_file(dict(entry["new"], path=str(path)))
        if entry["backup"] is None:
            # Verify before named removal under the cooperating-writer model.
            path.unlink()
        else:
            record = restorations[index]
            if record is None or not Path(record["path"]).exists():
                preparation = owner if owner is not None else _OutputPreparation(journal.with_suffix(".preparation"))
                publications: list[dict[str, Any]] = []
                record = None
                try:
                    record = _stage_recovery_copy(entry["backup"], preparation, path)
                    restorations[index] = record
                    _verify_transaction_file(journal_record, _JOURNAL_BYTES)
                    _write_transaction(journal, entries, committed=False,
                                       restorations=restorations, publications=publications,
                                       preparation=preparation)
                except BaseException:
                    # A completed journal rename owns the candidate even when
                    # its call was interrupted. Otherwise only this call's
                    # verified, unpublished copy may be removed.
                    if record is not None and (not publications or
                            _transaction_record(journal, _JOURNAL_BYTES) != publications[-1]):
                        _remove_transaction_file(record)
                    preparation.finish()
                    raise
                journal_record = publications[-1]
                preparation.finish()
            candidate = _verify_transaction_file(record)
            _verify_transaction_file(dict(entry["new"], path=str(path)))
            os.replace(candidate, path)  # Closed handles on Windows; restartable.
    if not transaction["committed"]:
        for entry in entries:
            try:
                current = _transaction_record(Path(entry["path"]))["sha256"]
            except FileNotFoundError:
                current = None
            if current != entry["old"]:
                raise MatrixError("recovery output changed; recovery copies retained")
    _verify_transaction_file(journal_record, _JOURNAL_BYTES)
    if owner is not None:
        _verify_transaction_file(owner.record(), _JOURNAL_BYTES)
        owner.retained()
    for index, entry in enumerate(entries):
        for record in (entry["new"], entry["backup"], restorations[index]):
            if record is not None and os.path.lexists(record["path"]):
                _verify_transaction_file(record)
    for index, entry in enumerate(entries):
        for record in (entry["new"], entry["backup"], restorations[index]):
            if record is not None and Path(record["path"]).exists():
                _remove_transaction_file(record)
    _verify_transaction_file(journal_record, _JOURNAL_BYTES).unlink()
    if automatic_owner:
        owner.reset()
        return receipt


def _replace_outputs(
    outputs: list[tuple[Path, str]], *, private_output: Path | None = None,
    verify: Callable[[dict[Path, bytes]], None] | None = None,
    journal: Path | None = None,
    custody: str | None = None,
) -> None:
    """Stage closed files and rollback copies before publishing on either OS."""
    journal = journal.parent.resolve() / journal.name if journal is not None else None
    staged: list[tuple[Path, Path]] = []
    backups: dict[Path, Path | None] = {}
    temporary_paths: list[Path] = []
    temporary_identities: dict[Path, os.stat_result] = {}
    temporary_records: dict[Path, dict[str, Any]] = {}
    digests: dict[Path, bytes] = {}
    backup_digests: dict[Path, bytes] = {}
    identities: dict[Path, tuple[Path, os.stat_result]] = {}
    originals: dict[Path, dict[str, Any] | None] = {}
    candidates: dict[Path, dict[str, Any]] = {}
    restorations: dict[Path, dict[str, Any] | None] = {}
    published: dict[Path, bytes] = {}
    keep_backups = False
    transaction: list[dict[str, Any]] = []
    journal_publications: list[dict[str, Any]] = []
    preparation = _OutputPreparation(journal.with_suffix(".preparation") if journal is not None else
                                     (outputs[0][0].parent / ".refresh-preparation" if outputs else
                                      Path(".refresh-preparation")))
    if custody is not None:
        if journal is None:
            raise MatrixError("preparation custody requires a recovery journal")
        preparation = _PreparationCustody(journal, custody)
        if len(preparation.events) != 1 or preparation.data != preparation.complete:
            raise MatrixError("recover enrolled preparation before replacing outputs")

    def stage(path: Path, data: bytes, mode: int | None, private: bool) -> Path:
        # A 128-bit random name plus exclusive creation prevents clobbering
        # another file. Register it for cleanup only after creation succeeds.
        temporary = path.parent / (".acceptance-" + secrets.token_hex(16))
        creation_mode = 0o600 if mode is not None or private else 0o666
        metadata = None
        creation = _ArtifactCreation(creation_mode)
        preparation.intend(temporary, path, data)
        try:
            with open(temporary, "xb", opener=creation) as handle:
                temporary_paths.append(temporary)
                metadata = _transaction_metadata(handle)
                temporary_identities[temporary] = metadata
                _written_transaction_prefix(temporary, b"", metadata)
                preparation.enroll(temporary, metadata)
                _written_transaction_prefix(temporary, b"", metadata)
                temporary_records[temporary] = {
                    "path": str(temporary), "device": metadata.st_dev, "inode": metadata.st_ino,
                    "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                }
                if handle.write(data) != len(data):
                    raise OSError("incomplete staged output write")
                handle.flush()
                os.fsync(handle.fileno())
        except BaseException:
            if creation.failed:
                preparation.cancel(temporary)
            if metadata is not None:
                temporary_records[temporary] = _written_transaction_prefix(temporary, data, metadata)
            elif creation.attempted and not creation.failed:
                preparation.uncertain = True
            raise
        if mode is not None:
            _verify_transaction_file(temporary_records[temporary]).chmod(mode)
        preparation.bind(temporary_records[temporary])
        return temporary

    def verify_temporary(path: Path) -> None:
        expected = temporary_identities.get(path)
        actual = path.lstat()
        if actual.st_nlink != 1:
            raise MatrixError(f"temporary output requires a sole link; transaction retained: {path}")
        if (expected is None or not expected.st_ino or not stat.S_ISREG(actual.st_mode)
                or not _same_transaction_identity(path, expected)):
            raise MatrixError(f"temporary output identity changed: {path}")

    def bound_temporary(path: Path, digest: bytes) -> dict[str, Any]:
        verify_temporary(path)
        record = _transaction_record(path)
        expected = temporary_identities[path]
        if ((record["device"], record["inode"]) != (expected.st_dev, expected.st_ino)
                or record["sha256"] != digest.hex()):
            raise MatrixError(f"temporary output identity or content changed: {path}")
        return record

    def verify_publication(path: Path) -> None:
        target, identity = identities[path]
        before = path.lstat()
        if (path.resolve() != target or not stat.S_ISREG(before.st_mode)
                or not identity.st_ino or not os.path.samestat(before, identity)):
            raise MatrixError("output changed since publication or its identity cannot be verified")
        fingerprint = hashlib.sha256(_read_transaction_file(path, _TRANSACTION_BYTES)[0]).digest()
        after = path.lstat()
        if (path.resolve() != target or not os.path.samestat(before, after) or fingerprint != digests[path]
                or (before.st_mode, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                != (after.st_mode, after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
            raise MatrixError("output changed since publication or its identity cannot be verified")
        _verify_transaction_file(dict(candidates[path], path=str(path)))

    def restored(path: Path) -> bool:
        try:
            current = _transaction_record(path)
        except FileNotFoundError:
            current = None
        if path in restorations:
            return current == restorations[path]
        original = originals[path]
        if current == original:
            # Original bytes and even its inode can recur after publication.
            # Only our still-unpublished candidate proves no restore is needed.
            _verify_transaction_file(candidates[path])
            return True
        return False

    def verify_retained_links() -> None:
        # At most fifteen payloads plus the fixed metadata names. Validate the
        # whole retained set before rollback or cleanup can mutate any member.
        for temporary in temporary_paths:
            if os.path.lexists(temporary):
                verify_temporary(temporary)
        for _, destination in staged:
            if os.path.lexists(destination):
                metadata = destination.lstat()
                if stat.S_ISREG(metadata.st_mode) and metadata.st_nlink != 1:
                    raise MatrixError(f"output requires a sole link; transaction retained: {destination}")
        if preparation.identity is not None:
            _written_transaction_prefix(preparation.path, preparation.data, preparation.identity, _JOURNAL_BYTES)
        if journal is not None:
            for metadata_path in (journal, journal.with_suffix(".pending")):
                if os.path.lexists(metadata_path) and metadata_path.lstat().st_nlink != 1:
                    raise MatrixError(f"recovery metadata requires a sole link; transaction retained: {metadata_path}")

    try:
        if len(outputs) > _TRANSACTION_OUTPUTS:
            raise MatrixError("too many transaction outputs")
        markers = {path.parent / ".refresh-preparation" for path, _ in outputs} if journal is None else {
            preparation.path, journal.with_suffix(".pending")}
        if any(os.path.lexists(marker) for marker in markers
               if not isinstance(preparation, _PreparationCustody) or marker != preparation.path):
            raise MatrixError("uncertain recovery preparation; outputs and recovery copies retained; inspect "
                              + ", ".join(str(marker) for marker in sorted(markers)))
        reserved = list(markers)
        if journal is not None:
            reserved.extend((journal, journal.with_suffix(".lock")))
        if any(_same_path(path, metadata) for path, _ in outputs for metadata in reserved):
            raise MatrixError("output path overlaps recovery metadata")
        for path, text in outputs:
            # --write-doc may explicitly name the standard generated document.
            if path in backups:
                continue
            if len(text) > _TRANSACTION_BYTES or (path.exists() and path.stat().st_size > _TRANSACTION_BYTES):
                raise MatrixError("transaction output exceeds size bound")
            # Count UTF-8 bytes before allocating the encoded candidate. The
            # character pass is bounded by the file limit and runs only on writes.
            encoded_size = sum(1 if point < 0x80 else 2 if point < 0x800 else 3 if point < 0x10000 else 4
                               for point in map(ord, text))
            if encoded_size > _TRANSACTION_BYTES:
                raise MatrixError("transaction output exceeds size bound")
            data = text.encode("utf-8")
            try:
                original, original_metadata = _read_transaction_file(path, _TRANSACTION_BYTES)
                mode = stat.S_IMODE(original_metadata.st_mode)
            except FileNotFoundError:
                original, mode = None, None
            except MatrixError:
                if verify is not None:
                    verify(published)
                raise
            originals[path] = (dict(path=str(path), device=original_metadata.st_dev,
                                    inode=original_metadata.st_ino, size=len(original),
                                    sha256=hashlib.sha256(original).hexdigest())
                               if original is not None else None)
            if original == data:
                continue
            backups[path] = stage(path, original, mode, True) if original is not None else None
            if original is not None:
                backup_digests[path] = hashlib.sha256(original).digest()
            staged.append((stage(path, data, mode, path == private_output), path))
            digests[path] = hashlib.sha256(data).digest()
        if verify is not None:
            verify(published)
        verify_retained_links()
        for temporary, path in staged:
            candidates[path] = bound_temporary(temporary, digests[path])
        transaction = [{"path": str(path),
                        "old": backup_digests[path].hex() if path in backup_digests else None,
                        "backup": bound_temporary(backups[path], backup_digests[path])
                                  if backups[path] is not None else None,
                        "new": candidates[path]} for temporary, path in staged]
        if journal is not None and transaction:
            _write_transaction(journal, transaction, committed=False, publications=journal_publications,
                               preparation=preparation)
            # The published journal now binds every staged file. Until this
            # point a hard exit leaves the fixed preparation marker in place.
            preparation.finish()
        for temporary, path in staged:
            target, identity = path.resolve(), temporary.stat()
            identities[path] = (target, identity)
            if verify is not None:
                verify(published)
            verify_retained_links()
            try:
                current = _transaction_record(path)
            except FileNotFoundError:
                current = None
            if current != originals[path]:
                raise MatrixError(f"output changed before publication: {path}")
            verify_temporary(temporary)
            _verify_transaction_file(candidates[path])
            temporary.replace(path)
            _verify_transaction_file(dict(candidates[path], path=str(path)))
            published[target] = digests[path]
            if verify is not None:
                verify(published)
        if verify is not None:
            verify(published)
        verify_retained_links()
        if journal is not None and transaction:
            _write_transaction(journal, transaction, committed=True, publications=journal_publications,
                               preparation=preparation if isinstance(preparation, _PreparationCustody) else None)
    except BaseException as failure:
        keep_backups = True
        conflicts = []
        verify_retained_links()
        # A commit rename can also finish before reporting interruption. Put a
        # verified journal back in rollback state before changing destinations;
        # if that fails, leave the complete publication and its copies intact.
        if len(journal_publications) == 2 and journal is not None and journal.exists():
            try:
                if _transaction_record(journal, _JOURNAL_BYTES) == journal_publications[-1]:
                    _write_transaction(journal, transaction, committed=False, publications=journal_publications,
                                       preparation=preparation if isinstance(preparation, _PreparationCustody) else None)
            except BaseException as conflict:
                raise MatrixError(f"{failure}\nrollback journal unavailable; all recovery copies retained: {conflict}") from failure
        for temporary, path in reversed(staged):
            try:
                # No operation has been attempted until its source is bound.
                # Check every staged destination, including a native success
                # whose Python bookkeeping was interrupted.
                if path not in candidates or restored(path):
                    continue
                verify_publication(path)
                backup = backups[path]
                if backup is None:
                    restorations[path] = None
                    verify_publication(path)
                    path.unlink()
                else:
                    bound_temporary(backup, backup_digests[path])
                    original, backup_metadata = _read_transaction_file(backup, _TRANSACTION_BYTES)
                    if hashlib.sha256(original).digest() != backup_digests[path]:
                        raise MatrixError(f"recovery copy changed: {backup}")
                    # Restore from a separate closed candidate. Every backup
                    # must survive even if a later output cannot be restored.
                    candidate = stage(path, original, stat.S_IMODE(backup_metadata.st_mode), True)
                    restoration = bound_temporary(candidate, backup_digests[path])
                    restorations[path] = dict(restoration, path=str(path))
                    # Portable rename/unlink has no compare-and-swap operation.
                    # Recheck bytes and identity immediately before replacement;
                    # all handles are closed so Windows can replace the file.
                    verify_publication(path)
                    _verify_transaction_file(restoration)
                    os.replace(candidate, path)
            except BaseException as conflict:
                try:
                    if path in restorations and restored(path):
                        continue
                except BaseException:
                    pass
                conflicts.append(f"rollback conflict for {path}: {conflict}")
        # Keep every backup until the entire set, including earlier restores,
        # has been checked again. Never infer restoration from a pathname.
        for temporary, path in staged:
            if path in candidates:
                try:
                    if not restored(path):
                        raise MatrixError("restored output changed")
                except BaseException as conflict:
                    conflicts.append(f"rollback conflict for {path}: {conflict}")
        if conflicts:
            recovery = [f"recovery copy for {path}: {backup}"
                        for path, backup in backups.items() if backup is not None]
            raise MatrixError(f"{failure}\nrollback incomplete; all recovery copies retained\n"
                              + "\n".join([*conflicts, *recovery])) from failure
        keep_backups = False
        raise
    finally:
        verify_retained_links()
        journal_record = None
        if journal_publications and not keep_backups and journal is not None and journal.exists():
            journal_record = _transaction_record(journal, _JOURNAL_BYTES)
            if journal_record not in journal_publications:
                # Check before discarding recovery copies, then check this same
                # retained identity again before the named unlink.
                keep_backups = True
                raise MatrixError("recovery journal identity or content changed")
        recovery_paths = {backup for backup in backups.values() if backup is not None} if keep_backups else set()
        cleanup_complete = True
        if journal_record is not None and not preparation.uncertain:
            recorded = {Path(record["path"]) for entry in transaction
                        for record in (entry["new"], entry["backup"]) if record is not None}
            # A completed rollback has verified every restored destination.
            # Once each extra candidate is bound there, the journal alone can
            # discover all remaining files even if cleanup is interrupted.
            extra = set(temporary_paths) - recorded
            if all(not os.path.lexists(path) and any(
                    restoration is not None and temporary_records.get(path) is not None
                    and restoration == dict(temporary_records[path], path=str(target))
                    and _transaction_record(target) == restoration
                    for target, restoration in restorations.items()) for path in extra):
                preparation.finish()
        for temporary in temporary_paths:
            if temporary in recovery_paths:
                continue
            try:
                try:
                    verify_temporary(temporary)
                except FileNotFoundError:
                    continue
                record = temporary_records.get(temporary)
                if record is None:
                    raise MatrixError(f"temporary output content cannot be verified: {temporary}")
                _remove_transaction_file(record)
            except (MatrixError, OSError) as cleanup_error:
                cleanup_complete = False
                if not keep_backups:
                    raise
                # Cleanup must not hide the conflict and recovery-copy paths.
                print(f"error: cannot remove temporary output {temporary}: {cleanup_error}", file=sys.stderr)
        if cleanup_complete and (not keep_backups or (journal_publications and journal is not None
                and _transaction_record(journal, _JOURNAL_BYTES) in journal_publications)):
            # Direct calls without a journal retain the marker on incomplete
            # rollback. No recovery file may become undiscoverable at cleanup.
            preparation.finish()
        if journal_record is not None:
            _verify_transaction_file(journal_record, _JOURNAL_BYTES).unlink()
        if isinstance(preparation, _PreparationCustody) and cleanup_complete and not keep_backups:
            preparation.reset()


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
    path_problem: Callable[..., str | None],
) -> None:
    if not isinstance(item, dict):
        add("evidence item must be an object")
        return
    kind = item.get("kind")
    if not isinstance(kind, str) or kind not in EVIDENCE_KINDS:
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
        problem = path_problem(base, ref, must_exist=not is_planned)
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
                problem = path_problem(base, entry_point, must_exist=not is_planned)
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
    kinds = {item["kind"] for item in items if isinstance(item.get("kind"), str)}

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
    *, inventory: dict[str, Any] | None = None, inventory_path: Path | None = None,
) -> list[str]:
    """Validate disk inputs, or a prospective inventory before publishing it."""
    base = root.resolve()
    # Evidence and native entry points often repeat across rows. Bound this
    # invocation's path-result cache to 256 entries; never retain disk answers
    # across validation calls, even when another process edits the same path.
    @lru_cache(maxsize=256)
    def path_problem(directory: Path, ref: str, must_exist: bool) -> str | None:
        return _path_problem(directory, ref, must_exist, resolved_base=base)
    problems: list[tuple[str, str]] = []
    try:
        if inventory is None:
            inventory = load_matrix(inventory_path if inventory_path is not None
                                    else base / "tools/ci/acceptance_requirements.json")
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
    documents: dict[str, tuple[str | None, frozenset[str] | None]] = {}
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
            problem = _string_problem(row.get(key), key)
            if problem:
                messages.append(problem)
        if "discrepancy" in row and not _is_nonempty_str(row["discrepancy"]):
            messages.append("discrepancy must be a non-empty string")
        if _is_nonempty_str(row.get("source")):
            source = row["source"]
            if source not in source_problems:
                source_problems[source] = _source_problem(base, source, documents)
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
        if not isinstance(status, str) or status not in VALID_STATUSES:
            messages.append(f"unknown status: {status}")

        evidence = row.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            messages.append("evidence must be a non-empty list")
        else:
            for item in evidence:
                item_messages: list[str] = []
                _check_item(item_messages.append, item, base, index, require_landed, path_problem)
                messages.extend(item_messages)
            dict_items = [item for item in evidence if isinstance(item, dict)]
            if isinstance(status, str) and status in VALID_STATUSES and len(dict_items) == len(evidence):
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
        "in the Acceptance requirement IDs section of [the contract](distant-world.md) "
        "and checks references; it does not "
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


def _matrix_input_paths(matrix: dict[str, Any], root: Path) -> tuple[list[Path], list[Path]]:
    """Return snapshot inputs and the subset protected against output overlap.

    Only literal doc evidence for the generated view may overlap its output.
    Retain that reference in snapshots, including its unresolved alias identity.
    Source uses, other evidence kinds and alias spellings remain protected.
    """
    references: dict[tuple[str, bool], None] = {}
    rows = matrix.get("rows", [])
    for row in rows if isinstance(rows, list) else []:
        if not isinstance(row, dict):
            continue
        source = row.get("source")
        if _is_nonempty_str(source):
            references[source.partition("#")[0], False] = None
        evidence = row.get("evidence", [])
        for item in evidence if isinstance(evidence, list) else []:
            if isinstance(item, dict) and item.get("kind") != "ctest" and _is_nonempty_str(item.get("ref")):
                generated = item.get("kind") == "doc" and item["ref"] == "docs/acceptance-matrix.md"
                references[item["ref"], generated] = None
    inputs, protected = [], []
    base = root.resolve()
    # Repeated row references have the same role, but alternate spellings and
    # protected/generated roles remain distinct. Work is linear in unique references.
    for ref, generated in references:
        path = root / ref
        if not _path_problem(root, ref, must_exist=False, resolved_base=base) and not path.is_dir():
            inputs.append(path)
            if not generated:
                protected.append(path)
    return inputs, protected


class _PathComparisons:
    """Reuse metadata only within a single prepublication overlap check.

    Storage is bounded by that check's distinct input and output paths. Never
    reuse it for snapshots, publication, rollback or a later CLI invocation.
    Path.stat (rather than DirEntry.stat) supplies Windows file identities.
    """

    def __init__(self) -> None:
        self.resolved: dict[Path, Path] = {}
        self.metadata: dict[Path, os.stat_result | None] = {}

    def _resolve(self, path: Path) -> Path:
        if path not in self.resolved:
            self.resolved[path] = path.resolve()
        return self.resolved[path]

    def _stat(self, path: Path) -> os.stat_result | None:
        if path not in self.metadata:
            try:
                self.metadata[path] = path.stat()
            except FileNotFoundError:
                self.metadata[path] = None
        return self.metadata[path]

    def same(self, left: Path, right: Path) -> bool:
        if self._resolve(left) == self._resolve(right):
            return True
        first = self._stat(left)
        if first is None:
            return False
        second = self._stat(right)
        return second is not None and os.path.samestat(first, second)


def _same_path(left: Path, right: Path) -> bool:
    """Compare names and file identities, including hard links and short names."""
    return _PathComparisons().same(left, right)


def _check_output_paths(args: argparse.Namespace, root: Path, matrix_path: Path, inputs: list[Path]) -> None:
    inventory, doc = args.inventory, args.generated_doc
    targets = [inventory, matrix_path, doc] if args.refresh_inventory else []
    if args.write_doc is not None and (not args.refresh_inventory or args.write_doc != doc):
        targets.append(args.write_doc)
    if args.execution_receipt is not None:
        targets.append(args.execution_receipt)
    comparisons = _PathComparisons()
    if any(comparisons.same(path, other) for index, path in enumerate(targets) for other in targets[:index]):
        raise MatrixError("refresh output paths must be distinct" if args.refresh_inventory
                          else "output paths must be distinct")
    if not targets:
        return
    cmake_files, gtest_files = _discovery_files(root)
    protected = [*(root / path for path in CONTRACT_DOCUMENTS),
                 *cmake_files, *gtest_files, *inputs, *args.junit,
                 *([args.discovery] if args.discovery is not None else [])]
    lock = _refresh_lock_path(root)
    protected.extend([lock, lock.with_suffix(".transaction"), lock.with_suffix(".pending"),
                      lock.with_suffix(".preparation")])
    if getattr(args, "preparation_receipt_file", None) is not None:
        protected.append(args.preparation_receipt_file)
    for path in targets:
        # Refresh may replace its own matrix/inventory, but neither may alias
        # another output or any contract/evidence input. Document aliases get
        # exactly the same checks as explicitly supplied destinations.
        extra = [] if args.refresh_inventory else [matrix_path, inventory]
        if path != doc or path == args.execution_receipt:
            extra.append(doc)
        if any(comparisons.same(path, source) for source in dict.fromkeys([*protected, *extra])):
            raise MatrixError(f"output path overlaps an input: {path}")


@lru_cache(maxsize=1)
def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Check the acceptance matrix.")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--matrix", type=Path, default=None)
    parser.add_argument("--check-doc", type=Path, default=None)
    parser.add_argument("--write-doc", type=Path, default=None)
    parser.add_argument("--refresh-inventory", action="store_true",
                        help="refresh unique exact-text relocations and print human decision fragments")
    parser.add_argument("--require-landed", action="store_true")
    parser.add_argument("--junit", type=Path, action="append", default=[])
    parser.add_argument("--discovery", type=Path)
    parser.add_argument("--execution-receipt", type=Path)
    parser.add_argument("--ci-context", action="store_true")
    parser.add_argument("--enroll-preparation", action="store_true",
                        help="enroll an empty preparation marker and print its independent custody receipt")
    parser.add_argument("--preparation-custody",
                        help="JSON receipt returned by --enroll-preparation; retain outside recovery files")
    parser.add_argument("--preparation-receipt-file", type=Path,
                        help="new caller-owned file outside the checkout and Git metadata; required for new enrollment")
    parser.add_argument("--retire-preparation", action="store_true",
                        help="recover and retire the marker bound by --preparation-custody")
    return parser


def _create_cli_custody(journal: Path, receipt_file: Path | None,
                        journal_record: dict[str, Any] | None = None) -> str:
    if receipt_file is None:
        raise MatrixError("new enrollment requires --preparation-receipt-file outside the checkout and Git metadata")
    return _PreparationCustody.create(journal, journal_record, receipt_file=receipt_file)


def _retire_automatic_custody(journal: Path | None, custody: str | None) -> None:
    if custody is not None and not os.path.lexists(journal):
        owner = _PreparationCustody(journal, custody)
        if len(owner.events) == 1 and owner.data == owner.complete:
            owner.retire()


def _run_after_recovery(args: argparse.Namespace, root: Path, journal: Path) -> int:
    automatic_custody = _recover_outputs(journal, args.preparation_custody, enroll_recovery=True,
                                         receipt_file=args.preparation_receipt_file)
    try:
        if args.retire_preparation:
            _PreparationCustody(journal, args.preparation_custody).retire()
            return 0
        if automatic_custody is not None:
            args = copy.copy(args)
            args.preparation_custody = automatic_custody
        return _run_locked(args, root)
    finally:
        # Recovery and publication share one enrollment. This also retires a
        # clean marker when input validation fails before publication starts.
        _retire_automatic_custody(journal, automatic_custody)


def main(argv: list[str] | None = None) -> int:
    try:
        args = _argument_parser().parse_args(argv)
    except SystemExit as exc:
        return int(exc.code or 0)

    root = args.root.resolve()
    if ((args.enroll_preparation and (args.preparation_custody is not None or args.retire_preparation))
            or args.retire_preparation and args.preparation_custody is None
            or (args.enroll_preparation or args.retire_preparation) and (
                args.refresh_inventory or args.write_doc is not None or args.execution_receipt is not None
                or args.check_doc is not None or args.junit or args.discovery is not None or args.ci_context)):
        print("error: enrollment/retirement requires a separate invocation; retirement requires custody", file=sys.stderr)
        return 2
    if bool(args.junit) != bool(args.discovery) or ((args.execution_receipt or args.ci_context) and not args.junit):
        print("error: --junit and --discovery are required together for execution evidence", file=sys.stderr)
        return 2
    try:
        if args.preparation_receipt_file is not None:
            args.preparation_receipt_file = args.preparation_receipt_file.resolve()
            lock = _refresh_lock_path(root, required=False)
            metadata = lock.parent if lock is not None else None
            if (args.preparation_receipt_file.is_relative_to(root)
                    or metadata is not None and args.preparation_receipt_file.is_relative_to(metadata)):
                raise MatrixError("preparation receipt file must be outside the checkout and Git metadata")
        # At most one speculative read on a read-only checkout. A writer always
        # creates the persistent lock before publishing any journal or output;
        # its appearance invalidates even an unsuccessful speculative result.
        writing = (args.refresh_inventory or args.write_doc is not None or args.execution_receipt is not None
                   or args.enroll_preparation or args.retire_preparation or args.preparation_custody is not None)
        for attempt in range(2):
            with _refresh_lock(root, shared=not writing) as synchronized:
                lock = _refresh_lock_path(root, required=writing)
                journal = lock.with_suffix(".transaction") if lock is not None else None
                if writing:
                    if args.enroll_preparation:
                        print(_create_cli_custody(journal, args.preparation_receipt_file), flush=True)
                        return 0
                    return _run_after_recovery(args, root, journal)
                if journal is not None and (journal.exists() or _preparation_pending(journal)):
                    break
                if synchronized:
                    return _run_locked(args, root)
                if attempt:
                    raise MatrixError("inventory refresh lock changed during validation; retry")
                out, err = _ReadDiagnostics(), _ReadDiagnostics()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    try:
                        code = _run_locked(args, root)
                    except (MatrixError, OSError, UnicodeError, RuntimeError) as exc:
                        print(f"error: {exc}", file=sys.stderr)
                        code = 2
                lock = _refresh_lock_path(root, required=False)
                if lock is None or not lock.exists():
                    if out.overflow or err.overflow:
                        raise MatrixError("unlocked validation diagnostics exceed capture bound; retry with a refresh lock")
                    sys.stdout.write(out.getvalue())
                    sys.stderr.write(err.getvalue())
                    return code
        # Recovery needs exclusive ownership. Recheck under that lock before
        # reading inputs; another writer may have completed recovery meanwhile.
        with _refresh_lock(root):
            return _run_after_recovery(args, root, journal)
    except (MatrixError, OSError, UnicodeError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


def _run_locked(args: argparse.Namespace, root: Path) -> int:
    # A speculative reader may retry. Never discard its original names by
    # mutating the parsed arguments, including an aliased checkout root.
    args = copy.copy(args)
    args.matrix = args.matrix if args.matrix is not None else args.root / "tools/ci/acceptance_matrix.json"
    args.inventory = args.root / "tools/ci/acceptance_requirements.json"
    args.generated_doc = args.root / "docs/acceptance-matrix.md"
    # Use one identity for aliases, including Windows short names and paths
    # containing parent components, throughout snapshots and publication.
    # Defaults follow the same rule: replace the resolved destination and
    # preserve the symlink used to reach it.
    aliases: dict[Path, Path] = {}
    for name in ("matrix", "inventory", "generated_doc", "check_doc", "write_doc", "discovery",
                 "execution_receipt"):
        path = getattr(args, name)
        if path is not None:
            if path not in aliases:
                aliases[path] = path.resolve()
            setattr(args, name, aliases[path])
    for path in args.junit:
        if path not in aliases:
            aliases[path] = path.resolve()
    args.junit = [aliases[path] for path in args.junit]
    lock_path = _refresh_lock_path(root, required=False)
    lock = lock_path.resolve() if lock_path is not None else None
    comparisons = _PathComparisons()
    if lock is not None and any(path is not None and comparisons.same(path, lock)
                                for path in (args.write_doc, args.execution_receipt)):
        raise MatrixError("output path overlaps an input: refresh lock")
    if lock is not None and any(path is not None and comparisons.same(path, lock)
                                for path in (args.matrix, args.inventory, args.generated_doc,
                                             args.discovery, args.check_doc, *args.junit)):
        raise MatrixError("refresh lock cannot be used as an input")
    snapshot = None
    if args.refresh_inventory or args.write_doc is not None or args.execution_receipt is not None:
        paths = [args.inventory, args.matrix, args.generated_doc, *args.junit]
        paths.extend(path for path in (args.discovery, args.check_doc, args.write_doc,
                                       args.execution_receipt) if path is not None)
        snapshot = _RefreshSnapshot(root, paths, aliases=aliases)
    return _run(args, root, snapshot)


def _run(args: argparse.Namespace, root: Path, snapshot: _RefreshSnapshot | None) -> int:
    matrix_path = args.matrix
    try:
        matrix = load_matrix(matrix_path)
    except MatrixError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if snapshot is not None:
        # Read-only validation has no destinations to protect or snapshot to
        # extend. Its source and evidence checks still run in validate below.
        inputs, protected_inputs = _matrix_input_paths(matrix, root)
        _check_output_paths(args, root, matrix_path, protected_inputs)
        snapshot.add(inputs)
    refreshed = None
    decisions: list[dict[str, Any]] = []
    outputs: list[tuple[Path, str]] = []
    if args.refresh_inventory:
        inventory_path = args.inventory
        doc = args.generated_doc
        try:
            inventory = load_matrix(inventory_path)
            refreshed, refreshed_matrix, decisions = refresh_inventory(inventory, matrix, root)
            # Matrix precedes inventory even when only canonical formatting changes.
            if refreshed_matrix != matrix or refreshed != inventory:
                outputs.append((matrix_path, json.dumps(refreshed_matrix, indent=2, sort_keys=True) + "\n"))
            if refreshed != inventory:
                outputs.append((inventory_path, json.dumps(refreshed, indent=2, sort_keys=True) + "\n"))
            matrix = refreshed_matrix
        except (MatrixError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        for decision in decisions:
            print("inventory decision: " + json.dumps(decision, sort_keys=True))

    index = index_tests(root)
    errors = validate(matrix, index, root, args.require_landed,
                      inventory=refreshed, inventory_path=args.inventory)
    if errors or decisions:
        for message in errors:
            print(f"error: {message}", file=sys.stderr)
        return 1

    rendered = render_doc(matrix)
    if args.refresh_inventory and not _doc_matches(doc, rendered):
        outputs.insert(0, (doc, rendered))
    if args.write_doc is not None:
        outputs.insert(0, (args.write_doc, rendered))

    if (args.check_doc is not None
            and not any(path.resolve() == args.check_doc.resolve() and text == rendered for path, text in outputs)
            and not _doc_matches(args.check_doc, rendered)):
        print(
            f"error: {args.check_doc} is stale; regenerate with --write-doc",
            file=sys.stderr,
        )
        return 1

    if args.junit:
        try:
            receipt = validate_execution(matrix, index, args.junit, args.discovery)
            matrix_text = next((text for path, text in outputs if path == matrix_path), None)
            receipt["matrix_sha256"] = (hashlib.sha256(matrix_text.encode("utf-8")).hexdigest()
                                        if matrix_text is not None else _sha256(matrix_path))
            receipt["identity"] = _ci_identity(root) if args.ci_context else None
            if args.execution_receipt:
                args.execution_receipt.parent.mkdir(parents=True, exist_ok=True)
                outputs.insert(0, (args.execution_receipt, json.dumps(receipt, indent=2, sort_keys=True) + "\n"))
        except (MatrixError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    automatic_custody = None
    output_journal = _refresh_lock_path(root).with_suffix(".transaction") if outputs else None
    try:
        if outputs and args.preparation_custody is None:
            automatic_custody = _create_cli_custody(output_journal, args.preparation_receipt_file)
            # Deliver independent authority before any payload creation. A
            # caller retains this diagnostic with its run logs for restart.
            print("preparation custody receipt: " + automatic_custody, file=sys.stderr, flush=True)
        _replace_outputs(outputs, private_output=args.execution_receipt,
                         verify=snapshot.verify if snapshot is not None else None,
                         journal=output_journal,
                         custody=(args.preparation_custody or automatic_custody) if outputs else None)
    except (MatrixError, OSError) as exc:
        print(f"error: cannot replace output: {exc}", file=sys.stderr)
        return 2
    finally:
        _retire_automatic_custody(output_journal, automatic_custody)
    if args.junit:
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
