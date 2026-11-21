#!/usr/bin/env python3
"""Committed, multiplicity-aware source finding ratchet (stdlib only)."""

from __future__ import annotations

import argparse
from collections import Counter
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tempfile


CONFIG = "tools/ci/ratchet"
REGISTRY = f"{CONFIG}/registry.json"
MAX_INPUT = 10 * 1024 * 1024  # Bound each source/configuration input to 10 MiB.
TOKEN = re.compile(r'(?:u8|[uUL])?R"([^\s()\\]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\])*"|'
                   r"(?<![\w])(?:u8|[uUL])?'(?:\\.|[^'\\\n])*'|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|::|->|!=|==|&&|\|\||\S", re.S)
TRIVIA = re.compile(r'//(?:\\\r?\n|[^\n])*|/\*.*?\*/|(?:u8|[uUL])?R"([^\s()\\]{0,16})\(.*?\)\1"|'
                    r'"(?:\\.|[^"\\])*"|(?<![\w])(?:u8|[uUL])?\'(?:\\.|[^\'\\\n])*\'', re.S)


class AnalysisError(ValueError):
    """Invalid policy, incomplete analysis, or unavailable input."""


def open_input(root, path):
    """Open each POSIX component without following links; verify Windows handles.

    The checkout is developer-owned. Concurrent non-cooperating replacement of
    the checkout root or baseline output directories is outside the write model.
    Input identity/type checks and reads always use the same opened descriptor.
    """
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_BINARY", 0)
    if os.name != "nt":
        directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            parts = path.split("/")
            for part in parts[:-1]:
                child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
                os.close(directory)
                directory = child
            return os.open(parts[-1], flags | os.O_NOFOLLOW, dir_fd=directory)
        finally:
            os.close(directory)
    descriptor = os.open(root / path, flags)
    try:
        import ctypes
        from ctypes import wintypes
        import msvcrt
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        final_path = api.GetFinalPathNameByHandleW
        final_path.argtypes = (wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD)
        final_path.restype = wintypes.DWORD
        # Windows extended paths are bounded at 32,767 UTF-16 code units.
        buffer = ctypes.create_unicode_buffer(32768)
        length = final_path(msvcrt.get_osfhandle(descriptor), buffer, len(buffer), 0)
        if not length or length >= len(buffer):
            raise AnalysisError(f"cannot resolve opened Windows input: {path}")
        actual = buffer.value
        if actual.startswith("\\\\?\\UNC\\"):
            actual = "\\\\" + actual[8:]
        elif actual.startswith("\\\\?\\"):
            actual = actual[4:]
        # root.resolve() and GetFinalPathNameByHandleW both expand 8.3 names.
        if os.path.normcase(actual) != os.path.normcase(str(root / path)):
            raise AnalysisError(f"linked or escaping input: {path}")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def canonical(value):
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def relative_path(path):
    if (not isinstance(path, str) or not path or "\\" in path or ":" in path
            or PurePosixPath(path).is_absolute()
            or any(part in {"", ".", ".."} for part in path.split("/"))):
        raise AnalysisError(f"invalid repository path: {path!r}")
    return path


class Tree:
    """A working tree or immutable Git tree; absence differs from a read failure."""

    def __init__(self, root, ref=None):
        self.root = Path(root).resolve()
        self.ref = None
        self.modes = {}
        if ref is not None:
            if not isinstance(ref, str) or not ref:
                raise AnalysisError("empty or invalid target reference")
            self.ref = self.git("rev-parse", "--verify", "--end-of-options", ref + "^{commit}").strip()
        if self.ref:
            for record in self.git("ls-tree", "-r", "-z", self.ref).split("\0"):
                if record:
                    metadata, path = record.split("\t", 1)
                    self.modes[path] = metadata.split()[0]
            self.paths = set(self.modes)
        else:
            self.paths = set(self.git("ls-files", "--cached", "--others", "--exclude-standard", "-z").split("\0")) - {""}

    def git(self, *args):
        # Bound reads before allocating a subprocess result, including tree lists.
        with subprocess.Popen(["git", *args], cwd=self.root, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL) as process:
            payload = process.stdout.read(MAX_INPUT + 1)
            if len(payload) > MAX_INPUT:
                process.kill()
                raise AnalysisError("Git input exceeds 10 MiB")
            if process.wait():
                raise AnalysisError(f"git {' '.join(args)} failed")
        try:
            return payload.decode("utf-8")
        except UnicodeError as error:
            raise AnalysisError("non-UTF-8 Git input") from error

    def read(self, path):
        relative_path(path)
        if path not in self.paths:
            raise AnalysisError(f"missing input: {path}")
        if self.ref:
            if self.modes[path] not in {"100644", "100755"}:
                raise AnalysisError(f"non-regular Git input: {path}")
            value = self.git("show", f"{self.ref}:{path}")
            if len(value.encode("utf-8")) > MAX_INPUT:
                raise AnalysisError(f"input exceeds 10 MiB: {path}")
            return value
        with os.fdopen(open_input(self.root, path), "rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat.S_ISREG(before.st_mode):
                raise AnalysisError(f"non-regular input: {path}")
            if before.st_size < 0 or before.st_size > MAX_INPUT:
                raise AnalysisError(f"input exceeds 10 MiB: {path}")
            payload = stream.read(MAX_INPUT + 1)
            after = os.fstat(stream.fileno())
            if (len(payload) != before.st_size or before.st_size != after.st_size
                    or before.st_mtime_ns != after.st_mtime_ns or before.st_ctime_ns != after.st_ctime_ns):
                raise AnalysisError(f"incomplete or changing input: {path}")
        if len(payload) > MAX_INPUT:
            raise AnalysisError(f"input exceeds 10 MiB: {path}")
        return payload.decode("utf-8")

    def json(self, path):
        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise AnalysisError(f"duplicate JSON key in {path}: {key}")
                result[key] = value
            return result
        return json.loads(self.read(path), object_pairs_hook=unique)


def mask(source, literals=True):
    """Keep offsets/newlines while removing comments and, optionally, literals."""
    def replace(match):
        value = match.group()
        if not literals and not value.startswith(("//", "/*")):
            return value
        return "".join("\n" if char == "\n" else " " for char in value)
    return TRIVIA.sub(replace, source)


def normalize(source):
    return " ".join(match.group() for match in TOKEN.finditer(mask(source, literals=False)))


def pairs(source, opening, closing):
    stack, result = [], {}
    for index, char in enumerate(source):
        if char == opening:
            stack.append(index)
        elif char == closing:
            if not stack:
                raise AnalysisError(f"unmatched {closing}")
            result[stack.pop()] = index
    if stack:
        raise AnalysisError(f"unmatched {opening}")
    return result


def functions(source):
    """Lexical named functions, including inline class methods and nested lambdas.

    This deliberately does not claim C++ AST or preprocessor analysis. Unsupported
    or unresolved manifest roots fail instead of contributing an empty result.
    """
    clean = mask(source)
    if '"' in clean or "/*" in clean:
        raise AnalysisError("unterminated string or comment")
    braces, parens = pairs(clean, "{", "}"), pairs(clean, "(", ")")
    classes = []
    for match in re.finditer(r"\b(?:class|struct)\s+(\w+)[^;{}]*\{", clean):
        start = match.end() - 1
        classes.append((start, braces[start], match[1]))
    namespaces = []
    for match in re.finditer(r"\bnamespace\s*(?:([A-Za-z_][\w:]*)\s*)?\{", clean):
        start = match.end() - 1
        namespaces.append((start, braces[start], match[1] or "(anonymous)"))
    result = []
    declaration = re.compile(r"\b((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)\s*\(")
    suffix = re.compile(r"\s*(?:(?:const|noexcept|override|final)\s*)*(?:->[^;{}]+)?\{")
    for match in declaration.finditer(clean):
        start = match.end() - 1
        end = parens[start]
        tail = suffix.match(clean, end + 1)
        if tail is None:
            continue
        name = match[1]
        if name in {"if", "for", "while", "switch", "catch", "noexcept", "decltype", "sizeof", "alignof"}:
            continue
        if "::" not in name:
            owners = [item for item in classes if item[0] < start < item[1]]
            if owners:
                name = "::".join(item[2] for item in sorted(owners)) + "::" + name
        body = tail.end() - 1
        prefix = "::".join(item[2] for item in sorted(namespaces) if item[0] < start < item[1])
        symbol = (prefix + "::" if prefix else "") + name + normalize(source[start:end + 1]) + normalize(clean[end + 1:body])
        result.append((name, symbol, body + 1, braces[body]))
    return clean, parens, result


def finding(rule, path, symbol, construct, count=1):
    construct = normalize(construct)
    key = hashlib.sha256("\0".join((rule, path, symbol, construct)).encode("utf-8")).hexdigest()
    return dict(rule=rule, file=path, symbol=symbol, construct=construct, key=key, count=count)


def registry(tree, optional=False):
    if optional and REGISTRY not in tree.paths:
        # A missing initial registry is valid only if there is no orphan policy.
        if any(path.startswith(CONFIG + "/") for path in tree.paths):
            raise AnalysisError("missing registry with existing ratchet policy")
        return {}
    data = tree.json(REGISTRY)
    if (not isinstance(data, dict) or set(data) != {"schema", "rules"}
            or type(data["schema"]) is not int or data["schema"] != 1
            or not isinstance(data["rules"], list) or not data["rules"]):
        raise AnalysisError("invalid registry")
    result = {}
    fields = {"id", "family", "description", "rationale", "scope", "auto_fix", "adapter", "pattern", "anchor"}
    for rule in data["rules"]:
        if not isinstance(rule, dict) or set(rule) != fields or any(not isinstance(value, str) or not value for value in rule.values()):
            raise AnalysisError("invalid rule fields")
        for field in ("id", "family", "scope"):
            if not re.fullmatch(r"[a-z][a-z0-9-]*", rule[field]):
                raise AnalysisError(f"invalid rule {field}")
        if rule["id"] in result or rule["adapter"] != "source-pattern" or rule["anchor"] not in {"call", "match"}:
            raise AnalysisError("duplicate rule or unsupported adapter/anchor")
        try:
            re.compile(rule["pattern"])
        except re.error as error:
            raise AnalysisError(f"invalid rule pattern: {rule['id']}: {error}") from error
        result[rule["id"]] = rule
    return result


def scopes(tree, rules):
    result = {}
    for area in sorted({rule["scope"] for rule in rules.values()}):
        data = tree.json(f"{CONFIG}/scopes/{area}.json")
        if (not isinstance(data, dict) or set(data) != {"schema", "groups"}
                or type(data["schema"]) is not int or data["schema"] != 1
                or not isinstance(data["groups"], dict)):
            raise AnalysisError(f"invalid scope: {area}")
        for rule_id, declarations in data["groups"].items():
            if (rule_id not in rules or rules[rule_id]["scope"] != area
                    or not isinstance(declarations, list) or not declarations):
                raise AnalysisError(f"unknown or empty scope group: {rule_id}")
            for declaration in declarations:
                if (not isinstance(declaration, dict) or set(declaration) != {"files", "symbols", "frequency"}
                        or declaration["frequency"] not in ("frame", "tick", "owner")):
                    raise AnalysisError(f"invalid scope declaration: {rule_id}")
                for field in ("files", "symbols"):
                    if (not isinstance(declaration[field], list) or not declaration[field]
                            or any(not isinstance(value, str) or not value for value in declaration[field])):
                        raise AnalysisError(f"invalid scope {field}: {rule_id}")
                for path in declaration["files"]:
                    relative_path(path)
            result[rule_id] = declarations
    if set(result) != set(rules):
        raise AnalysisError("missing rule scope")
    return result


def analyze(tree, rules, scope):
    parsed, entries = {}, {}
    for rule_id, rule in sorted(rules.items()):
        selected = set()
        for declaration in scope[rule_id]:
            paths = set()
            for glob in declaration["files"]:
                matches = {path for path in tree.paths if fnmatch.fnmatchcase(path, glob)}
                if not matches:
                    raise AnalysisError(f"missing scope input: {glob}")
                paths.update(matches)
            for pattern in declaration["symbols"]:
                resolved = False
                for path in sorted(paths):
                    if path not in parsed:
                        source = tree.read(path)
                        # A whole-source policy must scan every input, but need
                        # not parse functions in files with no candidate at all.
                        if pattern == "*" and not re.search(rule["pattern"], mask(source)):
                            resolved = True
                            continue
                        parsed[path] = (source, *functions(source))
                    if pattern == "*":
                        resolved = True
                        clean = parsed[path][1]
                        for match in re.finditer(rule["pattern"], clean):
                            if not any(begin <= match.start() < end for _, _, begin, end in parsed[path][3]):
                                raise AnalysisError(f"cannot identify enclosing function: {path}")
                    for name, symbol, begin, end in parsed[path][3]:
                        if fnmatch.fnmatchcase(name, pattern):
                            selected.add((path, symbol, begin, end))
                            resolved = True
                if not resolved:
                    raise AnalysisError(f"unresolved scope symbol: {pattern}")
        for path, symbol, begin, end in sorted(selected):
            source, clean, parens, _ = parsed[path]
            for match in re.finditer(rule["pattern"], clean[begin:end]):
                first, last = begin + match.start(), begin + match.end()
                enclosing = max((item for item in parsed[path][3] if item[2] <= first < item[3]),
                                key=lambda item: item[2])
                if enclosing[1] != symbol:
                    continue
                if rule["anchor"] == "call":
                    if clean[last - 1] != "(" or last - 1 not in parens:
                        raise AnalysisError(f"invalid call adapter result: {rule_id}")
                    last = parens[last - 1] + 1
                    receiver = re.search(r"(?:\b[A-Za-z_]\w*\s*(?:::|\.|->)\s*)+$", clean[begin:first])
                    if receiver:
                        first = begin + receiver.start()
                item = finding(rule_id, path, symbol, source[first:last])
                if item["key"] in entries:
                    entries[item["key"]]["count"] += 1
                else:
                    entries[item["key"]] = item
    return entries


def baselines(tree, rules):
    result = {}
    expected = {f"{CONFIG}/baselines/{rule['family']}/{rule['scope']}.json" for rule in rules.values()}
    actual = {path for path in tree.paths if path.startswith(CONFIG + "/baselines/")}
    if actual - expected:
        raise AnalysisError(f"unknown baseline shards: {sorted(actual - expected)}")
    for path in sorted(expected):
        data = tree.json(path)
        if (not isinstance(data, dict) or set(data) != {"schema", "entries"}
                or type(data["schema"]) is not int or data["schema"] != 1 or not isinstance(data["entries"], list)):
            raise AnalysisError(f"invalid baseline: {path}")
        for item in data["entries"]:
            if (not isinstance(item, dict) or set(item) != {"key", "rule", "file", "symbol", "construct", "count"}
                    or any(not isinstance(item[field], str) for field in ("key", "rule", "file", "symbol", "construct"))):
                raise AnalysisError(f"invalid entry: {path}")
            rule = rules.get(item["rule"])
            if rule is None or path != f"{CONFIG}/baselines/{rule['family']}/{rule['scope']}.json":
                raise AnalysisError(f"unknown or misplaced rule: {path}")
            relative_path(item["file"])
            if (type(item["count"]) is not int or item["count"] <= 0
                    or any(not isinstance(item[field], str) or not item[field] for field in ("symbol", "construct"))
                    or finding(item["rule"], item["file"], item["symbol"], item["construct"], item["count"]) != item
                    or item["key"] in result):
                raise AnalysisError(f"invalid or duplicate baseline key: {path}")
            result[item["key"]] = item
    return result


def excess(observed, allowed):
    return [dict(item, count=item["count"] - allowed.get(key, {}).get("count", 0))
            for key, item in sorted(observed.items())
            if item["count"] > allowed.get(key, {}).get("count", 0)]


def evaluate(current, target, updating=False):
    rules = registry(current)
    old_rules = registry(target, optional=True)
    scope, old_scope = scopes(current, rules), scopes(target, old_rules)
    for rule_id, old in old_rules.items():
        if rule_id not in rules:
            raise AnalysisError(f"cannot remove registered rule: {rule_id}")
        for field in ("family", "scope", "adapter", "pattern", "anchor"):
            if rules[rule_id][field] != old[field]:
                raise AnalysisError(f"cannot change existing rule policy: {rule_id}.{field}")
        if any(declaration not in scope[rule_id] for declaration in old_scope[rule_id]):
            raise AnalysisError(f"cannot remove existing scope: {rule_id}")
    observed = analyze(current, rules, scope)  # Complete every adapter before any mutation.
    submitted, previous = baselines(current, rules), baselines(target, old_rules)
    old_submitted = {key: value for key, value in submitted.items() if value["rule"] in old_rules}
    failures = excess(old_submitted, previous)
    new_observed = {key: value for key, value in observed.items() if value["rule"] not in old_rules}
    # Seeds may contain only findings that actually exist now, never spare allowance.
    seeds = {key: value for key, value in submitted.items() if value["rule"] not in old_rules}
    if not updating:
        failures.extend(excess(seeds, new_observed))
    allowed = dict(previous) if updating else dict(submitted)
    if updating:
        # Both target and submitted allowances must permit existing-rule findings.
        failures.extend(excess({key: value for key, value in observed.items() if value["rule"] in old_rules}, submitted))
        allowed.update(new_observed)
    failures.extend(excess(observed, allowed))
    if not updating and not failures and submitted != observed:
        raise AnalysisError("stale baseline; run update --shrink-only")
    unique = {canonical(item): item for item in failures}
    return rules, observed, [unique[key] for key in sorted(unique)]


def update(tree, rules, observed):
    """Validate/serialize first, stage closed files, then replace; roll back failures."""
    shards = {}
    for rule in rules.values():
        shards[f"{CONFIG}/baselines/{rule['family']}/{rule['scope']}.json"] = []
    for item in observed.values():
        rule = rules[item["rule"]]
        shards[f"{CONFIG}/baselines/{rule['family']}/{rule['scope']}.json"].append(item)
    staged, backups, replaced = {}, {}, []
    try:
        for path, entries in sorted(shards.items()):
            original = tree.read(path)
            payload = canonical(dict(schema=1, entries=sorted(entries, key=lambda item: item["key"])))
            if payload == original:
                continue
            for contents, destinations in ((original, backups), (payload, staged)):
                with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                                 dir=tree.root / PurePosixPath(path).parent,
                                                 delete=False) as stream:
                    destinations[path] = Path(stream.name)
                    stream.write(contents)
                    stream.flush()
        for path, temporary in staged.items():
            os.replace(temporary, tree.root / path)
            replaced.append(path)
    except OSError:
        for path in reversed(replaced):
            os.replace(backups[path], tree.root / path)
        raise
    finally:
        for temporary in (*staged.values(), *backups.values()):
            temporary.unlink(missing_ok=True)


def main(argv=None, root=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check").add_argument("--target")
    updater = commands.add_parser("update")
    updater.add_argument("--shrink-only", required=True, action="store_true")
    updater.add_argument("--target")
    commands.add_parser("explain").add_argument("rule")
    args = parser.parse_args(argv)
    try:
        current = Tree(root or Path(__file__).resolve().parents[2])
        if args.command == "explain":
            rules = registry(current)
            if args.rule not in rules:
                raise AnalysisError(f"unknown rule: {args.rule}")
            print(canonical(rules[args.rule]), end="")
            return 0
        target = Tree(current.root, args.target if args.target is not None else "HEAD")
        rules, observed, failures = evaluate(current, target, args.command == "update")
        if failures:
            print(canonical(dict(new_entries=failures)), end="")
            return 1
        if args.command == "update":
            update(current, rules, observed)
        counts = Counter()
        for item in observed.values():
            counts[item["rule"]] += item["count"]
        print(canonical(dict(status="ok", counts={rule: counts[rule] for rule in sorted(rules)})), end="")
        return 0
    except (AnalysisError, OSError, UnicodeError, ValueError, TypeError, KeyError, re.error) as error:
        print(f"ratchet analysis error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
