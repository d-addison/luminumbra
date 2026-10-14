#!/usr/bin/env python3
"""Validate the acceptance matrix and render its generated markdown view."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any


SCHEMA = "luminumbra.acceptance_matrix.v2"
DEFAULT_ROOT = Path(__file__).resolve().parents[2]
REQUIREMENT_SOURCE = Path("docs/distant-world.md")
REQUIREMENT_HEADING = "## Acceptance requirement IDs"
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


def _check_planned(add: Any, planned: Any) -> None:
    if not _is_nonempty_str(planned):
        add("planned must be a non-empty descriptive string")
        return
    note, separator, details = planned.partition("; owner:")
    if not note.strip():
        add("planned work description must not be empty")
    if not separator or not details.split(";", 1)[0].strip():
        add("planned must include a non-empty '; owner:' workstream")


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
    try:
        required_ids = load_required_ids(base / REQUIREMENT_SOURCE)
    except MatrixError as exc:
        problems.append(("<matrix>", str(exc)))
        required_ids = frozenset()
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

    problems.extend((row_id, "required mapping missing") for row_id in required_ids - seen_ids)
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
        "change the matrix and regenerate.",
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
