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
from urllib.parse import parse_qs, urlsplit


GATE_IDS = (
    "required-ci-strict-promotion",
    "final-revision-native-manual",
    "ucrt64-120-second-guard",
    "server-load-qualification",
    "paired-performance-gates",
    "owner-native-windows-smoke",
)
RECORD_LIMIT = 64 * 1024
RULES_LIMIT = 1024 * 1024
CHECKS_LIMIT = 16 * 1024 * 1024
DEPTH_LIMIT = 32
MAIN_HISTORY_LIMIT = 100_000
GIT_STDERR_LIMIT = 4096
SHA = re.compile(r"[0-9a-fA-F]{40}")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
RESULT_URL = re.compile(
    r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/"
    r"(?:actions/runs/[1-9][0-9]*(?:/artifacts/[1-9][0-9]*)?"
    r"|issues/[1-9][0-9]*#issuecomment-[1-9][0-9]*)"
)


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


def rules_links(value: str, page_number: int) -> tuple[str | None, str | None, int | None]:
    """Check pagination links and return the endpoint, next URL and final page."""
    links = {}
    endpoint = None
    last = None
    for entry in value.split(",") if value else ():
        match = re.fullmatch(r'\s*<([^<>\s]+)>;\s*rel="(next|prev|first|last)"\s*', entry)
        require(match is not None, "invalid rules pagination link")
        url, relation = match.groups()
        require(relation not in links, "duplicate rules pagination relation")
        parsed = urlsplit(url)
        query = parse_qs(parsed.query, keep_blank_values=True)
        require(parsed.scheme == "https" and parsed.netloc == "api.github.com"
                and re.fullmatch(r"/repos/[^/]+/[^/]+/rules/branches/main", parsed.path)
                and not parsed.fragment and set(query) == {"page", "per_page"}
                and query["per_page"] == ["100"] and len(query["page"]) == 1
                and re.fullmatch(r"[1-9][0-9]*", query["page"][0]) is not None,
                "invalid rules pagination endpoint")
        target = int(query["page"][0])
        if relation == "next":
            require(target == page_number + 1, "rules pagination skipped a page")
        elif relation == "prev":
            require(target == page_number - 1, "inconsistent previous rules page")
        elif relation == "first":
            require(target == 1, "inconsistent first rules page")
        else:
            require(target >= page_number, "inconsistent last rules page")
            last = target
        require(endpoint is None or endpoint == parsed.path, "rules pagination endpoint changed")
        endpoint = parsed.path
        links[relation] = url
    require(page_number == 1 or "prev" in links, "missing previous rules page link")
    require(last is None or (last > page_number) == ("next" in links),
            "inconsistent last rules page link")
    return endpoint, links.get("next"), last


def parse_rules_pages(data: bytes) -> list:
    """Decode gh api --include --paginate arrays, requiring a complete link chain."""
    require(len(data) <= RULES_LIMIT, "JSON exceeds size limit")
    try:
        content = data.decode("utf-8")
    except UnicodeError as error:
        raise GateError(f"invalid rules response: {error}") from error
    position, page_number = 0, 0
    expected_next, endpoint = None, None
    last_page = None
    rules, seen = [], set()
    while position < len(content):
        position += len(content[position:]) - len(content[position:].lstrip())
        if position == len(content):
            break
        require(page_number == 0 or expected_next is not None, "unexpected extra rules page")
        separator = re.search(r"\r?\n\r?\n", content[position:])
        require(separator is not None, "missing rules response headers")
        header_end = position + separator.start()
        headers = content[position:header_end].splitlines()
        require(bool(headers) and re.fullmatch(r"HTTP/[0-9.]+ 200(?: .*)?", headers[0]),
                "rules response must have HTTP status 200")
        link_headers = []
        for header in headers[1:]:
            name, colon, value = header.partition(":")
            require(bool(colon) and bool(name.strip()), "invalid rules response header")
            if name.lower() == "link":
                link_headers.append(value.strip())
        require(len(link_headers) <= 1, "duplicate rules Link header")
        page_number += 1
        page_endpoint, expected_next, last = rules_links(link_headers[0] if link_headers else "", page_number)
        require(endpoint is None or page_endpoint is None or endpoint == page_endpoint,
                "rules pagination endpoint changed")
        endpoint = page_endpoint or endpoint
        require(last_page is None or last is None or last_page == last,
                "rules page count changed during pagination")
        last_page = last if last is not None else last_page
        position += separator.end()
        position += len(content[position:]) - len(content[position:].lstrip())
        require(position < len(content) and content[position] == "[", "rules page must be an array")
        start, depth, quoted, escaped = position, 0, False, False
        # Find one complete body, bounding nesting before JSON decoding.
        while position < len(content):
            char = content[position]
            position += 1
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char in "[{":
                depth += 1
                require(depth <= DEPTH_LIMIT, "JSON exceeds nesting limit")
            elif char in "]}":
                depth -= 1
                if depth == 0:
                    break
        require(depth == 0 and not quoted, "incomplete rules JSON body")
        page = parse_json(content[start:position].encode("utf-8"), RULES_LIMIT)
        require(type(page) is list and len(page) <= 100, "invalid rules page size")
        require(expected_next is None or len(page) == 100, "inconsistent rules pagination")
        for rule in page:
            require(type(rule) is dict and type(rule.get("ruleset_id")) is int
                    and rule["ruleset_id"] > 0 and type(rule.get("type")) is str,
                    "invalid rules pagination identity")
            identity = (rule["ruleset_id"], rule["type"])
            require(identity not in seen, "duplicate rule during pagination")
            seen.add(identity)
            rules.append(rule)
    require(page_number > 0 and expected_next is None
            and (last_page is None or last_page == page_number), "incomplete rules pagination")
    return rules


def validate_evidence(evidence: str, qualified: str, gate_id: str) -> None:
    fields = evidence.strip().split()
    require(len(fields) == 2 and fields[0].lower() == qualified.lower(),
            f"evidence must name qualified_commit and a concrete record: {gate_id}")
    location = fields[1]
    require(RESULT_URL.fullmatch(location) is not None,
            f"evidence record location must be a supported HTTPS URL: {gate_id}")


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
        validate_evidence(gate["evidence"], qualified, expected)
    return qualified.lower()


def required_contexts(rules: object) -> tuple[tuple[str, int | None], ...]:
    """Preserve every (context, app) requirement, including same-name bindings."""
    # Offline callers may also supply classic required_status_checks protection.
    if type(rules) is dict:
        require(rules.get("strict") is True, "main must require strict status checks for promotion")
        sources = [(rules.get("checks"), "app_id")]
    else:
        require(type(rules) is list, "main rules response must be an array")
        sources = []
        for rule in rules:
            require(type(rule) is dict and type(rule.get("type")) is str, "invalid main rule")
            if rule["type"] != "required_status_checks":
                continue
            parameters = rule.get("parameters")
            require(type(parameters) is dict, "missing required status-check parameters")
            require(parameters.get("strict_required_status_checks_policy") is True,
                    "main must require strict status checks for promotion")
            sources.append((parameters.get("required_status_checks"), "integration_id"))
    contexts = []
    for checks, app_field in sources:
        require(type(checks) is list and bool(checks), "no required status checks in rule")
        for check in checks:
            require(type(check) is dict and type(check.get("context")) is str
                    and bool(check["context"].strip()), "invalid required context")
            app_id = check.get(app_field)
            require(app_id is None or (type(app_id) is int and app_id > 0),
                    "invalid required check app ID")
            context = (check["context"], app_id)
            if context not in contexts:
                contexts.append(context)
    require(bool(contexts), "no required status-check contexts could be read for main")
    return tuple(contexts)


def validate_check_runs(pages: object, contexts: tuple[tuple[str, int | None], ...], commit: str) -> None:
    """Select the highest run ID per requirement, filtering pinned apps first."""
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
            app = run.get("app")
            app_id = app.get("id") if type(app) is dict else None
            selections = [(name, None)]
            if type(app_id) is int and app_id > 0:
                selections.append((name, app_id))
            for context in selections:
                if context not in latest or run_id > latest[context]["id"]:
                    latest[context] = run
    require(count == total, "incomplete check-run pagination")
    for context in contexts:
        run = latest.get(context)
        require(run is not None and run["status"] == "completed"
                and run.get("conclusion") == "success",
                f"latest required check is not completed SUCCESS: {context}")


def git(root: Path, *arguments: str) -> bytes:
    # Ignore inherited location overrides and configuration, and replacement objects.
    # System/global settings must not affect read-only qualification checks.
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    result = subprocess.run(["git", "--no-replace-objects", "-C", str(root), *arguments],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != 0:
        detail = result.stderr[:GIT_STDERR_LIMIT].decode("utf-8", errors="replace").strip()
        if len(result.stderr) > GIT_STDERR_LIMIT:
            detail += "\n[stderr truncated]"
        message = f"Git prerequisite failed: {' '.join(arguments)} (exit {result.returncode})"
        raise GateError(f"{message}: {detail}" if detail else message)
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
    # Use a brace-free peel: MSYS2 runtimes brace-expand argv ("HEAD^{commit}" -> "HEAD^commit").
    require(git(root, "rev-parse", "--verify", "HEAD^0").decode().strip() == commit,
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
    parser.add_argument("--rules", required=True, help="main rules HTTP header/JSON page stream file")
    parser.add_argument("--checks", default="-", help="check-run JSON page stream file or stdin")
    args = parser.parse_args()
    try:
        require(args.rules != "-", "rules must be a file; stdin is reserved for check runs")
        validate_git_record(args.repo_root, args.version, args.tag_commit)
        contexts = required_contexts(parse_rules_pages(read_bytes(args.rules, RULES_LIMIT)))
        validate_check_runs(parse_check_pages(read_bytes(args.checks, CHECKS_LIMIT)),
                            contexts, args.tag_commit)
    except (GateError, OSError, ValueError) as error:
        print(f"release gates: FAIL: {error}", file=sys.stderr)
        return 1
    print("release gates: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
