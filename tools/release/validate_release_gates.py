#!/usr/bin/env python3
"""Refuse source publication unless committed qualification and required CI pass."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


GATE_IDS = (
    "required-ci-strict-promotion",
    "final-revision-native-manual",
    "ucrt64-120-second-guard",
    "server-load-qualification",
    "paired-performance-gates",
    "independent-release-verification",
    "owner-native-windows-smoke",
)
RECORD_LIMIT = 64 * 1024
RULES_LIMIT = 1024 * 1024
CHECKS_LIMIT = 16 * 1024 * 1024
DEPTH_LIMIT = 32
MAIN_HISTORY_LIMIT = 100_000
SHA = re.compile(r"[0-9a-fA-F]{40}")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


class GateError(ValueError):
    """A missing, malformed, or unsatisfied publication prerequisite."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> None:
    raise GateError(f"invalid JSON constant: {value}")


def bounded_json_text(data: bytes, limit: int) -> str:
    require(len(data) <= limit, "JSON exceeds size limit")
    depth, quoted, escaped = 0, False, False
    for byte in data:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):
            depth += 1
            require(depth <= DEPTH_LIMIT, "JSON exceeds nesting limit")
        elif byte in (93, 125):
            depth -= 1
    try:
        return data.decode("utf-8")
    except UnicodeError as error:
        raise GateError(f"invalid JSON: {error}") from error


def parse_json(data: bytes, limit: int) -> object:
    try:
        return json.loads(bounded_json_text(data, limit), object_pairs_hook=unique_object,
                          parse_constant=reject_constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise GateError(f"invalid JSON: {error}") from error


def parse_check_pages(data: bytes, limit: int = CHECKS_LIMIT) -> list:
    """Decode concatenated gh pagination objects, or one array of pages."""
    content = bounded_json_text(data, limit)
    decoder = json.JSONDecoder(object_pairs_hook=unique_object, parse_constant=reject_constant)
    pages = []
    position = 0
    try:
        while position < len(content):
            if content[position] in " \t\r\n":
                position += 1
                continue
            page, position = decoder.raw_decode(content, position)
            pages.append(page)
    except (ValueError, RecursionError) as error:
        raise GateError(f"invalid check-run JSON: {error}") from error
    if len(pages) == 1 and type(pages[0]) is list:
        pages = pages[0]
    require(bool(pages) and all(type(page) is dict for page in pages),
            "check-run input must contain page objects")
    return pages


def read_bytes(path: str, limit: int) -> bytes:
    if path == "-":
        return sys.stdin.buffer.read(limit + 1)
    with open(path, "rb") as stream:
        return stream.read(limit + 1)


def read_json(path: str, limit: int) -> object:
    return parse_json(read_bytes(path, limit), limit)


def validate_record(record: object, version: str) -> str:
    require(type(record) is dict and set(record) == {
        "schema_version", "version", "qualified_commit", "gates",
    }, "record must contain exactly schema_version, version, qualified_commit, gates")
    require(type(record["schema_version"]) is int and record["schema_version"] == 1,
            "unsupported schema_version")
    require(type(record["version"]) is str and record["version"] == version,
            "record version does not match release version")
    qualified = record["qualified_commit"]
    require(type(qualified) is str and SHA.fullmatch(qualified) is not None,
            "qualified_commit must be a full 40-hex SHA")
    gates = record["gates"]
    require(type(gates) is list and len(gates) == len(GATE_IDS),
            "record must contain every pre-publication gate")
    for expected, gate in zip(GATE_IDS, gates):
        require(type(gate) is dict and set(gate) == {"id", "status", "evidence"},
                f"invalid gate entry: {expected}")
        require(type(gate["id"]) is str and gate["id"] == expected,
                f"expected gate in order: {expected}")
        require(type(gate["status"]) is str and gate["status"] == "passed",
                f"gate has not passed: {expected}")
        require(type(gate["evidence"]) is str and bool(gate["evidence"].strip()),
                f"gate lacks evidence: {expected}")
    return qualified.lower()


def required_contexts(rules: object) -> tuple[str, ...]:
    """Evaluate the effective main rules response, preserving context order."""
    require(type(rules) is list, "main rules response must be an array")
    contexts = []
    for rule in rules:
        require(type(rule) is dict and type(rule.get("type")) is str,
                "invalid main rule")
        if rule["type"] != "required_status_checks":
            continue
        parameters = rule.get("parameters")
        require(type(parameters) is dict, "missing required status-check parameters")
        require(parameters.get("strict_required_status_checks_policy") is True,
                "main must require strict status checks for promotion")
        checks = parameters.get("required_status_checks")
        require(type(checks) is list and bool(checks), "no required status checks in rule")
        for check in checks:
            require(type(check) is dict and type(check.get("context")) is str
                    and bool(check["context"].strip()), "invalid required context")
            if check["context"] not in contexts:
                contexts.append(check["context"])
    require(bool(contexts), "no required status-check contexts could be read for main")
    return tuple(contexts)


def validate_check_runs(pages: object, contexts: tuple[str, ...], commit: str) -> None:
    """Evaluate all check-run pages; highest run ID is latest per name."""
    require(type(pages) is list and bool(pages), "missing check-run pages")
    latest = {}
    seen = set()
    total = None
    count = 0
    for page in pages:
        require(type(page) is dict, "invalid check-run page")
        page_total = page.get("total_count")
        runs = page.get("check_runs")
        require(type(page_total) is int and page_total >= 0 and type(runs) is list,
                "invalid check-run page fields")
        if total is None:
            total = page_total
        require(page_total == total, "check-run count changed during pagination")
        count += len(runs)
        for run in runs:
            require(type(run) is dict, "invalid check run")
            run_id, name = run.get("id"), run.get("name")
            require(type(run_id) is int and run_id > 0 and run_id not in seen,
                    "invalid or duplicate check-run ID")
            require(type(name) is str and bool(name.strip()), "invalid check-run name")
            require(type(run.get("head_sha")) is str
                    and run["head_sha"].lower() == commit.lower(),
                    "check run is not for the tag commit")
            require(type(run.get("status")) is str
                    and (run.get("conclusion") is None
                         or type(run["conclusion"]) is str), "invalid check-run result")
            seen.add(run_id)
            if name not in latest or run_id > latest[name]["id"]:
                latest[name] = run
    require(count == total, "incomplete check-run pagination")
    for context in contexts:
        run = latest.get(context)
        require(run is not None and run["status"] == "completed"
                and run.get("conclusion") == "success",
                f"latest required check is not completed SUCCESS: {context}")


def git(root: Path, *arguments: str) -> bytes:
    # Ignore location overrides and replacement objects when checking ancestry.
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    result = subprocess.run(["git", "--no-replace-objects", "-C", str(root), *arguments],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    require(result.returncode == 0, f"Git prerequisite failed: {' '.join(arguments)}")
    return result.stdout


def validate_qualified_tree(root: Path, qualified: str, commit: str, record_path: str) -> None:
    git(root, "merge-base", "--is-ancestor", qualified, commit)
    changed = git(root, "diff", "--name-only", "--no-renames", "--no-ext-diff",
                  "--no-textconv", "--ignore-submodules=none", "-z", qualified, commit, "--")
    require(all(path == record_path.encode() for path in changed.split(b"\0") if path),
            "tag changes qualified content outside its gate record")


def main_first_parent_history(root: Path) -> set[str]:
    # Limit output in Git itself; exceeding the bound refuses even a tip match.
    history = git(root, "rev-list", "--first-parent", f"--max-count={MAIN_HISTORY_LIMIT + 1}",
                  "refs/remotes/origin/main").decode("ascii").splitlines()
    require(0 < len(history) <= MAIN_HISTORY_LIMIT, "main first-parent history exceeds bound or is empty")
    require(all(SHA.fullmatch(commit) is not None for commit in history)
            and len(set(history)) == len(history), "invalid main first-parent history")
    return set(history)


def validate_git_record(root: Path, version: str, commit: str) -> None:
    require(VERSION.fullmatch(version) is not None, "version must be X.Y.Z")
    require(SHA.fullmatch(commit) is not None, "tag commit must be a full 40-hex SHA")
    commit = commit.lower()
    require(git(root, "rev-parse", "HEAD^{commit}").decode().strip() == commit,
            "checkout does not match tag commit")
    history = main_first_parent_history(root)
    require(commit in history, "tag commit must lie on main first-parent history")
    record_path = f"docs/release-gates/v{version}.json"
    entry = git(root, "ls-tree", "-z", commit, "--", record_path)
    require(entry.endswith(b"\0") and entry.count(b"\0") == 1, "missing committed gate record")
    metadata, name = entry[:-1].split(b"\t", 1)
    fields = metadata.split()
    require(len(fields) == 3 and fields[0] in (b"100644", b"100755")
            and fields[1] == b"blob" and name == record_path.encode(),
            "gate record must be a regular committed file")
    blob = fields[2].decode("ascii")
    size = int(git(root, "cat-file", "-s", blob))
    require(size <= RECORD_LIMIT, "gate record exceeds size limit")
    record = parse_json(git(root, "cat-file", "blob", blob), RECORD_LIMIT)
    qualified = validate_record(record, version)
    require(qualified in history, "qualified_commit must lie on main first-parent history")
    validate_qualified_tree(root, qualified, commit, record_path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--version", required=True)
    parser.add_argument("--tag-commit", required=True)
    parser.add_argument("--rules", required=True, help="main rules JSON file")
    parser.add_argument("--checks", default="-", help="check-run JSON page stream file or stdin")
    args = parser.parse_args()
    try:
        require(args.rules != "-", "rules must be a file; stdin is reserved for check runs")
        validate_git_record(args.repo_root, args.version, args.tag_commit)
        contexts = required_contexts(read_json(args.rules, RULES_LIMIT))
        validate_check_runs(parse_check_pages(read_bytes(args.checks, CHECKS_LIMIT)),
                            contexts, args.tag_commit)
    except (GateError, OSError, ValueError) as error:
        print(f"release gates: FAIL: {error}", file=sys.stderr)
        return 1
    print("release gates: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
