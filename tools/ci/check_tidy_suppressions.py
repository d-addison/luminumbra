#!/usr/bin/env python3
"""Keep gate suppressions and whole-TU global exemptions on a shrinking budget."""

import argparse
from bisect import bisect_right
from collections import Counter
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import stat
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
BASELINE = "tools/ci/tidy_suppression_baseline.json"
TU_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
            ".ipp", ".tpp", ".inc", ".ixx", ".cppm"}
# Lex comments separately from literals when identifying the enclosing scope.
TOKENS = re.compile(
    r'(?P<comment>//[^\n]*|/\*.*?\*/)'
    r'|(?P<literal>(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\"]{0,16})'
    r'\(.*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')'
    r'|(?P<code>[A-Za-z_][A-Za-z_0-9]*|[^\s])',
    re.DOTALL | re.MULTILINE,
)
# clang-tidy searches raw source, including literals, and accepts a word prefix
# such as XNOLINT. Whitespace before '(' means a blanket suppression.
ANNOTATION = re.compile(r'NOLINT(?P<kind>NEXTLINE|BEGIN|END)?\b(?P<checks>\([^\n)]*\))?')


def read_text(path):
    # Open once and inspect that descriptor; a special file must never block CI.
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
    with os.fdopen(descriptor, "r", encoding="utf-8", newline=None) as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError(f"not a regular source file: {path}")
        return stream.read()


def gate_policy(script):
    fragments = re.findall(r'^gate_checks\+?="([^"\n]*)"$', script, re.MULTILINE)
    checks = "".join(fragments).split(",")
    if not checks or checks.pop(0) != "-*" or not checks or any(not c for c in checks):
        raise ValueError("cannot read gate_checks from scripts/tidy.sh")
    match = re.search(r'^tracked_global_tus=\(\s*\n(.*?)^\)', script, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError("cannot read tracked_global_tus from scripts/tidy.sh")
    exemptions = shlex.split(match[1], comments=True)
    for path in exemptions:
        parts = PurePosixPath(path).parts
        if (not parts or parts[0] not in ("src", "test", "tools")
                or ".." in parts or "$" in path or "\\" in path
                or PurePosixPath(path).suffix not in TU_SUFFIXES):
            raise ValueError(f"invalid tracked global TU: {path}")
    if len(set(exemptions)) != len(exemptions):
        raise ValueError("duplicate tracked global TU")
    return sorted(set(checks)), sorted(exemptions)


def globs_overlap(first, second):
    """Intersection of clang-tidy's '*' globs, bounded by the pattern product."""
    pending = [(0, 0)]
    visited = set()
    while pending:
        left, right = pending.pop()
        if (left, right) in visited:
            continue
        visited.add((left, right))
        if left == len(first) and right == len(second):
            return True
        if left < len(first) and first[left] == "*":
            pending.append((left + 1, right))
            if right < len(second):
                pending.append((left, right + 1))
        elif right < len(second) and second[right] == "*":
            pending.append((left, right + 1))
            if left < len(first):
                pending.append((left + 1, right))
        elif left < len(first) and right < len(second) and first[left] == second[right]:
            pending.append((left + 1, right + 1))
    return False


def suppressions(path, text, checks):
    """Identify each annotation by path, lexical scope and normalized construct.

    Formatting/line-number changes do not buy a new allowance. Repeated identical
    annotations retain their multiplicity, so copying one still grows the budget.
    """
    if "NOLINT" not in text:
        return []
    newlines = [index for index, char in enumerate(text) if char == "\n"]
    code = []
    annotations = []
    for match in ANNOTATION.finditer(text):
        kind = match["kind"] or ""
        if kind == "END":
            continue  # The corresponding BEGIN owns the allowance.
        patterns = (match["checks"][1:-1].split(",")
                    if match["checks"] is not None else ["*"])
        patterns = sorted(p.strip() for p in patterns)
        affected = sorted(c for c in checks if any(
            not p.startswith("-") and globs_overlap(p, c) for p in patterns))
        if affected:
            line = bisect_right(newlines, match.start())
            annotations.append((line + (kind in ("NEXTLINE", "BEGIN")),
                                "NOLINT" + kind, affected, patterns))
    preprocessor_end = 0
    for token in TOKENS.finditer(text):
        line = bisect_right(newlines, token.start())
        if token.lastgroup != "comment":
            if token[0] == "#" and not text[text.rfind("\n", 0, token.start()) + 1:token.start()].strip():
                preprocessor_end = text.find("\n", token.end())
                while preprocessor_end >= 0 and text[preprocessor_end - 1] == "\\":
                    preprocessor_end = text.find("\n", preprocessor_end + 1)
                if preprocessor_end < 0:
                    preprocessor_end = len(text)
            if token.start() >= preprocessor_end:
                code.append((line, token[0]))
    lines = [line for line, _ in code]
    scope = []
    pending = []
    scopes = {}
    wanted = {bisect_right(lines, line - 1) for line, _, _, _ in annotations}
    for index, (_, word) in enumerate(code):
        if index in wanted:
            scopes[index] = list(scope)
        if word == "{":
            scope.append(" ".join(pending))
            pending = []
        elif word == "}":
            if scope:
                scope.pop()
            pending = []
        elif word == ";":
            pending = []
        else:
            pending.append(word)
    result = []
    for line, kind, affected, patterns in annotations:
        index = bisect_right(lines, line - 1)
        target = []
        # Each traversal stops at the next construct boundary or end of source.
        for token_index in range(index, len(code)):
            word = code[token_index][1]
            target.append(word)
            if word in (";", "{", "}"):
                break
        result.append({"path": path, "directive": kind, "checks": affected,
                       "patterns": patterns, "scope": scopes.get(index, []),
                       "target": " ".join(target)})
    return result


def inventory(root):
    checks, exemptions = gate_policy(read_text(root / "scripts/tidy.sh"))
    paths = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z",
         "--", "src", "include", "test", "tools"], cwd=root,
    ).split(b"\0")
    entries = []
    # Git bounds discovery to repository files, including new untracked sources.
    for raw in sorted(set(paths) - {b""}):
        path = os.fsdecode(raw)
        if PurePosixPath(path).suffix in SUFFIXES:
            try:
                text = read_text(root / path)
            except FileNotFoundError:
                continue  # A deleted tracked source retires its allowances.
            entries.extend(suppressions(path, text, checks))
    return {"version": 1, "gate_checks": checks, "suppressions": entries,
            "tracked_global_tus": exemptions}


def budget_errors(actual, baseline):
    if (not isinstance(baseline, dict) or set(baseline) != set(actual)
            or baseline["version"] != 1
            or baseline["gate_checks"] != actual["gate_checks"]):
        return ["baseline schema or gate check set differs"]
    errors = []
    for key in ("suppressions", "tracked_global_tus"):
        if not isinstance(baseline[key], list):
            return [f"invalid baseline {key}"]
        current = Counter(json.dumps(item, sort_keys=True) for item in actual[key])
        allowed = Counter(json.dumps(item, sort_keys=True) for item in baseline[key])
        for item, count in sorted((current - allowed).items()):
            errors.append(f"new {key} ({count}): {item}")
        for item, count in sorted((allowed - current).items()):
            errors.append(f"remove retired {key} from baseline ({count}): {item}")
    return errors


def absolute_header_filter(root):
    # Exact checkout anchoring excludes dependency/build trees even when their
    # nested directories are called include/, src/ or tools/.
    return ("^" + re.escape(root.resolve().as_posix()).replace("/", r"[/\\]")
            + r"[/\\](src|include|test|tools)[/\\]")


def header_smoke(root, database):
    """Measure the shipped filter and an exact-checkout candidate without a build."""
    checks, _ = gate_policy(read_text(root / "scripts/tidy.sh"))
    commands = json.loads(read_text(database))
    if not isinstance(commands, list) or not commands:
        raise ValueError("smoke probe requires a nonempty compile database")
    entry = commands[0]
    if not isinstance(entry, dict) or not all(key in entry for key in ("directory", "file")):
        raise ValueError("invalid smoke compile command")
    script = read_text(root / "scripts/tidy.sh")
    match = re.search(r"^gate_config='(.*?)'\n", script, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError("cannot read gate_config from scripts/tidy.sh")
    config = json.loads(match[1].replace("'\"$gate_checks\"'", ",".join(["-*"] + checks)))
    filters = re.findall(r'"HeaderFilterRegex": "([^"]+)"', script)
    if len(filters) != 2 or len(set(filters)) != 1:
        raise ValueError("expected matching regular and tracked-global header filters")
    candidate = absolute_header_filter(root)
    with tempfile.TemporaryDirectory(prefix="tidy-header-smoke-") as directory:
        temporary = Path(directory)
        unit = temporary / "probe.cpp"
        header = (root / "test/fixtures/tidy_header_smoke.h").resolve().as_posix()
        unit.write_text('#include "' + header + '"\n', encoding="utf-8")
        # Keep the existing compiler, working directory and include environment.
        # The input is deliberately tiny, so the database's first command works
        # for C and C++ configurations alike after selecting C++ explicitly.
        arguments = entry.get("arguments")
        if arguments is None:
            arguments = shlex.split(entry["command"])
        if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
            raise ValueError("invalid smoke compile arguments")
        arguments = [str(unit) if arg == entry["file"] else arg for arg in arguments]
        arguments += ["-x", "c++", "-std=c++20"]
        (temporary / "compile_commands.json").write_text(json.dumps([
            {"directory": entry["directory"], "file": str(unit), "arguments": arguments}]), encoding="utf-8")
        counts = []
        for label, pattern in (("current", filters[0]), ("candidate", candidate)):
            config["HeaderFilterRegex"] = pattern
            result = subprocess.run(["clang-tidy-18", "-p", str(temporary),
                                     "--config=" + json.dumps(config), str(unit)],
                                    capture_output=True, text=True, check=False)
            output = result.stdout + result.stderr
            count = output.count("[modernize-use-nullptr,")
            print(f"header smoke {label}: exit={result.returncode}; modernize-use-nullptr={count}")
            print(output, end="")
            if "[clang-diagnostic-error]" in output or result.returncode != (1 if count else 0):
                raise ValueError(f"{label} header probe failed to analyse")
            counts.append(count)
        if counts[1] != 1:
            raise ValueError("candidate filter did not diagnose the included fixture exactly once")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--header-smoke-db", type=Path,
                        help="measure header coverage using an existing compile database")
    args = parser.parse_args()
    try:
        if args.header_smoke_db is not None:
            header_smoke(args.root, args.header_smoke_db)
            return 0
        actual = inventory(args.root)
        baseline = json.loads(read_text(args.root / BASELINE))
        errors = budget_errors(actual, baseline)
        if errors:
            print("\n".join(errors), file=sys.stderr)
            return 1
        counts = Counter(check for item in actual["suppressions"] for check in item["checks"])
        print(f"tidy suppression budget ok: {len(actual['suppressions'])} annotations; "
              f"{len(actual['tracked_global_tus'])} tracked global TUs")
        for check, count in sorted(counts.items()):
            print(f"  {check}: {count}")
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"tidy suppression budget: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
