#!/usr/bin/env python3
"""Partition ASan tests without duplicating fixtures, then audit their exact union."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET


def require(condition, message):
    if not condition:
        raise ValueError(message)


def partition(all_tests, eligible):
    names = [test["name"] for test in all_tests]
    require(len(names) == len(set(names)), "duplicate CTest names")
    positions = {name: index for index, name in enumerate(names)}
    require(eligible and len(eligible) == len(set(eligible)) and set(eligible) <= set(names),
            "empty, duplicate or unknown eligible tests")
    groups = {name: {name} for name in eligible}
    fixtures = {}
    for test in all_tests:
        name = test["name"]
        if name not in groups:
            continue
        for prop in test.get("properties", []):
            if prop["name"] in ("FIXTURES_SETUP", "FIXTURES_CLEANUP", "FIXTURES_REQUIRED"):
                for fixture in prop["value"]:
                    if fixture in fixtures:
                        combined = groups[name] | groups[fixtures[fixture]]
                        for member in combined:
                            groups[member] = combined
                    fixtures[fixture] = name
    heavy = [name for name in names if name in groups and "SimBudgetWorldParity." in name]
    require(len(heavy) in (2, 4), "expected two or four heavy parity tests")
    owners = {name: index % 2 for index, name in enumerate(heavy)}
    assignments = [[], []]
    for name in names:
        if name not in groups:
            continue
        group = groups[name]
        forced = {owners[member] for member in group if member in owners}
        require(len(forced) <= 1, "fixture group prevents balanced heavy tests")
        owner = next(iter(forced)) if forced else (min(positions[member] for member in group) % 2)
        assignments[owner].append(name)
    require(all(assignments), "empty ASan shard")
    return assignments


def plan(directory, shard):
    all_tests = json.loads((directory / "ctest.all.json").read_text(encoding="utf-8"))["tests"]
    eligible = json.loads((directory / "ctest.inventory.json").read_text(encoding="utf-8"))["tests"]
    eligible_names = [test["name"] for test in eligible]
    # CTest 3.28 numbers -I against the roster after -LE manual, in registration
    # order. The literal regex "manual" matches that substring in any label.
    # Verify the same roster/order; refuse unexpected automatic fixture additions.
    filtered = [test["name"] for test in all_tests
                if not any(prop["name"] == "LABELS" and
                           any("manual" in label for label in prop["value"])
                           for prop in test.get("properties", []))]
    require(eligible_names == filtered, "filtered CTest roster or ordering differs from inventory")
    names = partition(all_tests, eligible_names)[shard - 1]
    positions = {name: index for index, name in enumerate(filtered, 1)}
    indices = [positions[name] for name in names]
    selection = [indices[0], indices[0], 1, *indices[1:]]
    (directory / "ctest.shard-index.txt").write_text(
        ",".join(map(str, selection)) + "\n", encoding="utf-8")


def planned_roster(directory):
    tests = json.loads((directory / "ctest.inventory.json").read_text(encoding="utf-8"))["tests"]
    names = [test["name"] for test in tests]
    require(names and len(names) == len(set(names)), "empty or duplicate CTest inventory")
    return names


def run(directory):
    directory = directory.resolve()
    planned = planned_roster(directory)
    # Share argv, cwd and environment between the last roster check and execution.
    # In particular, run this wrapper inside the workflow's env/xvfb-run context.
    command = ["ctest", "--preset", "debug-asan", "--test-dir", str(directory), "-LE", "manual"]
    discovery = subprocess.run([*command, "--show-only=json-v1"],
                               check=True, capture_output=True, text=True)
    current = [test["name"] for test in json.loads(discovery.stdout)["tests"]]
    require(current == planned, "execution CTest roster or ordering differs from planned inventory")
    # CTest -I accepts a file whose first line is Start,End,Stride,test#,... (ctest(1)).
    return subprocess.run([*command, "--no-tests=error", "-I", str(directory / "ctest.shard-index.txt"),
                           "--parallel", "2", "--timeout", "300", "--output-junit", "ctest.junit.xml"]).returncode


def merge(directory, output):
    merged = ET.Element("testsuites")
    inventory = None
    observed = set()
    heavy_counts = []
    for shard in (1, 2):
        source = directory / f"shard-{shard}"
        names = planned_roster(source)
        if inventory is None:
            inventory = names
        require(names == inventory, "shards discovered different CTest inventories")
        root = ET.parse(source / "ctest.junit.xml").getroot()
        cases = list(root.iter("testcase"))
        names = [case.get("name") for case in cases]
        require(names and all(names), f"shard {shard} ran zero tests or unnamed tests")
        require(len(names) == len(set(names)), f"shard {shard} contains duplicate tests")
        require(not observed.intersection(names), "tests ran in both shards")
        require(not any(case.get("status") not in (None, "run") or
                        case.get("result") not in (None, "completed") or
                        any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
                        for case in cases), "shard has failed or unevaluated tests")
        observed.update(names)
        heavy_counts.append(sum("SimBudgetWorldParity." in name for name in names))
        if root.tag == "testsuite":
            merged.append(root)
        else:
            require(root.tag == "testsuites", "unexpected JUnit root")
            merged.extend(root)
    require(observed == set(inventory), "executed shard union differs from the planned filtered CTest inventory")
    # Accept the current Bool pair and a second pair, but refuse an unbalanced roster.
    require(heavy_counts in ([1, 1], [2, 2]), "heavy parity tests are not evenly split")
    output.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(merged).write(output, encoding="utf-8", xml_declaration=True)
    print(f"Merged {len(observed)} tests; heavy parity tests per shard: {heavy_counts}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    sub = commands.add_parser("plan")
    sub.add_argument("--directory", type=Path, required=True)
    sub.add_argument("--shard", type=int, choices=(1, 2), required=True)
    sub = commands.add_parser("run")
    sub.add_argument("--directory", type=Path, required=True)
    sub = commands.add_parser("merge")
    sub.add_argument("--directory", type=Path, required=True)
    sub.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.mode == "plan":
            plan(args.directory, args.shard)
        elif args.mode == "run":
            return run(args.directory)
        else:
            merge(args.directory, args.output)
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError, subprocess.CalledProcessError) as error:
        print(f"ASan {args.mode} refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
