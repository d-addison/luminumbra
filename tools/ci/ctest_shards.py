#!/usr/bin/env python3
"""Plan deterministic CTest shards and validate their exact executed union."""

import argparse
import hashlib
import json
import math
import os
import re
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


PROFILES = {"asan": ("debug-asan", 2, 300), "gcc": ("release", 4, 180),
            "msvc": ("release", 4, 180), "ucrt64": ("release", 4, 360)}
RELATIONS = {"FIXTURES_SETUP", "FIXTURES_CLEANUP", "FIXTURES_REQUIRED", "DEPENDS", "RESOURCE_LOCK"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)


def inventory(document):
    require(isinstance(document, dict) and isinstance(document.get("tests"), list), "malformed CTest inventory")
    tests = document["tests"]
    require(tests, "empty CTest inventory")
    names = []
    for test in tests:
        require(isinstance(test, dict) and isinstance(test.get("name"), str) and test["name"],
                "malformed CTest test name")
        name = test["name"]
        require("_NOT_BUILT" not in name, "CTest contains _NOT_BUILT placeholder")
        names.append(name)
        command = test.get("command")
        require(isinstance(command, list) and command and command[0] and all(isinstance(item, str) for item in command)
                and not any("_NOT_BUILT" in item for item in command), "malformed or unbuilt CTest command")
        props = test.get("properties", [])
        require(isinstance(props, list), "malformed CTest properties")
        seen = set()
        for prop in props:
            require(isinstance(prop, dict) and isinstance(prop.get("name"), str)
                    and "value" in prop and prop["name"] not in seen, "malformed or duplicate CTest property")
            seen.add(prop["name"])
            if prop["name"] in RELATIONS | {"LABELS"}:
                require(isinstance(prop["value"], list) and
                        all(isinstance(value, str) and value for value in prop["value"]),
                        "malformed CTest relationship")
    require(len(names) == len(set(names)), "duplicate CTest names")
    return tests


def roster(directory):
    return inventory(read_json(directory / "ctest.inventory.json"))


def planned_roster(directory):
    return [test["name"] for test in roster(directory)]


def filtered_tests(tests):
    return [test for test in tests if not any(prop["name"] == "LABELS" and
            any("manual" in label for label in prop["value"]) for prop in test.get("properties", []))]


def validate_union(names, parts):
    require(isinstance(parts, list) and all(isinstance(part, list) and
            all(isinstance(name, str) and name for name in part) for part in parts), "malformed shard partitions")
    require(all(parts), "empty CTest shard")
    flattened = [name for part in parts for name in part]
    require(len(flattened) == len(set(flattened)), "overlapping or duplicate shard tests")
    require(set(flattened) == set(names), "partition union differs from eligible inventory")


def partition(all_tests, eligible, profile="gcc", timings=None):
    inventory({"tests": all_tests})
    names = [test["name"] for test in all_tests]
    require(eligible and len(eligible) == len(set(eligible)) and set(eligible) <= set(names),
            "empty, duplicate or unknown eligible tests")
    groups = {name: {name} for name in names}

    def join(left, right):
        combined = groups[left] | groups[right]
        for member in combined:
            groups[member] = combined

    related = {}
    for test in all_tests:
        name = test["name"]
        for prop in test.get("properties", []):
            kind = prop["name"]
            if kind not in RELATIONS or (profile == "asan" and not kind.startswith("FIXTURES_")):
                continue
            for value in prop["value"]:
                if kind == "DEPENDS":
                    require(value in groups, "unknown CTest dependency")
                    join(name, value)
                else:
                    key = ("fixture" if kind.startswith("FIXTURES_") else "lock", value)
                    if key in related:
                        join(name, related[key])
                    related[key] = name
    # The installed-runtime qualification consumes all prefab cases together.
    if profile == "gcc":
        prefab = [name for name in eligible if name.startswith(("PrefabDigest.", "PrefabRuntimeTest."))]
        for name in prefab[1:]:
            join(prefab[0], name)
    selected = set(eligible)
    for name in eligible:
        require(groups[name] <= selected, "eligible selection breaks fixture, dependency or resource lock closure")
    assignments = [[], []]
    if profile == "asan":
        # Preserve the existing ASan placement and heavy parity balance.
        heavy = [name for name in names if name in selected and "SimBudgetWorldParity." in name]
        require(len(heavy) in (2, 4), "expected two or four heavy parity tests")
        owners = {name: index % 2 for index, name in enumerate(heavy)}
        positions = {name: index for index, name in enumerate(names)}
        for name in names:
            if name not in selected:
                continue
            group = groups[name]
            forced = {owners[member] for member in group if member in owners}
            require(len(forced) <= 1, "fixture group prevents balanced heavy tests")
            owner = next(iter(forced)) if forced else min(positions[member] for member in group) % 2
            assignments[owner].append(name)
    else:
        timings = {} if timings is None else timings
        require(isinstance(timings, dict) and all(isinstance(key, str) and
                isinstance(value, (int, float)) and not isinstance(value, bool) and
                math.isfinite(value) and value > 0 for key, value in timings.items()), "invalid timings")
        # Unknown tests get at least a minute, or the largest known observation.
        unknown_weight = max([60.0, *timings.values()])
        unique = sorted({tuple(sorted(groups[name])) for name in eligible})
        loads = [0.0, 0.0]
        if timings:
            unique.sort(key=lambda group: (-sum(timings.get(name, unknown_weight) for name in group), group))
        for group in unique:
            owner = min(range(2), key=lambda index: (loads[index], index)) if timings else int(digest(group), 16) % 2
            assignments[owner].extend(group)
            loads[owner] += sum(timings.get(name, unknown_weight) for name in group)
        # CTest executes indices in registration order.
        owners = {name: owner for owner, part in enumerate(assignments) for name in part}
        assignments = [[name for name in eligible if owners[name] == owner] for owner in range(2)]
    validate_union(eligible, assignments)
    return assignments


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()).hexdigest()


def inventory_roots(directory):
    # Producers use the same checkout/build paths. Merge validates these recorded
    # roots against each other, independently of its artifact download directory.
    return {"build": str(directory.resolve()), "source": str(Path(__file__).resolve().parents[2])}


def validate_inventory_roots(roots):
    require(isinstance(roots, dict) and set(roots) == {"build", "source"}, "invalid inventory roots")
    for root in roots.values():
        require(isinstance(root, str) and
                (PurePosixPath(root).is_absolute() or PureWindowsPath(root).is_absolute()),
                "inventory root must be absolute")


def execution_inventory(tests):
    # Backtrace IDs locate declarations in a separate diagnostic graph. All
    # execution values remain literal, including paths, regexes and environment.
    # Only property registration order is immaterial; value/argument order is not.
    result = [{**{key: value for key, value in test.items() if key != "backtrace"},
               "properties": sorted(test.get("properties", []), key=lambda prop: prop["name"])} for test in tests]
    try:
        json.dumps(result, allow_nan=False)
    except (TypeError, ValueError) as error:
        raise ValueError("non-finite or invalid inventory value") from error
    return result


def evidence_for(tests, parts, profile, timings=None, roots=None):
    validate_inventory_roots(roots)
    contract = {"roots": roots, "tests": execution_inventory(tests)}
    return {"version": 3, "profile": profile, "roots": roots, "timings": timings or {}, "inventory_digest": digest(contract),
            "partition_digest": digest(parts), "partitions": parts}


def plan(directory, shard, profile="gcc", timings=None):
    require(shard in (0, 1, 2) and not (profile == "asan" and shard == 0), "invalid shard")
    all_tests = inventory(read_json(directory / "ctest.all.json"))
    tests = roster(directory)
    roots = inventory_roots(directory)
    eligible = [test["name"] for test in tests]
    require(eligible == [test["name"] for test in filtered_tests(all_tests)],
            "filtered CTest roster or ordering differs from inventory")
    require(evidence_for(tests, [eligible], profile, roots=roots) ==
            evidence_for(filtered_tests(all_tests), [eligible], profile, roots=roots),
            "discovery CTest execution inventory differs between inventories")
    parts = [eligible] if shard == 0 else partition(all_tests, eligible, profile, timings)
    manifest = {**evidence_for(tests, parts, profile, timings, roots), "shard": shard}
    names = parts[0 if shard == 0 else shard - 1]
    positions = {name: index for index, name in enumerate(eligible, 1)}
    indices = [positions[name] for name in names]
    selection = [indices[0], indices[0], 1, *indices[1:]]
    (directory / "ctest.shard-index.txt").write_text(",".join(map(str, selection)) + "\n", encoding="utf-8")
    (directory / "ctest.partition.json").write_text(json.dumps(manifest, sort_keys=True) + "\n", encoding="utf-8")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as handle:
            handle.write(f"prefab={'true' if any(name.startswith('PrefabRuntimeTest.') for name in names) else 'false'}\n")


def run(directory, profile="gcc"):
    directory = directory.resolve()
    planned = planned_roster(directory)
    preset, parallel, timeout = PROFILES[profile]
    command = ["ctest", "--preset", preset, "--test-dir", str(directory), "-LE", "manual"]
    discovery = subprocess.run([*command, "--show-only=json-v1"], check=True, capture_output=True, text=True, encoding="utf-8")
    current_tests = inventory(json.loads(discovery.stdout, object_pairs_hook=unique_object))
    current = [test["name"] for test in current_tests]
    require(current == planned, "execution CTest roster or ordering differs from planned inventory")
    manifest = read_json(directory / "ctest.partition.json")
    require(manifest["profile"] == profile, "execution profile differs from plan")
    parts = manifest["partitions"]
    validate_union(planned, parts)
    shard = manifest["shard"]
    require(shard in (0, 1, 2) and len(parts) == (1 if shard == 0 else 2), "invalid execution partition")
    roots = inventory_roots(directory)
    require(manifest == {**evidence_for(roster(directory), parts, profile, manifest["timings"], roots), "shard": shard},
            "execution partition digest differs from plan")
    require(evidence_for(current_tests, parts, profile, manifest["timings"], roots) ==
            evidence_for(roster(directory), parts, profile, manifest["timings"], roots),
            "CTest execution inventory differs from plan")
    selected = parts[0 if shard == 0 else shard - 1]
    positions = {name: index for index, name in enumerate(planned, 1)}
    indices = [positions[name] for name in selected]
    expected_index = ",".join(map(str, [indices[0], indices[0], 1, *indices[1:]]))
    require((directory / "ctest.shard-index.txt").read_text(encoding="utf-8").strip() == expected_index,
            "execution index differs from plan")
    selection = [] if manifest["shard"] == 0 else ["-I", str(directory / "ctest.shard-index.txt")]
    return subprocess.run([*command, "--no-tests=error", *selection, "--parallel", str(parallel),
                           "--timeout", str(timeout), "--output-junit", "ctest.junit.xml"]).returncode


def merge(directory, output, profile="gcc", evidence=True, full=False):
    merged = ET.Element("testsuites")
    common = None
    observed = set()
    counts = []
    reports = []
    for shard in ((0,) if full else (1, 2)):
        source = directory / f"shard-{shard}"
        tests = roster(source)
        names = [test["name"] for test in tests]
        if common is None:
            common = names
        require(names == common, "shards discovered different CTest inventories")
        if evidence:
            manifest = read_json(source / "ctest.partition.json")
            parts = manifest["partitions"]
            validate_union(names, parts)
            require(len(parts) == (1 if full else 2), "unexpected partition count")
            expected = {**evidence_for(tests, parts, profile, manifest["timings"], manifest["roots"]), "shard": shard}
            require(manifest == expected, "shard inventory or partition digest mismatch")
            if profile != "asan" and not full:
                require(parts == partition(tests, names, profile, manifest["timings"]),
                        "partition differs from deterministic placement or relationship closure")
            if reports:
                require(manifest["roots"] == reports[0][2], "shards recorded different source or build roots")
                require(parts == reports[0][0], "shards disagree on partitions")
                require(manifest["inventory_digest"] == reports[0][1], "shards discovered different CTest inventories")
            reports.append((parts, manifest["inventory_digest"], manifest["roots"]))
        root = ET.parse(source / "ctest.junit.xml").getroot()
        cases = list(root.iter("testcase"))
        executed = [case.get("name") for case in cases]
        require(executed and all(executed), f"shard {shard} ran zero tests or unnamed tests")
        require(len(executed) == len(set(executed)), f"shard {shard} contains duplicate tests")
        require(not observed.intersection(executed), "tests ran in both shards")
        require(not any(case.find(tag) is not None for case in cases for tag in ("failure", "error", "skipped")),
                "shard has failed or unevaluated tests")
        for element in root.iter():
            if element.tag in ("testsuite", "testsuites"):
                for attribute in ("failures", "errors", "skipped", "disabled"):
                    require(int(element.get(attribute, "0")) == 0, "JUnit has failed or unevaluated totals")
        require(all(case.get("status", "run") == "run" and case.get("result") != "suppressed" for case in cases),
                "JUnit has unevaluated test status")
        if evidence:
            require(set(executed) == set(parts[0 if full else shard - 1]), "report differs from assigned partition")
        observed.update(executed)
        counts.append(sum("SimBudgetWorldParity." in name for name in executed))
        require(root.tag in ("testsuite", "testsuites"), "unexpected JUnit root")
        merged.extend([root] if root.tag == "testsuite" else root)
    require(observed == set(common), "executed shard union differs from the planned filtered CTest inventory")
    if profile == "asan":
        require(counts in ([1, 1], [2, 2]), "heavy parity tests are not evenly split")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        staged = Path(temporary) / "merged.xml"
        if full:
            # Preserve the full binary-bound qualification report byte for byte.
            shutil.copyfile(directory / "shard-0/ctest.junit.xml", staged)
        else:
            ET.ElementTree(merged).write(staged, encoding="utf-8", xml_declaration=True)
        staged.replace(output)
    print(f"Merged {len(observed)} tests")


def collect(directory, prefix, attempt, full=False):
    """Select each producer's latest attempt, preserving successful older shards."""
    expected = (0,) if full else (1, 2)
    selected = {}
    pattern = re.compile(re.escape(prefix) + r"-shard-([012])-attempt-([1-9][0-9]*)")
    for path in sorted(directory.iterdir()):
        match = pattern.fullmatch(path.name)
        require(match is not None and path.is_dir(), "unexpected shard artifact")
        shard, generation = map(int, match.groups())
        require(shard in expected and generation <= attempt, "artifact route or attempt mismatch")
        if shard not in selected or generation > selected[shard][0]:
            selected[shard] = (generation, path)
    require(set(selected) == set(expected), "missing expected shard artifacts")
    require(all(not (directory / f"shard-{shard}").exists() for shard in expected), "shard destination already exists")
    # Finish all copies before exposing any selected evidence.
    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        stage = Path(temporary)
        for shard in expected:
            shutil.copytree(selected[shard][1], stage / f"shard-{shard}")
        exposed = []
        try:
            for shard in expected:
                destination = directory / f"shard-{shard}"
                (stage / f"shard-{shard}").rename(destination)
                exposed.append(destination)
        except OSError:
            for destination in exposed:
                shutil.rmtree(destination)
            raise


def gate(result, cpp, cancelled, event, draft, qualification, enabled, classification):
    require(classification == "success", "path classification did not succeed")
    require(event != "pull_request" or draft == "false", "draft or missing draft metadata")
    require(cancelled == "false", "workflow cancelled or cancellation status missing")
    if enabled == "false":
        require(result == "skipped", "unexpected producer on excluded route")
        return "skipped"
    require(result == "success", "expected producers did not all succeed")
    if cpp == "false":
        require(qualification != "true", "qualification cannot use Python-only route")
        return "python-only"
    return "full" if qualification == "true" else "sharded"


def main(default_profile="gcc", legacy_evidence=False):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    for mode in ("plan", "run", "merge"):
        sub = commands.add_parser(mode)
        sub.add_argument("--directory", type=Path, required=True)
        sub.add_argument("--profile", choices=PROFILES, default=default_profile)
        if mode == "plan":
            sub.add_argument("--shard", type=int, choices=(0, 1, 2), required=True)
            sub.add_argument("--timings", type=Path)
            sub.add_argument("--require-suite", action="append", default=[])
        if mode == "merge":
            sub.add_argument("--output", type=Path, required=True)
            sub.add_argument("--full", action="store_true")
    sub = commands.add_parser("collect")
    sub.add_argument("--directory", type=Path, required=True)
    sub.add_argument("--prefix", required=True)
    sub.add_argument("--attempt", type=int, required=True)
    sub.add_argument("--full", action="store_true")
    sub = commands.add_parser("gate")
    for key in ("result", "cpp", "cancelled", "event", "draft", "qualification", "enabled", "classification"):
        sub.add_argument("--" + key, required=True)
    args = parser.parse_args()
    try:
        if args.mode == "plan":
            names = planned_roster(args.directory)
            require(all(any(suite in name for name in names) for suite in args.require_suite), "missing expected CTest suite")
            plan(args.directory, args.shard, args.profile, read_json(args.timings) if args.timings else None)
        elif args.mode == "run":
            return run(args.directory, args.profile)
        elif args.mode == "merge":
            merge(args.directory, args.output, args.profile, evidence=not legacy_evidence, full=args.full)
        elif args.mode == "collect":
            collect(args.directory, args.prefix, args.attempt, args.full)
        else:
            route = gate(args.result, args.cpp, args.cancelled, args.event, args.draft, args.qualification, args.enabled, args.classification)
            print(f"route: {route}")
            if os.environ.get("GITHUB_STEP_SUMMARY"):
                with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as handle:
                    handle.write(f"route: {route}\n")
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError, subprocess.CalledProcessError) as error:
        print(f"CTest {args.mode} refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
