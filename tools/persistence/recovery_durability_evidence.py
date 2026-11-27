#!/usr/bin/env python3
"""Validate retained recovery evidence without launching native programs.

This diagnostic contract does not authenticate a producer or qualify physical
power loss. A complete synthetic matrix never establishes native qualification.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "perf"))
from fixture_paths import read_bytes, resolve_reference

SCHEMA = "luminumbra.recovery_durability_evidence.v1"
LANES = ("windows-msvc", "windows-ucrt64", "posix")
CASES = ("matching_restore", "absent_create", "abort_previous_commit",
         "abort_initial_creation", "cleanup_committed_journal", "cleanup_directory_retirement")
MECHANISMS = ("injected_fault", "process_termination", "abrupt_vm_reset")
# Diagnostic parsing budgets only; no engine, persistence or operation limits change.
MAX_ARTIFACT_BYTES = 1024 * 1024
MAX_TOTAL_BYTES = 64 * MAX_ARTIFACT_BYTES
MAX_ARTIFACTS = 512
MAX_RECORDS = 4096
MAX_EVENTS = 256
MAX_TESTS = 10000


class Refusal(ValueError):
    """Evidence cannot establish the requested contract."""


def require(condition, message):
    if not condition:
        raise Refusal(message)


def text(value):
    require(type(value) is str and bool(value.strip()) and len(value) <= 1024,
            "missing or invalid text")
    return value


def digest(value, length=64):
    require(type(value) is str and re.fullmatch(r"[0-9a-f]{" + str(length) + r"}", value),
            "invalid identity digest")
    return value


def relative_name(value):
    text(value)
    require(len(value) <= 256 and not any(c in value for c in ("\\", ":", "\0")),
            "invalid relative path")
    parts = value.split("/")
    require(len(parts) <= 8 and all(part not in ("", ".", "..") for part in parts),
            "invalid relative path")
    return value


def unsigned(value):
    require(type(value) is int and 0 <= value <= (1 << 64) - 1, "invalid unsigned integer")
    return value


def context_digest(lane):
    context = {key: lane[key] for key in ("lane", "candidate", "binary_sha256", "compiler",
                                         "configuration", "environment")}
    raw = json.dumps(context, sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def parse_json(data):
    def invalid_constant(_):
        raise Refusal("non-finite JSON")

    def finite_float(token):
        value = float(token)
        require(math.isfinite(value), "non-finite JSON")
        return value

    value = json.loads(data, object_pairs_hook=object_pairs, parse_constant=invalid_constant, parse_float=finite_float)
    require(type(value) is dict, "expected JSON object")
    return value


class Artifacts:
    """Read each bounded artifact through the shared confined descriptor reader."""

    def __init__(self, root):
        self.root = root
        self.data = {}
        self.total = 0

    def read(self, name):
        relative_name(name)
        if name not in self.data:
            require(len(self.data) < MAX_ARTIFACTS, "artifact count budget exceeded")
            limit = min(MAX_ARTIFACT_BYTES, MAX_TOTAL_BYTES - self.total)
            require(limit > 0, "artifact byte budget exceeded")
            try:
                # The shared reader permits one lookahead byte to detect growth.
                data = read_bytes(self.root, resolve_reference(self.root, name, max_depth=8), limit=limit)
            except (ValueError, OSError):
                # A failed read can consume bytes too. Conservatively charge its
                # whole allowance so repeated partial reads cannot evade the cap.
                self.total += limit
                raise
            require(data, "empty artifact")
            self.total += len(data)
            self.data[name] = data
        return self.data[name]

    def reference(self, ref):
        require(type(ref) is dict and set(ref) == {"path", "sha256"}, "invalid artifact reference")
        data = self.read(ref["path"])
        require(hashlib.sha256(data).hexdigest() == digest(ref["sha256"]), "artifact digest mismatch")
        return data

    def document(self, ref, schema, identity):
        document = parse_json(self.reference(ref))
        require(document.get("schema") == schema, "unsupported artifact schema")
        require(document.get("identity") == identity, "stale or mismatched artifact identity")
        return document


def tree_receipt(document, phase):
    require(document.get("phase") == phase, "wrong member-tree phase")
    tree = document["tree"]
    require(tree["schema"] == "luminumbra.save_receipt.v1", "unsupported member-tree schema")
    files = tree["files"]
    require(type(files) is list and len(files) <= MAX_RECORDS, "member-tree record budget exceeded")
    previous = ""
    total = 0
    fold = hashlib.sha256()
    for item in files:
        name = relative_name(item["path"])
        require(name > previous, "member-tree paths must be unique and sorted")
        previous = name
        size = unsigned(item["size"])
        total += size
        require(total <= (1 << 64) - 1, "member-tree total overflow")
        sha = digest(item["sha256"])
        fold.update(f"{name}\0{size}\0{sha}\n".encode("utf-8"))
    require(unsigned(tree["file_count"]) == len(files) and unsigned(tree["total_bytes"]) == total
            and tree["tree_digest"] == fold.hexdigest(), "member-tree aggregate mismatch")
    return {item["path"]: (item["size"], item["sha256"]) for item in files}


def discovery_tests(data):
    document = parse_json(data)
    version = document.get("version")
    require(document.get("kind") == "ctestInfo" and type(version) is dict
            and version == {"major": 1, "minor": 0} and all(type(value) is int for value in version.values()),
            "raw CTest JSON discovery required")
    tests = document["tests"]
    require(type(tests) is list and 0 < len(tests) <= MAX_TESTS, "missing or oversized discovery")
    names = set()
    for test in tests:
        name = text(test["name"])
        require(name not in names, "duplicate discovered test")
        names.add(name)
        command = test["command"]
        require(type(command) is list and command and all(type(arg) is str for arg in command),
                "missing discovered test command")
    return names


class JUnitTreeBuilder(ET.TreeBuilder):
    """Enforce the node budget before constructing each XML element."""

    def __init__(self):
        super().__init__()
        self.nodes = 0

    def start(self, tag, attrs):
        require(self.nodes < MAX_TESTS * 8, "JUnit node budget exceeded")
        self.nodes += 1
        return super().start(tag, attrs)

    def doctype(self, name, public_id, system_id):
        raise Refusal("DTD is not evidence")


def passed_tests(data):
    # The target refuses actual DTDs in every XML encoding. Literal DTD text
    # inside captured stdout is harmless; it is not a declaration.
    root = ET.fromstring(data, parser=ET.XMLParser(target=JUnitTreeBuilder()))
    require(root.tag in ("testsuite", "testsuites"), "raw JUnit required")
    nodes = list(root.iter())
    # Sum each subtree once, including nested suites, within the XML node budget.
    counts = {}
    for node in reversed(nodes):
        counts[node] = (1 if node.tag == "testcase" else 0) + sum(counts[child] for child in node)
        if node.tag in ("testsuite", "testsuites") and node.get("tests") is not None:
            declared = node.get("tests")
            require(re.fullmatch(r"[0-9]{1,5}", declared) is not None
                    and int(declared) == counts[node], "inconsistent JUnit suite count")
    for node in nodes:
        require(node.tag not in ("failure", "error", "skipped"), "failed or skipped JUnit evidence")
        if node.tag in ("testsuite", "testsuites"):
            for field in ("failures", "errors", "skipped", "disabled"):
                require(node.get(field, "0") == "0", "incomplete JUnit suite")
    cases = list(root.iter("testcase"))
    require(0 < len(cases) <= MAX_TESTS, "missing or oversized JUnit")
    names = set()
    for case in cases:
        name = text(case.get("name"))
        require(name not in names, "duplicate JUnit testcase")
        names.add(name)
        require(case.get("status") in (None, "run", "passed") and
                case.get("result") in (None, "completed"), "unevaluated JUnit testcase")
    return names


def validate_trace(document, case, mechanism, before, interrupted, after, expected):
    events = document["events"]
    require(type(events) is list and 0 < len(events) <= MAX_EVENTS, "missing or oversized trace")
    # Every recorded event is accounted for. Extra payload attempts before the
    # index barrier or after a failed barrier cannot hide behind a later retry.
    prefix = {
        "matching_restore": "matching_index_observed",
        "absent_create": "absent_index_observed",
        "abort_previous_commit": "previous_commit_abort_started",
        "abort_initial_creation": "initial_creation_abort_started",
        "cleanup_committed_journal": "committed_journal_cleanup_started",
        "cleanup_directory_retirement": "directory_retirement_started",
    }[case]
    cleanup = case.startswith("cleanup_")
    boundary = "cleanup_completion" if cleanup else "index_completion"
    interruption = {"injected_fault": "fault_injected", "process_termination": "process_terminated",
                    "abrupt_vm_reset": "vm_reset_observed"}[mechanism]
    if mechanism == "injected_fault":
        required = [prefix, interruption, boundary + "_failed", "recovery_retained"]
    else:
        required = [prefix, boundary + "_pending", interruption, "recovery_retained"]
    required += ["retry_started"]
    required += (["cleanup_complete"] if cleanup else
                 ["index_complete", "payload_started", "payload_complete"])
    required += ["recovery_retired"]
    require(events == required, "trace ordering, failure retention or retry evidence incomplete")
    require(after == expected, "after tree differs from independently prepared expected tree")
    index = relative_name(document["index_path"])
    payload = relative_name(document["payload_path"])
    require(index != payload, "index and payload must be distinct")
    retained = document["recovery_paths"]
    require(type(retained) is list and 0 < len(retained) <= MAX_RECORDS, "missing recovery inventory")
    for name in retained:
        relative_name(name)
    require(len(set(retained)) == len(retained) and not {index, payload}.intersection(retained),
            "invalid recovery inventory")
    require(all(name in interrupted and name not in after for name in retained),
            "interrupted recovery evidence missing or not retired")
    if case in ("matching_restore", "abort_previous_commit"):
        require(index in before and payload in before and before == expected,
                "restoration must preserve the committed member tree")
        if case == "matching_restore":
            require(interrupted.get(index) == before[index] and interrupted.get(payload) == before[payload],
                    "matching restore did not observe matching member bytes")
    elif case in ("absent_create", "abort_initial_creation"):
        require(index not in before and payload not in before and before == expected,
                "created members must remain absent after recovery")
        if case == "absent_create":
            require(index not in interrupted and payload not in interrupted, "retry did not observe absent creates")
    else:
        require(index in before and payload in before, "cleanup lacks committed member inventory")
        require(all(after.get(name) == entry for name, entry in before.items() if name in (index, payload)),
                "cleanup changed committed members")
        require(set(before) - set(after) == set(retained) and not (set(after) - set(before))
                and all(before[name] == after[name] for name in after), "cleanup inventory mismatch")
        require(all(interrupted.get(name) == entry for name, entry in after.items()),
                "interrupted cleanup changed committed members")


def validate_interruption(artifacts, run, identity):
    mechanism = run["mechanism"]
    if mechanism == "injected_fault":
        require("interruption" not in run, "injected faults are not external interruptions")
        return
    receipt = artifacts.document(run["interruption"], "luminumbra.recovery_interruption.v1", identity)
    require(receipt["method"] == mechanism and receipt["observed"] is True, "interruption not observed")
    text(receipt["controller"])
    artifacts.reference(receipt["controller_log"])
    if mechanism == "abrupt_vm_reset":
        require(receipt["graceful_shutdown"] is False and receipt["guest_boot_before"] != receipt["guest_boot_after"],
                "abrupt reset requires distinct guest boots without graceful shutdown")
        text(receipt["guest_boot_before"])
        text(receipt["guest_boot_after"])
        text(receipt["virtual_storage_stack"])
    else:
        require(receipt["guest_boot_before"] == receipt["guest_boot_after"], "process kill is not a VM reset")
        text(receipt["guest_boot_before"])


def validate_lane(artifacts, lane, candidate, expected_build):
    require(lane["candidate"] == candidate, "stale lane candidate")
    require(digest(lane["binary_sha256"]) == expected_build, "stale build identity")
    text(lane["compiler"])
    text(lane["configuration"])
    environment = lane["environment"]
    require(type(environment) is dict, "missing storage environment")
    for key in ("os", "filesystem", "storage", "cache", "virtualization"):
        require(text(environment[key]).strip().casefold() not in ("unknown", "unavailable", "pending", "unrecorded"),
                "unknown storage environment")
    require(environment["virtualization"].strip().casefold() != "none",
            "abrupt-reset qualification requires a VM storage scope")
    discovered = discovery_tests(artifacts.reference(lane["discovery"]))
    passed = passed_tests(artifacts.reference(lane["junit"]))
    require(passed <= discovered, "JUnit contains undiscovered tests")
    runs = lane["runs"]
    require(type(runs) is list and len(runs) <= len(CASES) * len(MECHANISMS), "too many or invalid runs")
    selected = {}
    ids = set()
    testcases = set()
    for run in runs:
        key = (run["case"], run["mechanism"])
        require(key[0] in CASES and key[1] in MECHANISMS and key not in selected, "unknown or duplicate matrix cell")
        run_id = text(run["id"])
        require(run_id not in ids and text(run["testcase"]) not in testcases, "duplicate run identity or testcase")
        ids.add(run_id)
        testcases.add(run["testcase"])
        selected[key] = run
    results = []
    for case in CASES:
        for mechanism in MECHANISMS:
            result = {"lane": lane["lane"], "case": case, "mechanism": mechanism, "status": "unevaluated"}
            try:
                require((case, mechanism) in selected, "missing matrix cell")
                run = selected[(case, mechanism)]
                require(run["testcase"] in passed and type(run["exit_code"]) is int and run["exit_code"] == 0,
                        "missing or unsuccessful executed test")
                identity = {"candidate": candidate, "lane": lane["lane"], "binary_sha256": expected_build,
                            "context_sha256": context_digest(lane),
                            "run_id": run["id"], "case": case, "mechanism": mechanism}
                trees = [tree_receipt(artifacts.document(run[phase], "luminumbra.recovery_member_tree.v1", identity), phase)
                         for phase in ("before", "interrupted", "after", "expected")]
                trace = artifacts.document(run["trace"], "luminumbra.recovery_trace.v1", identity)
                validate_trace(trace, case, mechanism, *trees)
                validate_interruption(artifacts, run, identity)
                if mechanism == "abrupt_vm_reset":
                    receipt = parse_json(artifacts.reference(run["interruption"]))
                    require(receipt["virtual_storage_stack"] == environment["storage"], "reset storage scope differs")
                result["status"] = "complete"
            except EVIDENCE_ERRORS as error:
                result["reason"] = str(error)
            results.append(result)
    return results


EVIDENCE_ERRORS = (ValueError, LookupError, TypeError, OSError, RecursionError, ET.ParseError)


def evaluate(root, expected_candidate, expected_builds, source_roots=()):
    """Evaluate an external bundle against independently selected identities."""
    digest(expected_candidate, 40)
    require(set(expected_builds) == set(LANES), "expected builds must cover exactly the three lanes")
    for value in expected_builds.values():
        digest(value)
    root = Path(root).resolve(strict=True)
    sources = (Path(__file__).resolve().parents[2], *(Path(path).resolve() for path in source_roots))
    require(not any(root.is_relative_to(source) for source in sources), "evidence must be outside source trees")
    artifacts = Artifacts(root)
    manifest = parse_json(artifacts.read("recovery-evidence.json"))
    require(manifest.get("schema") == SCHEMA, "unsupported evidence schema")
    require(manifest.get("candidate") == expected_candidate, "stale candidate evidence")
    require(manifest.get("provenance") in ("synthetic", "native"), "unknown evidence provenance")
    lanes = manifest["lanes"]
    require(type(lanes) is list and len(lanes) <= len(LANES), "invalid lane matrix")
    selected = {}
    for lane in lanes:
        require(lane["lane"] in LANES and lane["lane"] not in selected, "unknown or duplicate lane")
        selected[lane["lane"]] = lane
    results = []
    scopes = []
    for name in LANES:
        try:
            require(name in selected, "missing lane")
            lane = selected[name]
            rows = validate_lane(artifacts, lane, expected_candidate, expected_builds[name])
            results.extend(rows)
            scopes.append({"lane": name, "environment": lane["environment"],
                           "compiler": lane["compiler"], "configuration": lane["configuration"]})
        except EVIDENCE_ERRORS as error:
            results.extend({"lane": name, "case": case, "mechanism": mechanism,
                            "status": "unevaluated", "reason": str(error)}
                           for case in CASES for mechanism in MECHANISMS)
    complete = all(row["status"] == "complete" for row in results)
    return {"schema": "luminumbra.recovery_durability_verdict.v1", "candidate": expected_candidate,
            "builds": dict(expected_builds), "provenance": manifest["provenance"],
            "status": "complete" if complete else "incomplete",
            # The validator cannot authenticate who produced a bundle, so a native
            # label is only a declaration; qualification needs owner review.
            "native_declared": complete and manifest["provenance"] == "native",
            "native_qualified": False,
            "physical_power_loss_qualified": False, "scope": "recorded virtual storage stacks only",
            "environments": scopes, "checks": results,
            "artifacts": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(artifacts.data.items())}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_dir", type=Path)
    parser.add_argument("--expected-candidate", required=True)
    parser.add_argument("--build", action="append", required=True, metavar="LANE=SHA256")
    parser.add_argument("--source-root", action="append", type=Path, default=[])
    args = parser.parse_args(argv)
    try:
        builds = {}
        for item in args.build:
            name, sha = item.split("=", 1)
            require(name not in builds, "duplicate expected build")
            builds[name] = sha
        result = evaluate(args.artifact_dir, args.expected_candidate, builds, args.source_root)
    except EVIDENCE_ERRORS as error:
        result = {"schema": "luminumbra.recovery_durability_verdict.v1", "status": "incomplete",
                  "native_declared": False, "native_qualified": False, "physical_power_loss_qualified": False, "refusal": str(error)}
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["status"] == "complete" else 1


if __name__ == "__main__":
    raise SystemExit(main())
