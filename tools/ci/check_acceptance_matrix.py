#!/usr/bin/env python3
"""Validate the acceptance matrix and render its generated markdown view."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any


SCHEMA = "luminumbra.acceptance_matrix.v1"
DEFAULT_ROOT = Path(__file__).resolve().parents[2]
STATUS_ORDER = ("covered", "partial", "needs-native-run", "planned")
VALID_STATUSES = frozenset(STATUS_ORDER)
EVIDENCE_KINDS = frozenset({"ctest", "script", "native", "doc"})

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


def _is_positive_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


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


def _check_planned(add: Any, planned: Any) -> None:
    if not isinstance(planned, dict):
        add("planned must be an object")
        return
    if not _is_nonempty_str(planned.get("owner")):
        add("planned.owner must be a non-empty string")
    for key in ("pr", "issue"):
        if key in planned and not _is_positive_int(planned[key]):
            add(f"planned.{key} must be a positive integer")
    if "note" in planned and not isinstance(planned["note"], str):
        add("planned.note must be a string")


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
        if not _is_nonempty_str(item.get("command")):
            add("native command must be a non-empty string")
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
    rows = matrix.get("rows")

    if matrix.get("schema") != SCHEMA:
        problems.append(("<matrix>", f"schema must be {SCHEMA}"))
    if not isinstance(rows, list) or not rows:
        problems.append(("<matrix>", "rows must be a non-empty list"))
        return [f"{label}: {message}" for label, message in sorted(problems)]

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

    return [f"{label}: {message}" for label, message in sorted(problems)]


def _escape_cell(text: str) -> str:
    return text.replace("|", "\\|")


def _render_item(item: dict[str, Any]) -> str:
    text = f"{item['kind']}: `{item['ref']}`"
    planned = item.get("planned")
    if isinstance(planned, dict):
        details = [str(planned["owner"])]
        if "pr" in planned:
            details.append(f"change {planned['pr']}")
        if "issue" in planned:
            details.append(f"issue #{planned['issue']}")
        inner = ", ".join(details)
        if planned.get("note"):
            inner += f" - {planned['note']}"
        text += f" (planned: {inner})"
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
        "change the matrix and regenerate.",
        "",
        "| Status | Rows |",
        "| --- | ---: |",
    ]
    lines.extend(f"| {status} | {counts[status]} |" for status in STATUS_ORDER)
    lines.append(f"| Total | {len(rows)} |")
    lines.extend(
        [
            "",
            "| Row | Status | Statement | Evidence |",
            "| --- | --- | --- | --- |",
        ]
    )
    for row in rows:
        evidence = "<br>".join(_render_item(item) for item in row["evidence"])
        lines.append(
            f"| {_escape_cell(row['id'])} | {_escape_cell(row['status'])} "
            f"| {_escape_cell(row['statement'])} | {evidence} |"
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
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return int(exc.code or 0)

    root = args.root.resolve()
    matrix_path = args.matrix if args.matrix is not None else root / "tools" / "ci" / "acceptance_matrix.json"
    try:
        matrix = load_matrix(matrix_path)
    except MatrixError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    errors = validate(matrix, index_tests(root), root, args.require_landed)
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

    print(f"acceptance matrix ok: {len(matrix['rows'])} rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
